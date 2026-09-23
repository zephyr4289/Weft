// test/fixture.test.mjs — byte-exact validation of the committed golden
// fixtures against docs/weft-tensor/LAYOUT-V1.md §7 (Law 2 cross-language
// parity anchor: the SAME bytes the Python suite asserts).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

import { WeftTensorRing, SLOT_HEADER_SIZE } from '../src/index.js';

const FIX = new URL('../../../fixtures/weft-tensor/', import.meta.url);
const f32Bin = readFileSync(new URL('ring-v1-f32.bin', FIX));
const u8Bin = readFileSync(new URL('ring-v1-u8.bin', FIX));

test('f32 fixture: header fields, 10 frames, dyadic payload parity', () => {
  const ring = WeftTensorRing.attach(f32Bin.buffer.slice(f32Bin.byteOffset, f32Bin.byteOffset + f32Bin.byteLength));
  assert.equal(ring.producerSeq, 10);
  assert.equal(ring.slotCount, 4);
  assert.equal(ring.slotStride, 128);
  assert.equal(ring.payloadCap, 64);
  assert.equal(ring.layout.tickHz, 120);

  // 10 frames through 4 slots: only the last 4 (seq 7..10) survive the wrap —
  // reading them back exactly IS the parity contract. Earlier seqs MUST be gone.
  for (let seq = 1; seq <= 6; seq++) {
    assert.equal(ring.acquireFrame(seq), null, `seq ${seq} lapped by ring wrap`);
  }
  for (let seq = 7; seq <= 10; seq++) {
    const v = ring.acquireFrame(seq);
    assert.notEqual(v, null, `frame ${seq}`);
    assert.equal(v.seq, seq);
    assert.equal(v.timestampNs, seq * 1_000_000);
    assert.equal(v.durationUs, 8333);
    assert.equal(v.fourcc, 'F32 ');
    for (let i = 0; i < 6; i++) {
      assert.equal(v.getF32(i), (seq * 10 + i) * 0.25, `frame ${seq} elem ${i}`);
    }
  }
  // Slot headers round-trip: seq 9 lives in slot (9-1)%4 = 0.
  const lastSlot = 0;
  const base = ring.layout.headerSize + lastSlot * ring.slotStride;
  const dv = new DataView(ring.buffer, base, SLOT_HEADER_SIZE);
  assert.equal(dv.getUint32(8, true), 9, 'slot header seq LE');
});

test('u8 fixture: header fields, 6 frames, byte payload parity', () => {
  const ring = WeftTensorRing.attach(u8Bin.buffer.slice(u8Bin.byteOffset, u8Bin.byteOffset + u8Bin.byteLength));
  assert.equal(ring.producerSeq, 6);
  const L = ring.layout;
  assert.deepEqual([L.dtype.code, L.dtype.bits], [1, 8]); // kDLUInt / 8
  assert.deepEqual(Array.from(L.shape.slice(0, 3)), [2, 2, 4]);
  for (let seq = 1; seq <= 2; seq++) {
    assert.equal(ring.acquireFrame(seq), null, `seq ${seq} lapped by ring wrap`);
  }
  for (let seq = 3; seq <= 6; seq++) {
    const v = ring.acquireFrame(seq);
    assert.notEqual(v, null);
    assert.equal(v.fourcc, 'RAW ');
    for (let i = 0; i < 16; i++) {
      assert.equal(v.getU8(i), (seq * 37 + i * 11) & 0xff, `frame ${seq} byte ${i}`);
    }
  }
});
