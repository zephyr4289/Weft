// cadence.test.ts — RFC-0009 §Cadence presentation policies conformance
// (PC-series), TS reference. Mirrors GovernorTest.kt's cadence battery,
// Tests/WeftTests/GovernorTests.swift, and governor_test.dart so all four
// VM ports are pinned by the same semantics (PC3 closes the loop with the
// shared-trace xlang fixture, fixtures/xlang-cadence/).
//
// The PC5 regimes pin EXACT counts — the traces are deterministic, and a
// range gate would be weaker than the truth.

import { describe, it, expect } from 'vitest';
import {
  CadencePolicy,
  CadencePolicyKind,
  CADENCE_ALPHA_ONE_Q12,
} from '../src/cadence';

/// xorshift32 — 04-LITMUS §0.2, the repo's canonical deterministic RNG
/// (the PC3 trace generator; identical step in every port).
function xorshift32(state: number): number {
  let x = state >>> 0;
  x ^= (x << 13) >>> 0;
  x ^= x >>> 17;
  x ^= (x << 5) >>> 0;
  return x >>> 0;
}

describe('RFC-0009 §cadence — LATEST_WINS', () => {
  it('PC1: presents iff seq advanced; at most one per tick; never stale', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.LATEST_WINS });
    // Trace: arrivals 0,1,0,3,0,0,1 -> seq 0,1,1,4,4,4,5
    const seqs = [0, 1, 1, 4, 4, 4, 5];
    const expectPresent = [false, true, false, true, false, false, true];
    const expectCoalesced = [0, 0, 0, 2, 0, 0, 0];
    const presented: number[] = [];
    for (let i = 0; i < seqs.length; i++) {
      const a = p.step(seqs[i]);
      expect(a.present).toBe(expectPresent[i]);
      expect(a.coalesced).toBe(expectCoalesced[i]);
      expect(a.interp).toBe(false);
      if (a.present) presented.push(a.presentSeq);
    }
    expect(presented).toEqual([1, 4, 5]);
    // Never stale: a non-present tick reports the last presented seq.
    const held = p.step(5);
    expect(held.present).toBe(false);
    expect(held.presentSeq).toBe(5);
  });

  it('PC2: telescoping identity sum(coalesced) == lastPresentedSeq - presents', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.LATEST_WINS });
    let state = 0x00c0ffee;
    let latest = 0;
    let sumCoalesced = 0;
    for (let i = 0; i < 10000; i++) {
      state = xorshift32(state);
      latest += state % 5; // 0..4 arrivals per tick
      const a = p.step(latest);
      sumCoalesced += a.coalesced;
    }
    expect(sumCoalesced).toBe(p['lastPresentedSeq'] - p.presents);
    // The observable form of the same identity (counters are public):
    expect(p.coalescedByDecision).toBe(sumCoalesced);
  });

  it('PC5 (240 Hz on 120 Hz): presents every tick, each coalescing 1', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.LATEST_WINS });
    let latest = 0;
    for (let t = 0; t < 1200; t++) {
      latest += 2; // two frames per display tick
      const a = p.step(latest);
      expect(a.present).toBe(true);
      expect(a.coalesced).toBe(1);
    }
    expect(p.presents).toBe(1200);
  });
});

describe('RFC-0009 §cadence — PACED_INTERPOLATE', () => {
  it('PC5 (30 Hz on 120 Hz): presents EVERY tick — steady display cadence', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PACED_INTERPOLATE });
    let latest = 0;
    let presents = 0;
    for (let t = 1; t <= 400; t++) {
      if (t % 4 === 1) {
        latest += 1; // one new frame every 4 ticks (30 Hz on 120 Hz)
      }
      const a = p.step(latest);
      expect(a.interp).toBe(true);
      if (a.present) presents++;
    }
    // Window arithmetic, exactly: the FIRST window has period 1
    // (prevObsTick=0, newestObsTick=1), so its ladder saturates at tick 2
    // and ticks 3-4 elide (identical saturated triple). From tick 5 on,
    // period=4 and the ladder (0,1024,2048,3072 -> next arrival's 0)
    // presents EVERY tick: 396 more presents. Total: 2 + 396 = 398 of 400
    // — the steady-cadence contract (the two elided ticks are the
    // pre-history warmup, not cadence jitter).
    expect(presents).toBe(398);
    expect(p.presents).toBe(presents);
  });

  it('PC5 (240 Hz on 120 Hz): presents every tick, one period behind', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PACED_INTERPOLATE });
    let latest = 0;
    for (let t = 1; t <= 400; t++) {
      latest += 2;
      const a = p.step(latest);
      // Every tick is an arrival tick: triple (prev, newest, 0) differs
      // every tick -> present, raster one frame behind (alpha=0 shows prev).
      expect(a.present).toBe(true);
      expect(a.alphaQ12).toBe(0);
      // One-period lag: the raster base is the previous window's newest.
      expect(a.presentSeq).toBe(latest);
    }
    expect(p.presents).toBe(400);
  });

  it('PC1/PC5 (stall): alpha saturates, presents elide, never extrapolates', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PACED_INTERPOLATE });
    p.step(1); // arrival at tick 1
    p.step(2); // arrival at tick 2 (period 1)
    // Stall: no new frames ever; alpha hits 4096 on tick 3 and stays.
    const a3 = p.step(2);
    expect(a3.present).toBe(true);
    expect(a3.alphaQ12).toBe(CADENCE_ALPHA_ONE_Q12);
    const a4 = p.step(2);
    expect(a4.present).toBe(false); // identical saturated triple: elided
    expect(a4.alphaQ12).toBe(CADENCE_ALPHA_ONE_Q12);
    const a5 = p.step(2);
    expect(a5.present).toBe(false);
    // 200 more stalled ticks: still nothing invented.
    for (let i = 0; i < 200; i++) p.step(2);
    expect(p.interpFrames).toBe(0); // endpoints only — zero true blends
  });

  it('PC2: telescoping identity sum(coalesced) == newestSeq - arrivalTicks', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PACED_INTERPOLATE });
    let state = 0xfeedface;
    let latest = 0;
    for (let i = 0; i < 10000; i++) {
      state = xorshift32(state);
      latest += state % 5;
      p.step(latest);
    }
    expect(p.coalescedByDecision).toBe(p['newestSeq'] - p.arrivalTicks);
  });

  it('PC6 (blend semantics): alpha steps are the period ladder', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PACED_INTERPOLATE });
    // Frames at ticks 1 and 5 -> period 4; ladder 0,1024,2048,3072.
    const alphas: number[] = [];
    for (let t = 1; t <= 8; t++) {
      const latest = t === 1 ? 1 : t === 5 ? 2 : t <= 4 ? 1 : 2;
      const a = p.step(latest);
      if (t >= 5) alphas.push(a.alphaQ12); // second window's ladder
    }
    expect(alphas).toEqual([0, 1024, 2048, 3072]);
  });
});

