// oracle.test.ts — the reference decimation math. This is the equation
// the WGSL compute pass, the GLSL TF pass, the Canvas2D raster and the
// native Vulkan probe must ALL reproduce bit-exactly (the cross-tier
// ==-gate's ground truth).

import { describe, expect, it } from 'vitest';
import { decimateHash, decimateWindowMinMax, fnv1a32 } from '../src/renderers/cpu_oracle.ts';
import { HotPlane } from '../src/plane/hot_plane.ts';
import { SynthProducer, synthSample } from '../src/plane/synth.ts';
import { HP_KIND } from '../src/plane/whp1.ts';

describe('reference window walk (cpu_oracle)', () => {
  it('linear window (writePos < capacity): slot(j) == j', () => {
    const f32 = new Float32Array(64);
    for (let i = 0; i < 64; i++) f32[i] = i;
    const out = new Float32Array(8 * 2);
    decimateWindowMinMax(f32, 64, 32, 32, 8, out);
    // windowStart 32, vis 32 → oldest slot (32-32+0)=0 → slots 0..31
    // columns of 4: [0..4) min 0 max 3, [4..8) min 4 max 7, ...
    expect(out[0]).toBe(0);
    expect(out[1]).toBe(3);
    expect(out[2]).toBe(4);
    expect(out[3]).toBe(7);
    expect(out[14]).toBe(28);
    expect(out[15]).toBe(31);
  });

  it('wrapped window: newest sample is one slot before windowStart', () => {
    const cap = 16;
    const f32 = new Float32Array(cap);
    for (let i = 0; i < cap; i++) f32[i] = i;
    // writePos 20 → windowStart 4, vis 16 → oldest slot (4-16+16)%16=4,
    // age order walks 4,5,...,15,0,1,2,3 — the NEWEST (j=15) is slot 3.
    const out = new Float32Array(2 * 2);
    decimateWindowMinMax(f32, cap, cap, 20 % cap, 2, out);
    // col 0: j 0..8 → slots 4..11 → min 4 max 11
    expect(out[0]).toBe(4);
    expect(out[1]).toBe(11);
    // col 1: j 8..16 → slots 12..15,0..3 → min 0 max 15
    expect(out[2]).toBe(0);
    expect(out[3]).toBe(15);
  });

  it('tail columns beyond the window emit the (0,0) sentinel', () => {
    const f32 = new Float32Array(64);
    for (let i = 0; i < 64; i++) f32[i] = 1 + (i % 7);
    const out = new Float32Array(16 * 2);
    decimateWindowMinMax(f32, 64, 4, 4, 16, out); // 4 samples, 16 cols
    expect(out[0]).not.toBe(0); // col 0 covers the data
    expect(out[8]).toBe(0); // col 4+ sentinel
    expect(out[9]).toBe(0);
  });

  it('decimateHash is deterministic for identical plane state', () => {
    const a = HotPlane.create([{ kind: HP_KIND.WAVEFORM_F32, capacity: 4096, stride: 4, granularity: 64 }]);
    const b = HotPlane.create([{ kind: HP_KIND.WAVEFORM_F32, capacity: 4096, stride: 4, granularity: 64 }]);
    const sa = new SynthProducer(a);
    const sb = new SynthProducer(b);
    sa.pumpWaveform(4000, 1.75);
    sb.pumpWaveform(4000, 1.75);
    const scratchA = new Float32Array(128 * 2);
    const scratchB = new Float32Array(128 * 2);
    const ha = decimateHash(a.lanes[0].f32, 4096, 4000, 4000 % 4096, 128, scratchA);
    const hb = decimateHash(b.lanes[0].f32, 4096, 4000, 4000 % 4096, 128, scratchB);
    expect(ha).toBe(hb);
    // and it CHANGES when the window moves (the hash is load-bearing)
    sa.pumpWaveform(200, 1.9);
    const ha2 = decimateHash(a.lanes[0].f32, 4096, 4200, 4200 % 4096, 128, scratchA);
    expect(ha2).not.toBe(ha);
  });

  it('hash covers min/max f32 BITS (not display strings)', () => {
    const words = new Uint32Array([0x3f800000, 0x40000000]); // 1.0, 2.0
    expect(fnv1a32(words)).toBe(fnv1a32(new Uint32Array([0x3f800000, 0x40000000])));
    // one LSB difference in a f32 bit pattern changes the hash
    const variant = fnv1a32(new Uint32Array([0x3f800001, 0x40000000]));
    expect(variant).not.toBe(fnv1a32(words));
  });

  it('1M-point window decimates within the frame budget envelope (sanity)', () => {
    // The mandate's 1,000,000-point claim: one full-window decimation on
    // the CPU oracle must complete in a sane time (the GPU passes do it
    // faster; this pins the oracle's own cost so the bench numbers have
    // a reference point).
    const cap = 1 << 20;
    const f32 = new Float32Array(cap);
    for (let i = 0; i < cap; i++) f32[i] = synthSample(i, 0.5);
    const out = new Float32Array(640 * 2);
    const t0 = process.hrtime.bigint();
    decimateWindowMinMax(f32, cap, cap, 0, 640, out);
    const micros = Number(process.hrtime.bigint() - t0) / 1000;
    // CPU oracle: ~1M compares — expect single-digit milliseconds.
    // NOT the frame path (that's the GPU's job); just the oracle sanity.
    expect(micros).toBeLessThan(50_000);
    expect(out[1]).toBeGreaterThan(out[0]); // a real min/max pair
  });
});
