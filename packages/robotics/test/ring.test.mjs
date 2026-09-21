// test/ring.test.mjs — RNG1 reader: header fail-closed, seqlock protocol,
// topic filtering, fixture parity with the frozen rng1-ring.bin.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  attachRing, Rng1Error, FMT_IMU6DOF, FMT_POINTS_F32, FMT_FRAME_DESC,
  FMT_BOXES_F32,
} from '../src/index.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const FIXTURES = join(HERE, '..', '..', '..', 'tests', 'adapters', 'managed', 'fixtures');

function buildRing(records, { slotSize = 4096, slotCount = 8, topics = [] } = {}) {
  const total = 128 + slotSize * slotCount;
  const buf = new ArrayBuffer(total);
  const dv = new DataView(buf);
  const u8 = new Uint8Array(buf);
  u8.set([0x52, 0x4e, 0x47, 0x31], 0); // "RNG1"
  dv.setUint16(4, 1, true);
  dv.setUint16(6, 128, true);
  dv.setUint32(8, slotSize, true);
  dv.setUint32(12, slotCount, true);
  topics.forEach((t, i) => dv.setUint32(64 + i * 4, t, true));
  let seq = 0;
  for (const [topic, fmt, payload, torn] of records) {
    seq++;
    const slot = (seq - 1) % slotCount;
    const base = 128 + slot * slotSize;
    if (64 + payload.length > slotSize) throw new Error('payload too big');
    dv.setBigUint64(base, BigInt(torn ? seq | 1 : seq), true);
    dv.setUint32(base + 8, payload.length, true);
    dv.setUint32(base + 12, topic, true);
    dv.setBigUint64(base + 16, BigInt(1000 + seq), true);
    dv.setUint32(base + 24, fmt, true);
    u8.set(payload, base + 64);
  }
  dv.setBigUint64(16, BigInt(seq), true);  // write_seq
  dv.setBigUint64(24, BigInt(seq), true);  // committed
  return buf;
}

const imuPayload = (ts = 111) => {
  const b = new Float64Array(8);
  b[0] = ts; b[1] = 0.7071; b[3] = 0.7071; b[5] = 0.01;
  return new Uint8Array(b.buffer);
};

const pointsPayload = (n = 6) => {
  const b = new Float32Array(n * 3);
  for (let i = 0; i < n; i++) {
    b[i * 3] = i; b[i * 3 + 1] = 0.5; b[i * 3 + 2] = -0.25;
  }
  return new Uint8Array(b.buffer);
};

test('header fail-closed: short / magic / version / geometry', () => {
  assert.throws(() => attachRing(new ArrayBuffer(64)), (e) => e.code === 1);
  const bad = buildRing([]);
  new DataView(bad).setUint32(0, 0xdeadbeef, true);
  assert.throws(() => attachRing(bad), (e) => e.code === 2);
  const ver = buildRing([]);
  new DataView(ver).setUint16(4, 2, true);
  assert.throws(() => attachRing(ver), (e) => e.code === 3);
  const geom = buildRing([], { slotCount: 3 });
  assert.throws(() => attachRing(geom), (e) => e.code === 5);
});

test('acquire newest-wins with exact field decode', () => {
  const buf = buildRing([
    [1, FMT_IMU6DOF, imuPayload(111), false],
    [1, FMT_IMU6DOF, imuPayload(222), false],
  ], { topics: [1] });
  const r = attachRing(buf);
  const rec = r.acquire(1);
  assert.equal(rec.fmt, FMT_IMU6DOF);
  const out = rec.imuInto(new Float64Array(8));
  assert.equal(out[0], 222);
  assert.equal(r.committedSeq(), 2);
  assert.equal(r.acquire(1), null); // consumed
});