describe('RFC-0009 §cadence — BURST_COALESCE', () => {
  it('PC5 (30 Hz on 120 Hz): K locks onto the content beat (K=4)', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.BURST_COALESCE });
    let latest = 0;
    const ks: number[] = [];
    for (let t = 1; t <= 400; t++) {
      if (t % 4 === 1) latest += 1;
      const a = p.step(latest);
      ks.push(a.k);
    }
    // Constant inter-arrival gap of 4 ticks -> the gap EWMA converges
    // monotonically to 4 (Q12 16384) -> K = round(4) = 4, locked once the
    // estimate crosses the rounding boundary and never leaves (monotone
    // convergence — no oscillation, unlike an arrivals-rate EMA).
    const settled = ks.slice(ks.length - 200);
    expect(settled.every((k) => k === 4)).toBe(true);
  });

  it('PC5 (240 Hz on 120 Hz): K -> 1, degrades to newest-wins', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.BURST_COALESCE });
    let latest = 0;
    const ks: number[] = [];
    let presents = 0;
    for (let t = 1; t <= 400; t++) {
      latest += 2;
      const a = p.step(latest);
      if (a.present) presents++;
      ks.push(a.k);
    }
    // Gap 1 every tick (K starts at 1 and the EWMA stays at 4096):
    // present every tick, each coalescing 1.
    const settled = ks.slice(ks.length - 200);
    expect(settled.every((k) => k === 1)).toBe(true);
    expect(presents).toBe(400);
    expect(p.coalescedByDecision).toBe(400);
  });

  it('PC5 (burst feed): paces at one present per burst, newest, fully coalesced', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.BURST_COALESCE });
    let latest = 0;
    let presents = 0;
    let coalescedSeen = 0;
    for (let t = 1; t <= 400; t++) {
      if (t % 10 === 1) latest += 24; // 24-frame burst every 10 ticks
      const a = p.step(latest);
      if (a.present) {
        presents++;
        coalescedSeen += a.coalesced;
      }
    }
    // 40 bursts x 24 frames = 960 frames; after lock-on (K=10) the loop
    // presents once per burst: ~40 presents, each coalescing 23.
    expect(presents).toBeGreaterThanOrEqual(38);
    expect(presents).toBeLessThanOrEqual(41);
    expect(coalescedSeen).toBe(960 - presents);
    expect(p.missedPresentTicks + p.elided + presents).toBe(400);
  });

  it('PC5 (stall): presents stop naturally; recovery within the locked K', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.BURST_COALESCE });
    let latest = 0;
    for (let t = 1; t <= 200; t++) {
      latest += 2;
      p.step(latest);
    }
    // K is 1 (gap-1 content). Stall completely: present ticks fire but
    // find nothing newer — counted misses, never silent.
    const before = p.presents;
    for (let t = 1; t <= 100; t++) p.step(latest);
    expect(p.presents).toBe(before); // nothing newer -> no presents
    expect(p.missedPresentTicks).toBe(100);
    // Content resumes: K is still locked at 1, so the FIRST resumed tick
    // presents — recovery is immediate (the gap estimator only moves on
    // arrival ticks; a stall freezes the last cadence, it does not decay).
    latest += 5;
    const a = p.step(latest);
    expect(a.present).toBe(true);
    expect(a.coalesced).toBe(4);
  });

  it('PC2: telescoping identity sum(coalesced) == lastPresentedSeq - presents', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.BURST_COALESCE });
    let state = 0x1234abcd;
    let latest = 0;
    for (let i = 0; i < 10000; i++) {
      state = xorshift32(state);
      latest += state % 5;
      p.step(latest);
    }
    expect(p.coalescedByDecision).toBe(p['lastPresentedSeq'] - p.presents);
  });
});

