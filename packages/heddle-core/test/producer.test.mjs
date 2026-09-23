// test/producer.test.mjs — publish protocol, in-place stats, NaN rejection,
// restart semantics, and generator/producer layout agreement.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  HotPlaneProducer, HotPlaneView, makeLaneOut, HPL1, LANE_FLAG_ACTIVE,
} from '../src/index.js';
import { buildPlane, deriveGeometry } from '../../../fixtures/heddle2/generate.mjs';

const LANE_OFFSET_CURRENT = 0x08;

test('publishLane: seqlock is even at rest, odd only mid-write (single thread probe)', () => {
  const p = HotPlaneProducer.create({ laneCount: 2, samplesPerLane: 8 });
  const u32 = new Uint32Array(p.planeBuffer);
  const ctrl = deriveGeometry(2, 8).laneCtrlBase;
  // Hook: interpose on DataView setFloat64 to observe the seq word mid-publish.
  const dv = new DataView(p.planeBuffer);
  const orig = dv.setFloat64.bind(dv);
  let sawOdd = false;
  dv.setFloat64 = (off, val, le) => {
    if (off === ctrl + LANE_OFFSET_CURRENT) sawOdd = (u32[ctrl >> 2] & 1) === 1;
    orig(off, val, le);
  };
  const savedDv = p.dv; p.dv = dv;
  p.publishLane(0, 5, 1);
  p.dv = savedDv;
  assert.equal(sawOdd, true, 'seq must be odd while lane fields are being written');
  assert.equal((u32[ctrl >> 2] & 1) === 0, true, 'seq must be even after publish');
});

test('publishLane: stats update in place, min/max/avg correct over the session', () => {
  const p = HotPlaneProducer.create({ laneCount: 1, samplesPerLane: 4 });
  const v = new HotPlaneView(p.planeBuffer);
  const out = makeLaneOut();
  const series = [10, -4, 30.5, 2];
  for (let i = 0; i < series.length; i++) p.publishLane(0, series[i], 100 + i);
  assert.equal(v.readLane(0, out), HPL1.OK);
  assert.equal(out.current, 2);
  assert.equal(out.min, -4);
  assert.equal(out.max, 30.5);
  const exact = series.reduce((a, b) => a + b, 0) / 4;
  assert.ok(Math.abs(out.avg - exact) < 1e-9, `avg ${out.avg} vs exact ${exact}`);
  assert.equal(out.samplesSeenLo, 4);
});

test('running average converges to exact mean (recurrence honesty)', () => {
  const p = HotPlaneProducer.create({ laneCount: 1, samplesPerLane: 64 });
  const v = new HotPlaneView(p.planeBuffer);
  const out = makeLaneOut();
  let sum = 0;
  for (let i = 0; i < 64; i++) { const x = 1000 + i * 0.25; sum += x; p.publishLane(0, x, i); }
  v.readLane(0, out);
  assert.ok(Math.abs(out.avg - sum / 64) < 1e-6);
});

test('NaN and Infinity are rejected with HPL1_INVALID_SAMPLE — never published', () => {
  const p = HotPlaneProducer.create({ laneCount: 1, samplesPerLane: 4 });
  const v = new HotPlaneView(p.planeBuffer);
  const out = makeLaneOut();
  p.publishLane(0, 1, 1);
  assert.throws(() => p.publishLane(0, NaN, 2), (e) => e.code === HPL1.INVALID_SAMPLE);
  assert.throws(() => p.publishLane(0, Infinity, 2), (e) => e.code === HPL1.INVALID_SAMPLE);
  assert.throws(() => p.publishLane(0, -Infinity, 2), (e) => e.code === HPL1.INVALID_SAMPLE);
  assert.equal(v.readLane(0, out), HPL1.OK);
  assert.equal(out.current, 1);           // poisoned sample never landed
  assert.equal(out.samplesSeenLo, 1);
});

test('head wraps by mask and laneFlags gain ACTIVE on first publish', () => {
  const p = HotPlaneProducer.create({ laneCount: 2, samplesPerLane: 4 });
  const v = new HotPlaneView(p.planeBuffer);
  const out = makeLaneOut();
  for (let i = 0; i < 5; i++) p.publishLane(1, i, i);
  v.readLane(1, out);
  assert.equal(out.head, 5); // head is the UNMASKED next-write index (HPL1 §4)
  assert.equal(out.flags & LANE_FLAG_ACTIVE, LANE_FLAG_ACTIVE);
});

test('publishTick publishes globals under the header seqlock', () => {
  const p = HotPlaneProducer.create({ laneCount: 4, samplesPerLane: 8, tickHz: 240 });
  const v = new HotPlaneView(p.planeBuffer);
  const h = { publishSeqLo: 0, publishSeqHi: 0, epochLo: 0, epochHi: 0,
    lastPublishNsLo: 0, lastPublishNsHi: 0, framesDroppedLo: 0, framesDroppedHi: 0,
    globalMin: 0, globalMax: 0, globalAvg: 0, globalCurrent: 0, tickHz: 0, flags: 0 };
  const values = new Float64Array(4);
  for (let i = 0; i < 3; i++) {
    for (let l = 0; l < 4; l++) values[l] = (i + 1) * (l + 1);
    p.publishTick(values, 4, 1000 + i);
  }
  assert.equal(v.readHeader(h), HPL1.OK);
  assert.equal(h.globalMin, 1);        // min over currents {1,2,3,4;2,4,6,8;3,6,9,12}
  assert.equal(h.globalMax, 12);
  assert.ok(Math.abs(h.globalAvg - (1 + 2 + 3 + 4 + 2 + 4 + 6 + 8 + 3 + 6 + 9 + 12) / 12) < 1e-9);
  assert.equal(h.globalCurrent, 12);
  assert.equal(h.lastPublishNsLo, 1002);
  assert.equal(h.publishSeqLo % 2, 0, 'header seqlock even at rest');
  p.addDrops(7);
  v.readHeader(h);
  assert.equal(h.framesDroppedLo, 7);
});

test('epochRestart bumps epoch, resets session stats, keeps samplesSeen monotonic', () => {
  const p = HotPlaneProducer.create({ laneCount: 2, samplesPerLane: 8 });
  const v = new HotPlaneView(p.planeBuffer);
  const out = makeLaneOut();
  p.publishLane(0, 100, 1);
  p.publishLane(0, 200, 2);
  p.epochRestart();
  v.readLane(0, out);
  assert.equal(out.min, 0);
  assert.equal(out.max, 0);
  assert.equal(out.avg, 0);
  assert.equal(out.samplesSeenLo, 2, 'monotonic counter survives restart');
  // min/max recompute cleanly after restart
  p.publishLane(0, 5, 3);
  v.readLane(0, out);
  assert.equal(out.min, 5);
  assert.equal(out.max, 5);
});

test('producer geometry agrees with the fixture generator (layout parity)', () => {
  const gen = buildPlane(3, 4, 1234, 240, 1);
  const p = HotPlaneProducer.create({ laneCount: 3, samplesPerLane: 4 });
  assert.equal(gen.buf.byteLength, p.planeBuffer.byteLength);
  const a = deriveGeometry(3, 4);
  const b = p.geo;
  assert.equal(a.laneCtrlBase, b.laneCtrlBase);
  assert.equal(a.ringBase, b.ringBase);
  assert.equal(a.totalBytes, b.totalBytes);
});