test('torn record counted, never thrown (drop-not-block)', () => {
  const buf = buildRing([
    [1, FMT_POINTS_F32, pointsPayload(4), false],
    [1, FMT_POINTS_F32, pointsPayload(4), true], // torn at even position
  ], { topics: [1] });
  const r = attachRing(buf);
  assert.equal(r.acquire(), null);
  assert.equal(r.stats[1], 1);
  assert.equal(r.stats[0], 1);
});

test('topic filter counts filtered', () => {
  const buf = buildRing([[2, FMT_IMU6DOF, imuPayload(), false]],
    { topics: [1, 2] });
  const r = attachRing(buf);
  assert.equal(r.acquire(1), null);
  assert.equal(r.stats[3], 1);
  assert.deepEqual(r.topicTable(), [1, 2]);
});

test('points window is a zero-copy view over ring bytes', () => {
  const buf = buildRing([[1, FMT_POINTS_F32, pointsPayload(6), false]],
    { topics: [1] });
  const r = attachRing(buf);
  let seen = 0;
  r.drain((v) => {
    seen++;
    const f32 = v.pointsView();
    assert.equal(f32.length, 18);
    assert.equal(f32[0], 0);
    assert.equal(f32[3], 1);
    // window shares the ring's memory: mutate ring -> view sees it
    // (slot 0 payload starts at 128 + 64 = 192)
    new DataView(buf).setFloat32(192, 42.5, true);
    assert.equal(f32[0], 42.5);
  });
  assert.equal(seen, 1);
});

test('frm1 descriptor decode (fail-closed)', () => {
  const p = new ArrayBuffer(32);
  const dv = new DataView(p);
  new Uint8Array(p).set([0x46, 0x52, 0x4d, 0x31]); // "FRM1"
  dv.setUint32(4, 3840, true);
  dv.setUint32(8, 2160, true);
  dv.setUint32(12, 11520, true);
  dv.setUint32(16, 1, true);
  dv.setUint32(20, 42, true);
  dv.setUint32(24, 7, true);
  const buf = buildRing([[3, FMT_FRAME_DESC, new Uint8Array(p), false]],
    { topics: [3] });
  const r = attachRing(buf);
  r.drain((v) => {
    const out = v.frm1Into(r.frm1Out);
    assert.equal(out.valid, true);
    assert.equal(out.width, 3840);
    assert.equal(out.height, 2160);
    assert.equal(out.handleLo, 7);
  });
});

test('boxes window decode', () => {
  const b = new Float32Array(2 * 6);
  b[0] = 0; b[6] = 10;
  const buf = buildRing([[4, FMT_BOXES_F32, new Uint8Array(b.buffer), false]],
    { topics: [4] });
  const r = attachRing(buf);
  r.drain((v) => {
    const f32 = v.boxesView();
    assert.equal(f32.length, 12);
    assert.equal(f32[6], 10);
  });
});

test('FROZEN fixture parity: rng1-ring.bin (10 records, 1 torn)', () => {
  const bin = readFileSync(join(FIXTURES, 'rng1-ring.bin'));
  const r = attachRing(bin.buffer.slice(bin.byteOffset, bin.byteOffset + bin.byteLength));
  assert.equal(r.slotSize, 4096);
  assert.equal(r.slotCount, 8);
  assert.equal(r.committedSeq(), 10);
  assert.deepEqual(r.topicTable(), [1, 2, 3, 4]);
  const seen = [];
  r.drain((v) => seen.push([v.seq, v.topicId, v.fmt, v.payloadLen]));
  // 10 committed - 1 torn (seq 4) - 2 overwritten (seqs 1,2 wrap to slots
  // now holding 9,10) = 7 continuous records — matches overwrite_count=2
  assert.equal(seen.length, 7);
  assert.deepEqual(seen[0], [3, 1, FMT_IMU6DOF, 64]);
  assert.deepEqual(seen[6], [10, 1, FMT_IMU6DOF, 64]);
  assert.equal(r.stats[1], 3);  // 1 torn + 2 overwritten windows
  assert.equal(r.headerDropCount(), 0);
});
