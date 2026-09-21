// test/plane.test.mjs — consumer-side seqlock acquire, tears, dirty mask,
// ring reads (incl. underrun), epoch change detection.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  HotPlaneProducer, HotPlaneView, makeLaneOut, makeHeaderOut,
  HPL1, LANE, HDR, deriveGeometry, LANE_FLAG_ACTIVE,
} from '../src/index.js';

function fresh(lanes = 4, samples = 8) {
  const p = HotPlaneProducer.create({ laneCount: lanes, samplesPerLane: samples, tickHz: 240 });
  return { p, v: new HotPlaneView(p.planeBuffer) };
}

test('readLane returns published stats in place (zero new objects)', () => {
  const { p, v } = fresh();
  const out = makeLaneOut();
  p.publishLane(0, 42.5, 1000);
  p.publishLane(0, 44.5, 2000);
  assert.equal(v.readLane(0, out), HPL1.OK);
  assert.equal(out.current, 44.5);
  assert.equal(out.min, 42.5);
  assert.equal(out.max, 44.5);
  assert.ok(Math.abs(out.avg - 43.5) < 1e-9);
  assert.equal(out.samplesSeenLo, 2);
  assert.equal(out.flags & LANE_FLAG_ACTIVE, LANE_FLAG_ACTIVE);
  assert.equal(out.publishNsLo, 2000);
});

test('readRecent returns newest-first ring order and wraps', () => {
  const { p, v } = fresh(1, 4); // deliberately force wrap: 6 samples in a 4-ring
  for (let i = 0; i < 6; i++) p.publishLane(0, i * 10, 1000 + i);
  const out = new Float64Array(4);
  const n = v.readRecent(0, 4, out);
  assert.equal(n, 4);
  assert.deepEqual([...out], [50, 40, 30, 20]); // newest first; 10 and 0 retired
});

test('readRecent underrun: fewer samples than requested returns available count', () => {
  const { p, v } = fresh(1, 8);
  p.publishLane(0, 1.5, 1);
  p.publishLane(0, 2.5, 2);
  const out = new Float64Array(8);
  assert.equal(v.readRecent(0, 8, out), 2);
  assert.equal(out[0], 2.5);
});

test('lane out of range is a caller error (Law 4 taxonomy code 5)', async () => {
  const { v } = fresh(2, 4);
  assert.throws(() => v.readLane(2, makeLaneOut()), (e) => e.code === HPL1.LANE_OUT_OF_RANGE);
});

test('header seqlock read + epoch change detection', () => {
  const { p, v } = fresh();
  const out = makeHeaderOut();
  assert.equal(v.readHeader(out), HPL1.OK); // first read primes the epoch cache
  p.publishTick([7, 8, 9, 10], 4, 5000);
  assert.equal(v.readHeader(out), HPL1.OK);
  assert.equal(out.globalCurrent, 10); // most recent published anywhere
  assert.ok(out.globalMin === 7 && out.globalMax === 10);
  p.epochRestart(); // producer restart
  assert.equal(v.readHeader(out), HPL1.EPOCH_CHANGED); // EXPLICIT, never silent
  assert.equal(v.readHeader(out), HPL1.OK); // subsequent reads clean
});

test('dirty mask: producer-set bits observed, consumers never write', () => {
  // 8 lanes so ticks can touch DIFFERENT lane sets → observable transitions
  const p = HotPlaneProducer.create({ laneCount: 8, samplesPerLane: 8 });
  const v = new HotPlaneView(p.planeBuffer);
  const changed = new Uint32Array(64);
  assert.equal(v.scanDirty(changed), 0); // nothing published yet
  p.publishTick([1, 2, 3, 4, 0, 0, 0, 0], 4, 10); // lanes 0..3
  let n = v.scanDirty(changed);
  assert.equal(n, 4);
  assert.deepEqual([...changed.slice(0, n)].sort(), [0, 1, 2, 3]);
  assert.equal(v.dirtyCount, 4);
  // mask is set→publish→HOLD: an identical tick produces no NEW transitions
  p.publishTick([5, 6, 7, 8, 0, 0, 0, 0], 4, 20);
  assert.equal(v.scanDirty(changed), 0);
  assert.equal(v.dirtyCount, 4);
  // tick that touches lanes 4..7 (mask cleared then re-set) → 4 new transitions
  p.publishTick([1, 1, 1, 1, 9, 10, 11, 12], 8, 30);
  n = v.scanDirty(changed);
  assert.equal(n, 4);
  assert.deepEqual([...changed.slice(0, n)].sort(), [4, 5, 6, 7]);
  // no publish → no transitions
  assert.equal(v.scanDirty(changed), 0);
});

test('torn seqlock: a concurrent writer is detected, value NOT returned', () => {
  // Simulate a writer racing the reader by flipping the seqlock mid-read.
  const { p, v } = fresh(2, 8);
  p.publishLane(0, 1, 1);
  const out = makeLaneOut();
  // corrupt to odd (write-in-progress) — reader must not return a value
  const ctrl = v.geo.laneCtrlBase;
  const u32 = new Uint32Array(p.planeBuffer);
  u32[ctrl >> 2] |= 1; // odd
  assert.equal(v.readLane(0, out), HPL1.TORN_SEQLOCK);
  assert.equal(v.tears > 0, true);
  // flip back to even → read succeeds again
  u32[ctrl >> 2] &= ~1;
  assert.equal(v.readLane(0, out), HPL1.OK);
});

test('reader works on a plain ArrayBuffer (fixture mode, non-shared)', () => {
  const { p } = fresh(2, 4);
  p.publishLane(0, 9.5, 7);
  const copy = new ArrayBuffer(p.planeBuffer.byteLength); // plain, not shared
  new Uint8Array(copy).set(new Uint8Array(p.planeBuffer));
  const v2 = new HotPlaneView(copy);
  const out = makeLaneOut();
  assert.equal(v2.readLane(0, out), HPL1.OK);
  assert.equal(out.current, 9.5);
});

test('SharedArrayBuffer path uses Atomics gates', () => {
  const { p, v } = fresh(2, 4);
  assert.equal(v.shared, true);
  p.publishLane(1, 3.25, 9);
  const out = makeLaneOut();
  assert.equal(v.readLane(1, out), HPL1.OK);
  assert.equal(out.current, 3.25);
});

test('lane ctrl block stride is pinned at 64 and layout matches spec offsets', () => {
  const { v } = fresh(4, 8);
  assert.equal(v.geo.laneCtrlBase + 1 * 64 - v.geo.laneCtrlBase, 64);
  assert.equal(LANE.CURRENT, 0x08);
  assert.equal(LANE.HEAD, 0x30);
  assert.equal(HDR.GLOBAL_MIN, 0x40);
});
