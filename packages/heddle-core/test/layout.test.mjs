// test/layout.test.mjs — HPL1 geometry, constants, fail-closed validation.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  HEADER_SIZE, LANE_CTRL_STRIDE, MAGIC_U32, VERSION, FLAG_LE_REQUIRED,
  HDR, align8, deriveGeometry, isPow2, validatePlane, initHeader,
  Hpl1Error, HPL1,
} from '../src/index.js';

test('align8 + geometry derivation matches the spec tables', () => {
  assert.equal(align8(0), 0);
  assert.equal(align8(1), 8);
  assert.equal(align8(4), 8);
  assert.equal(align8(8), 8);
  assert.equal(align8(129), 136);
  // 4 lanes × 8 samples (fixture geometry)
  const g4 = deriveGeometry(4, 8);
  assert.equal(g4.dirtyWords, 1);
  assert.equal(g4.laneCtrlBase, 136);          // 128 + align8(4)
  assert.equal(g4.ringBase, 392);              // align8(136 + 256)
  assert.equal(g4.totalBytes, 392 + 4 * 8 * 8); // 648
  // 1 lane × 2 (edge fixture)
  const g1 = deriveGeometry(1, 2);
  assert.equal(g1.dirtyWords, 1);
  assert.equal(g1.laneCtrlBase, 136);
  assert.equal(g1.ringBase, 200);
  assert.equal(g1.totalBytes, 216);
  // 16 lanes × 256 (demo geometry) — dirty words ceil(16/32)=1
  const g16 = deriveGeometry(16, 256);
  assert.equal(g16.dirtyWords, 1);
  assert.equal(g16.ringBase, align8(136 + 64 * 16));
  // 33 lanes ⇒ 2 dirty words, ctrl base moves
  const g33 = deriveGeometry(33, 4);
  assert.equal(g33.dirtyWords, 2);
  assert.equal(g33.laneCtrlBase, 128 + 8);
});

test('isPow2 gate', () => {
  assert.equal(isPow2(2), true);
  assert.equal(isPow2(256), true);
  assert.equal(isPow2(1), false);
  assert.equal(isPow2(3), false);
  assert.equal(isPow2(0), false);
});

function makePlane(laneCount, samplesPerLane) {
  const geo = deriveGeometry(laneCount, samplesPerLane);
  const buf = new ArrayBuffer(geo.totalBytes);
  initHeader(new DataView(buf), laneCount, samplesPerLane, 240, 1);
  return buf;
}

test('validatePlane accepts a well-formed plane and returns geometry', () => {
  const buf = makePlane(4, 8);
  const geo = validatePlane(buf);
  assert.equal(geo.laneCount, 4);
  assert.equal(geo.samplesPerLane, 8);
  assert.equal(geo.totalBytes, buf.byteLength);
  assert.equal(geo.tickHz, 240);
});

const CORRUPTIONS = [
  ['bad magic', HPL1.BAD_MAGIC, (buf, dv) => dv.setUint32(HDR.MAGIC, 0xdeadbeef, true)],
  ['bad version', HPL1.BAD_VERSION, (buf, dv) => dv.setUint32(HDR.VERSION, 2, true)],
  ['LE flag clear', HPL1.NOT_LITTLE_ENDIAN, (buf, dv) => dv.setUint32(HDR.FLAGS, 0, true)],
  ['laneCount 0', HPL1.CAPACITY_MISMATCH, (buf, dv) => dv.setUint32(HDR.LANE_COUNT, 0, true)],
  ['laneCount > MAX', HPL1.CAPACITY_MISMATCH, (buf, dv) => dv.setUint32(HDR.LANE_COUNT, 5000, true)],
  ['non-pow2 samples', HPL1.CAPACITY_MISMATCH, (buf, dv) => dv.setUint32(HDR.SAMPLES_PER_LANE, 7, true)],
  ['ringMask drift', HPL1.CAPACITY_MISMATCH, (buf, dv) => dv.setUint32(HDR.RING_MASK, 3, true)],
  ['dirtyWords drift', HPL1.CAPACITY_MISMATCH, (buf, dv) => dv.setUint32(HDR.DIRTY_WORDS, 9, true)],
  ['stride drift', HPL1.CAPACITY_MISMATCH, (buf, dv) => dv.setUint32(HDR.LANE_CTRL_STRIDE, 32, true)],
  ['ringBase drift', HPL1.CAPACITY_MISMATCH, (buf, dv) => dv.setUint32(HDR.RING_BASE, 8, true)],
  ['totalBytes drift', HPL1.CAPACITY_MISMATCH, (buf, dv) => dv.setUint32(HDR.TOTAL_BYTES, 4, true)],
];

for (const [name, code, mutate] of CORRUPTIONS) {
  test(`validatePlane fails closed: ${name}`, () => {
    const buf = makePlane(4, 8);
    assert.throws(() => {
      mutate(buf, new DataView(buf));
      validatePlane(buf);
    }, (e) => e instanceof Hpl1Error && e.code === code);
  });
}

test('validatePlane rejects undersized buffers and bad byteOffset', () => {
  assert.throws(() => validatePlane(new ArrayBuffer(64)), (e) => e.code === HPL1.PLANE_DETACHED);
  const buf = makePlane(4, 8);
  assert.throws(() => validatePlane(buf, 4), (e) => e.code === HPL1.PLANE_DETACHED); // unaligned
});

test('constants are the pinned HPL1 v1 values', () => {
  assert.equal(HEADER_SIZE, 128);
  assert.equal(LANE_CTRL_STRIDE, 64);
  assert.equal(MAGIC_U32, 0x314c5048);
  assert.equal(VERSION, 1);
  assert.equal(FLAG_LE_REQUIRED, 1);
  assert.equal(HDR.MAGIC, 0x00);
  assert.equal(HDR.TOTAL_BYTES, 0x6c);
});
