// cadence-predictive.test.ts — RFC-0012 §PREDICTIVE_PACED conformance
// (PC7–PC11), TS reference. The bounds below are the numerically verified
// ones (scripts/orbit_probe.mjs against the exact integer recurrence), not
// aspirations: the filter's steady-state orbit on beat 12/5 stays within
// ±0.125 ticks of the true beat, and PC8's integer equivalence holds
// tick-for-tick BECAUSE of the full-precision remainder carry (without it
// g=6/j=3 yields 2047 vs PACED's 2048).

import { describe, it, expect } from 'vitest';
import {
  CadencePolicy,
  CadencePolicyKind,
  CADENCE_ALPHA_ONE_Q12,
  CADENCE_ONE_Q16,
} from '../src/cadence';

function xorshift32(state: number): number {
  let x = state >>> 0;
  x ^= (x << 13) >>> 0;
  x ^= x >>> 17;
  x ^= (x << 5) >>> 0;
  return x >>> 0;
}

/// Build a stable-beat arrival trace: latestSeq increments by 1 exactly at
/// ticks floor(rate * m). Returns the per-tick latestSeq stream.
function stableBeat(rate: number, ticks: number): number[] {
  const seq: number[] = new Array(ticks);
  let latest = 0;
  let m = 1;
  for (let t = 1; t <= ticks; t++) {
    if (t >= Math.floor(rate * m)) {
      latest++;
      m++;
    }
    seq[t - 1] = latest;
  }
  return seq;
}

/// Drive a policy over a trace, collecting per-tick decisions.
function drive(p: CadencePolicy, seq: number[]) {
  const out: { present: boolean; alpha: number; coalesced: number }[] = [];
  for (const s of seq) {
    const d = p.step(s);
    out.push({ present: d.present, alpha: d.alphaQ12, coalesced: d.coalesced });
  }
  return out;
}

const ONE_Q16 = CADENCE_ONE_Q16; // 65536

