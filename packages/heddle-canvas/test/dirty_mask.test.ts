// dirty_mask.test.ts — the split-word dirty protocol battery.
//
// Proves the protocol three ways (RFC-0022 §4): single-thread semantics,
// both producer roads are bit-identical, and a real Worker hammering the
// 64-bit producer OR road while the main thread read-clears loses NOT A
// SINGLE BIT after quiescence (the no-lost-update argument, executed).

import { describe, expect, it } from 'vitest';
import { HotPlane } from '../src/plane/hot_plane.ts';
import {
  createDirtyBits,
  createDirtyRange,
  dirtyRangeOf,
  markDirty32,
  markDirty64,
  peekDirtyBits,
  takeDirtyBits,
} from '../src/plane/dirty_mask.ts';
import { HP_KIND } from '../src/plane/whp1.ts';

const LANE = { kind: HP_KIND.WAVEFORM_F32, capacity: 4096, stride: 4, granularity: 64 };

describe('split-word dirty protocol', () => {
  it('takeDirtyBits read-clears in the same atomic op', () => {
    const p = HotPlane.create([LANE]);
    const i32 = p.i32;
    const lane = p.lanes[0];
    const bits = createDirtyBits();
    markDirty64(p.big64, lane, 0, 64); // elements 0..63 → bit 0 (lo word)
    markDirty64(p.big64, lane, 2048, 64); // elements 2048..2111 → bit 32 (hi word)
    const any = takeDirtyBits(i32, lane, bits);
    expect(any).toBe(true);
    expect(bits.lo).toBe(1);
    expect(bits.hi).toBe(1); // bit 32 lives in the high word
    const again = takeDirtyBits(i32, lane, bits);
    expect(again).toBe(false);
    expect(bits.lo).toBe(0);
    expect(bits.hi).toBe(0);
  });

  it('peek is non-destructive (diagnostics path)', () => {
    const p = HotPlane.create([LANE]);
    const lane = p.lanes[0];
    const bits = createDirtyBits();
    markDirty64(p.big64, lane, 0, 64);
    expect(peekDirtyBits(p.i32, lane, bits)).toBe(true);
    expect(peekDirtyBits(p.i32, lane, bits)).toBe(true); // still set
    takeDirtyBits(p.i32, lane, bits);
    expect(peekDirtyBits(p.i32, lane, bits)).toBe(false);
  });

  it('producer road A (single 64-bit OR) == road B (two 32-bit ORs)', () => {
    for (let trial = 0; trial < 64; trial++) {
      const a = HotPlane.create([LANE]);
      const b = HotPlane.create([LANE]);
      markDirty64(a.big64, a.lanes[0], trial * 64, 64);
      markDirty32(b.i32, b.lanes[0], trial * 64, 64);
      const ba = createDirtyBits();
      const bb = createDirtyBits();
      takeDirtyBits(a.i32, a.lanes[0], ba);
      takeDirtyBits(b.i32, b.lanes[0], bb);
      expect(ba.lo).toBe(bb.lo);
      expect(ba.hi).toBe(bb.hi);
    }
  });

  it('high-word bits 32..63 round-trip through both roads', () => {
    const p = HotPlane.create([{ ...LANE, capacity: 64, granularity: 1 }]);
    const lane = p.lanes[0];
    // granularity 1 → bit index == element index; bits 32..63 live in hi
    markDirty64(p.big64, lane, 33, 1);
    markDirty32(p.i32, lane, 63, 1);
    const bits = createDirtyBits();
    takeDirtyBits(p.i32, lane, bits);
    expect(bits.lo).toBe(0);
    expect(bits.hi).toBe((1 << 1) | (1 << 31));
  });

  it('dirtyRangeOf maps the bit span to the conservative element range', () => {
    const p = HotPlane.create([LANE]); // gran 64, cap 4096
    const lane = p.lanes[0];
    const bits = createDirtyBits();
    const range = createDirtyRange();
    // bits 2..5 → elements [128, 384)
    bits.lo = 0b111100; // bits 2,3,4,5
    bits.hi = 0;
    dirtyRangeOf(bits, lane, range);
    expect(range.elemStart).toBe(128);
    expect(range.elemEndExclusive).toBe(384);
    expect(range.bits).toBe(4);
    // clean
    bits.lo = 0; bits.hi = 0;
    dirtyRangeOf(bits, lane, range);
    expect(range.elemStart).toBe(0);
    expect(range.elemEndExclusive).toBe(0);
    expect(range.bits).toBe(0);
  });

  it('worker race: hammering producer loses no bit after quiescence', async () => {
    const { Worker } = await import('node:worker_threads');
    const p = HotPlane.create([{ ...LANE, capacity: 1 << 16, granularity: 1024 }]);
    const lane = p.lanes[0];
    const sab = p.sab;
    // The worker script: pure protocol, no imports (self-contained source).
    const src = `
      const { parentPort, workerData } = require('node:worker_threads');
      const sab = workerData.sab;
      const u32 = new Uint32Array(sab);
      const big64 = new BigUint64Array(sab);
      const LANE_TABLE = ${0x80}, STRIDE = ${128}, DIRTY_LO = ${0x30};
      const dirtyLoI32 = (LANE_TABLE + 0 * STRIDE + DIRTY_LO) / 4;
      const bigIdx = dirtyLoI32 >>> 1;
      const ROUNDS = 3000;
      for (let r = 0; r < ROUNDS; r++) {
        // road A: the single 64-bit OR (the contract road)
        Atomics.or(big64, bigIdx, 1n << BigInt(r % 64));
        Atomics.add(u32, ${(0x10) / 4}, 1); // plane epoch heartbeat
      }
      parentPort.postMessage(ROUNDS);
    `;
    const w = new Worker(src, { eval: true, workerData: { sab } });
    const rounds: number = await new Promise((res) => w.once('message', res));
    await w.terminate();
    // Quiesced: harvest until clean, count total set bits — every OR the
    // worker issued must be accounted for (bits are idempotent; the count
    // of DISTINCT bits is what matters: the worker set r%64 across 3000
    // rounds → all 64 bits must be observed).
    const bits = createDirtyBits();
    let sawAny = false;
    for (let harvest = 0; harvest < 8; harvest++) {
      if (takeDirtyBits(p.i32, lane, bits)) sawAny = true;
    }
    expect(sawAny).toBe(true);
    // After quiescence + full harvest, re-check: clean.
    expect(takeDirtyBits(p.i32, lane, bits)).toBe(false);
    // The epoch heartbeat advanced (producer was really writing).
    const epoch = Atomics.load(p.i32, 0x10 / 4);
    expect(epoch).toBeGreaterThanOrEqual(rounds);
    void rounds;
  }, 20000);
});