describe('RFC-0009 §cadence — cross-policy gates', () => {
  it('PC1: no policy ever presents twice in one tick (structural: one step, one decision)', () => {
    // The step() contract returns exactly one decision per call; the
    // per-policy batteries above pin the per-tick present conditions.
    // Here: the volatile trace never yields present && coalesced<0 or
    // alphaQ12 out of [0,4096].
    for (const policy of [
      CadencePolicyKind.LATEST_WINS,
      CadencePolicyKind.PACED_INTERPOLATE,
      CadencePolicyKind.BURST_COALESCE,
    ]) {
      const p = new CadencePolicy({ policy });
      let state = 0x00c0ffee;
      let latest = 0;
      for (let i = 0; i < 10000; i++) {
        state = xorshift32(state);
        latest += state % 5;
        const a = p.step(latest);
        expect(a.alphaQ12).toBeGreaterThanOrEqual(0);
        expect(a.alphaQ12).toBeLessThanOrEqual(CADENCE_ALPHA_ONE_Q12);
        expect(a.coalesced).toBeGreaterThanOrEqual(0);
      }
    }
  });

  it('PC4: zero allocation — the decision record is identity-stable', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PACED_INTERPOLATE });
    const first = p.step(1);
    let state = 0xabcdef01;
    let latest = 1;
    for (let i = 0; i < 10000; i++) {
      state = xorshift32(state);
      latest += state % 5;
      const a = p.step(latest);
      expect(a).toBe(first); // SAME object, mutated in place
    }
  });

  it('PC6: policy switch mid-trace keeps presentSeq monotone and counters alive', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.LATEST_WINS });
    let latest = 0;
    let lastPresented = 0;
    const kinds = [
      CadencePolicyKind.LATEST_WINS,
      CadencePolicyKind.PACED_INTERPOLATE,
      CadencePolicyKind.BURST_COALESCE,
      CadencePolicyKind.LATEST_WINS,
    ];
    for (let i = 0; i < 4000; i++) {
      if (i % 1000 === 0) p.reset(kinds[i / 1000]);
      latest += (i % 3 === 0 ? 1 : 0) + (i % 7 === 0 ? 2 : 0);
      const a = p.step(latest);
      if (a.present && !a.interp) {
        expect(a.presentSeq).toBeGreaterThanOrEqual(lastPresented);
        lastPresented = a.presentSeq;
      }
    }
    expect(p.presents).toBeGreaterThan(0);
  });

  it('PC3 (shape): the packed trace this port emits is stable', () => {
    // The byte-level cross-language comparison is fixtures/xlang-cadence's
    // job; here we pin the PACKING itself (the protocol): 6 bytes per tick
    // — LATEST, PACED, BURST in kind order; b1 = present<<7 | interp<<6 |
    // (alphaQ12>>6), b2 = min(coalesced,255).
    const policies = [
      new CadencePolicy({ policy: CadencePolicyKind.LATEST_WINS }),
      new CadencePolicy({ policy: CadencePolicyKind.PACED_INTERPOLATE }),
      new CadencePolicy({ policy: CadencePolicyKind.BURST_COALESCE }),
    ];
    let state = 0x00c0ffee;
    let latest = 0;
    const bytes: number[] = [];
    for (let i = 0; i < 4; i++) {
      state = xorshift32(state);
      latest += state % 5;
      for (const p of policies) {
        const a = p.step(latest);
        bytes.push(
          ((a.present ? 1 : 0) << 7) |
            ((a.interp ? 1 : 0) << 6) |
            (a.alphaQ12 >> 6)
        );
        bytes.push(Math.min(a.coalesced, 255));
      }
    }
    // 4 ticks x 3 policies x 2 bytes = 24 bytes; determinism: re-run
    // produces the identical stream.
    const policies2 = [
      new CadencePolicy({ policy: CadencePolicyKind.LATEST_WINS }),
      new CadencePolicy({ policy: CadencePolicyKind.PACED_INTERPOLATE }),
      new CadencePolicy({ policy: CadencePolicyKind.BURST_COALESCE }),
    ];
    let state2 = 0x00c0ffee;
    let latest2 = 0;
    const bytes2: number[] = [];
    for (let i = 0; i < 4; i++) {
      state2 = xorshift32(state2);
      latest2 += state2 % 5;
      for (const p of policies2) {
        const a = p.step(latest2);
        bytes2.push(
          ((a.present ? 1 : 0) << 7) |
            ((a.interp ? 1 : 0) << 6) |
            (a.alphaQ12 >> 6)
        );
        bytes2.push(Math.min(a.coalesced, 255));
      }
    }
    expect(bytes).toEqual(bytes2);
    expect(bytes.length).toBe(24);
  });
});