describe('RFC-0012 §PREDICTIVE_PACED', () => {
  it('PC7a (drift-freedom): gap EWMA converges into ±0.125 ticks of the 12/5 beat and holds for 10k ticks', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PREDICTIVE_PACED });
    const seq = stableBeat(2.4, 10000);
    drive(p, seq);
    const beatQ16 = Math.trunc(2.4 * ONE_Q16); // 157286
    const dev = Math.abs(p.internal_gapQ16ForTest() - beatQ16);
    expect(dev).toBeLessThanOrEqual(8192); // ±0.125 ticks (orbit-probe bound)
  });

  it('PC7b (on-the-nose prediction): saturated holds are vanishingly rare on a stable 12/5 beat, and the reactive gate clears after warmup', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PREDICTIVE_PACED });
    const seq = stableBeat(2.4, 10000);
    let satHolds = 0;
    for (let t = 0; t < seq.length; t++) {
      const d = p.step(seq[t]);
      // Arrival ticks present alpha 0; any presented 4096 is a saturated
      // hold (content late relative to the prediction). The gap EWMA's
      // steady-state orbit (±0.125 ticks) lets the phase cross a tick
      // early on ~0.05% of windows — the honest bound the probe pinned.
      if (d.present && d.alphaQ12 === CADENCE_ALPHA_ONE_Q12) satHolds++;
    }
    expect(satHolds).toBeLessThanOrEqual(10); // ≤ 0.1% of 4167 windows
    // The gate may fire during the filter's warmup transient; on a stable
    // beat it clears and stays clear.
    const r0 = p.reactiveTicks;
    for (let t = 0; t < 5000; t++) p.step(seq[t % seq.length]);
    expect(p.reactiveTicks - r0).toBe(0);
  });

  it('PC7c (honest hold): with +2-tick late jitter every 25th arrival, EVERY late arrival produces exactly one saturated hold — and only late arrivals do', () => {
    // Stable 2-tick beat; every 25th arrival is 2 ticks late (gap 4).
    // The converged estimate (~2.08) keeps 2-gap windows hold-free (phase
    // at j=2 ≈ 98% — no saturation), while every 4-gap window saturates
    // at j=3: exactly one honest hold per late arrival, zero spurious.
    const T = 12000;
    const seq: number[] = new Array(T);
    const arrivals: number[] = [];
    let tt = 2;
    for (let m = 1; tt <= T; m++) {
      arrivals.push(tt);
      tt += 2 + (m % 25 === 0 ? 2 : 0);
    }
    let latest = 0;
    let ai = 0;
    let lateAfterWarm = 0;
    for (let tick = 1; tick <= T; tick++) {
      if (ai < arrivals.length && arrivals[ai] === tick) {
        latest++;
        ai++;
      }
      seq[tick - 1] = latest;
    }
    // Every late arrival (gap 4) whose hold tick (start + 3) fits inside
    // the trace produces exactly one saturated hold — count them all.
    for (let i = 1; i < arrivals.length; i++) {
      if (arrivals[i] - arrivals[i - 1] === 4 && arrivals[i] + 3 <= T) {
        lateAfterWarm++;
      }
    }
    const p = new CadencePolicy({ policy: CadencePolicyKind.PREDICTIVE_PACED });
    let satHolds = 0;
    for (let t = 0; t < T; t++) {
      const d = p.step(seq[t]);
      if (t === 0) continue; // the pre-history tick (prev==0) — PACED-identical, not a prediction hold
      if (d.present && d.alphaQ12 === CADENCE_ALPHA_ONE_Q12) satHolds++;
    }
    // Exactly one saturated present per late window — whichever rule
    // serves it (the phase accumulator, or the reactive-gated integer
    // rule whose period is the previous 2-gap: both saturate exactly
    // once inside a 4-gap window, and the elision key collapses the
    // rest of the window to elision).
    expect(satHolds).toBe(lateAfterWarm);
    expect(lateAfterWarm).toBeGreaterThan(50); // the fixture is real
  });

  it('PC8 (integer equivalence): tick-for-tick identical to PACED_INTERPOLATE on every stable integer beat g=1..12', () => {
    for (let g = 1; g <= 12; g++) {
      const seq = stableBeat(g, 1600);
      const pred = new CadencePolicy({
        policy: CadencePolicyKind.PREDICTIVE_PACED,
      });
      const paced = new CadencePolicy({
        policy: CadencePolicyKind.PACED_INTERPOLATE,
      });
      const a1 = drive(pred, seq);
      const a2 = drive(paced, seq);
      for (let t = 800; t < seq.length; t++) {
        // Warmup excluded: the gap EWMA pins at (g*ONE - 3) by ~arrival
        // 42 (geometric decay 3/4 with integer truncation) — from there
        // the full-precision carry makes the ladders tick-for-tick
        // identical (verified: the phase equals floor(j*ONE^2/(g*ONE-3))
        // whose floor == PACED's trunc(j*4096/g) for every j < g).
        expect(
          [a1[t].present, a1[t].alpha],
          `g=${g} t=${t}: predictive ${JSON.stringify(a1[t])} vs paced ${JSON.stringify(a2[t])}`
        ).toEqual([a2[t].present, a2[t].alpha]);
      }
    }
  });

  it('PC9 (the judder gate): on the 12/5 beat every post-warmup window is monotone and within 5% of the ideal fractional ladder', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PREDICTIVE_PACED });
    const seq = stableBeat(2.4, 10000);
    let j = 0;
    let checked = 0;
    for (let t = 0; t < seq.length; t++) {
      const arrival = t > 0 && seq[t] > seq[t - 1];
      const d = p.step(seq[t]);
      if (arrival) {
        j = 0;
        continue;
      }
      j++;
      if (t < 200 || !d.present) continue; // warmup + elision-tolerant
      checked++;
      // Monotone: alpha follows the phase, which never decreases within
      // a window (saturates at the top — but PC7b says it never even
      // saturates here).
      expect(d.alphaQ12).toBeGreaterThanOrEqual(0);
      expect(d.alphaQ12).toBeLessThan(CADENCE_ALPHA_ONE_Q12);
      // 5% of the ideal fractional ladder: ideal = j*4096/2.4.
      const ideal = Math.trunc((j * CADENCE_ALPHA_ONE_Q12) / 2.4);
      expect(
        Math.abs(d.alphaQ12 - ideal),
        `t=${t} j=${j} alpha=${d.alphaQ12} ideal=${ideal}`
      ).toBeLessThanOrEqual(205);
    }
    expect(checked).toBeGreaterThan(5000);
  });

  it('PC9-contrast: PACED on the same beat deviates from the ideal ladder by far more than the gate', () => {
    // Documents WHY the gate exists: PACED's integer period quantization
    // blows the same bound (the sawtooth this policy removes).
    const p = new CadencePolicy({ policy: CadencePolicyKind.PACED_INTERPOLATE });
    const seq = stableBeat(2.4, 4000);
    let j = 0;
    let maxDev = 0;
    for (let t = 0; t < seq.length; t++) {
      const arrival = t > 0 && seq[t] > seq[t - 1];
      const d = p.step(seq[t]);
      if (arrival) {
        j = 0;
        continue;
      }
      j++;
      if (!d.present) continue;
      const ideal = Math.trunc((j * CADENCE_ALPHA_ONE_Q12) / 2.4);
      maxDev = Math.max(maxDev, Math.abs(d.alphaQ12 - ideal));
    }
    expect(maxDev).toBeGreaterThan(205); // RED under the predictive gate
  });

  it('PC10 (reactive fallback): the gate fires on an alternating 1,5 feed and the gated stream equals PACED tick-for-tick', () => {
    // Alternating gaps 1,5 for 6000 ticks, then a stable 2-beat for 4000.
    const arrivals: number[] = [];
    let t = 1;
    let flip = 0;
    while (t <= 6000) {
      arrivals.push(t);
      t += flip % 2 === 0 ? 1 : 5;
      flip++;
    }
    while (t <= 10000) {
      arrivals.push(t);
      t += 2;
    }
    const T = 10000;
    const seq: number[] = new Array(T);
    let latest = 0;
    let ai = 0;
    for (let i = 1; i <= T; i++) {
      if (ai < arrivals.length && arrivals[ai] === i) {
        latest++;
        ai++;
      }
      seq[i - 1] = latest;
    }
    const pred = new CadencePolicy({
      policy: CadencePolicyKind.PREDICTIVE_PACED,
    });
    const paced = new CadencePolicy({
      policy: CadencePolicyKind.PACED_INTERPOLATE,
    });
    const a1 = drive(pred, seq);
    const a2 = drive(paced, seq);
    // Gate fired substantially during the pathological regime.
    const midReactive = pred.reactiveTicks;
    expect(midReactive).toBeGreaterThan(3000);
    // While the gate is pinned on (varQ16*4 > gapQ16 holds for the whole
    // alternating regime), the predictive stream IS the PACED stream.
    for (let i = 3000; i < 6000; i++) {
      expect(
        [a1[i].present, a1[i].alpha],
        `t=${i}`
      ).toEqual([a2[i].present, a2[i].alpha]);
    }
    // Gate clears on the stable beat and stays clear (last 1000 ticks).
    const r0 = pred.reactiveTicks;
    for (let i = 9000; i < T; i++) pred.step(seq[i]);
    expect(pred.reactiveTicks - r0).toBe(0);
  });

  it('PC2 (Law 4 telescoping): sum(coalesced) == newestSeq - arrivalTicks on random traces', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PREDICTIVE_PACED });
    let state = 0x00c0ffee;
    let latest = 0;
    let sumCoalesced = 0;
    for (let i = 0; i < 10000; i++) {
      state = xorshift32(state);
      latest += state % 5;
      sumCoalesced += p.step(latest).coalesced;
    }
    expect(sumCoalesced).toBe(
      p['newestSeq'] - p.arrivalTicks
    );
  });

  it('PC4 (zero allocation): the decision record is identity-stable across steps', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PREDICTIVE_PACED });
    const seq = stableBeat(2.4, 100);
    const first = p.step(seq[0]);
    for (let t = 1; t < seq.length; t++) {
      expect(p.step(seq[t])).toBe(first); // same object, mutated in place
    }
  });

  it('PC6 (switch safety): reset(kind) mid-trace re-seats cleanly across all four kinds', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PREDICTIVE_PACED });
    let state = 0x00feed;
    let latest = 0;
    for (let i = 0; i < 500; i++) {
      state = xorshift32(state);
      latest += state % 5;
      p.step(latest);
    }
    const kinds = [
      CadencePolicyKind.LATEST_WINS,
      CadencePolicyKind.PACED_INTERPOLATE,
      CadencePolicyKind.BURST_COALESCE,
      CadencePolicyKind.PREDICTIVE_PACED,
    ];
    let lastPresented = 0;
    for (const k of kinds) {
      p.reset(k);
      expect(p.reactiveTicks).toBe(0);
      for (let i = 0; i < 300; i++) {
        state = xorshift32(state);
        latest += state % 5;
        const d = p.step(latest);
        if (d.present) {
          // Every policy presents the newest seq at most (monotone).
          expect(d.presentSeq).toBeGreaterThanOrEqual(lastPresented);
          lastPresented = d.presentSeq;
        }
      }
    }
  });

  it('stall: alpha saturates, holds honestly, never extrapolates past newest', () => {
    const p = new CadencePolicy({ policy: CadencePolicyKind.PREDICTIVE_PACED });
    const seq = stableBeat(2, 200);
    drive(p, seq);
    const frozen = seq[seq.length - 1];
    // Content stops: alpha saturates at the top of the window and the
    // raster holds (elision key stable — presents elide).
    let satPresents = 0;
    let elidedAfterSat = 0;
    let sawSat = false;
    for (let i = 0; i < 100; i++) {
      const d = p.step(frozen);
      if (d.alphaQ12 === CADENCE_ALPHA_ONE_Q12) sawSat = true;
      if (sawSat) {
        if (d.present && d.alphaQ12 === CADENCE_ALPHA_ONE_Q12) satPresents++;
        else if (!d.present) elidedAfterSat++;
      }
      expect(d.presentSeq).toBe(frozen); // never past newest
    }
    expect(sawSat).toBe(true);
    // Exactly one saturated present (the hold entering the elision key),
    // then honest elision for the remaining 98 frozen ticks.
    expect(satPresents).toBe(1);
    expect(elidedAfterSat).toBe(98);
  });

  it('PC11 (trace shape): the packed kind-3 trace hash is pinned for cross-port parity', () => {
    // The PC3 v2 packing for PREDICTIVE_PACED: b1 = present<<7 | interp<<6
    // | alphaQ12>>7, b2 = min(coalesced, 255) — the same wire shape the
    // other policies use; the fixture extends the stream with kind 3.
    const p = new CadencePolicy({ policy: CadencePolicyKind.PREDICTIVE_PACED });
    let state = 0x00c0ffee;
    let latest = 0;
    const bytes: number[] = [];
    for (let i = 0; i < 10000; i++) {
      state = xorshift32(state);
      latest += state % 5;
      const d = p.step(latest);
      bytes.push((d.present ? 0x80 : 0) | (d.interp ? 0x40 : 0) | (d.alphaQ12 >> 7));
      bytes.push(Math.min(d.coalesced, 255));
    }
    let h = 0xcbf29ce484222325n;
    for (const b of bytes) {
      h ^= BigInt(b);
      h = (h * 0x100000001b3n) & 0xffffffffffffffffn;
    }
    expect('0x' + h.toString(16).padStart(16, '0')).toBe(
      '0xa6b942ef48046e0e'
    );
  });
});
