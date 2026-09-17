// governed-ui-thread.test.ts — Series 7 worklet parity + wiring tests.
//
// THE PARITY GATE (the PC3/G5 discipline applied to the WORKLET
// FLATTENING): the same canonical 04-LITMUS §0.2 xorshift32 trace through
// (a) @weft/core's reference FreshnessGovernor + CadencePolicy classes and
// (b) the flattened plain-object machines (ladderStep/cadenceStep, the
// bodies that carry 'worklet' directives and run on the UI thread when
// Reanimated processes them) must produce the IDENTICAL packed bytes —
// drift in the flattening fails here, in vitest, with no device needed.
//
// Plus: the createGovernedUiThread wiring (SharedValue-shaped plain
// objects — structural typing), the JS-thread fallback stance, and the
// closed-set guard.

import { describe, expect, it } from 'vitest';
import { CadencePolicyKind } from '@weft/core';
import {
  cadenceStep,
  createCadenceState,
  createDecision,
  createGovernedUiThread,
  createLadderState,
  ladderStep,
  referenceTickTrace,
} from '../src/governed-ui-thread';

function xorshift32(x: number): number {
  x >>>= 0;
  x ^= (x << 13) >>> 0;
  x ^= x >>> 17;
  x ^= (x << 5) >>> 0;
  return x >>> 0;
}

describe('RFC-0009 Series 7 — governed UI-thread worklets', () => {
  it('worklet flattening is byte-identical to @weft/core on the canonical trace (all three policies)', () => {
    const STEPS = 10000;
    for (const policy of [
      CadencePolicyKind.LATEST_WINS,
      CadencePolicyKind.PACED_INTERPOLATE,
      CadencePolicyKind.BURST_COALESCE,
    ]) {
      // The trace: arrivals drive the policy; the ladder sees the same
      // arrivals as drop bursts; nowMs = i (cooldowns fire).
      const seqs: number[] = [];
      const behinds: number[] = [];
      const nows: number[] = [];
      let state = 0x00c0ffee;
      let latest = 0;
      for (let i = 0; i < STEPS; i++) {
        state = xorshift32(state);
        const arrivals = state % 5;
        latest += arrivals;
        seqs.push(latest);
        behinds.push(arrivals === 0 ? 0 : arrivals); // drop-shaped staleness
        nows.push(i);
      }

      // (a) the reference (the same oracle the xlang fixtures use).
      const reference = referenceTickTrace(policy, seqs, behinds, nows);

      // (b) the flattened worklet machines.
      const ladder = createLadderState();
      const cadence = createCadenceState(policy);
      const decision = createDecision();
      const packed: number[] = [];
      let prevAction = 0;
      for (let i = 0; i < STEPS; i++) {
        ladderStep(ladder, behinds[i], nows[i], decision, prevAction);
        prevAction = decision.action;
        cadenceStep(cadence, seqs[i], decision);
        packed.push(
          ((decision.action << 6) | Math.min(decision.skipN, 63)) & 0xff,
          ((decision.present ? 1 : 0) << 7) |
            ((decision.interp ? 1 : 0) << 6) |
            (decision.alphaQ12 >> 7),
          Math.min(decision.coalesced, 255)
        );
      }

      expect(packed, `policy=${policy}`).toEqual(reference);
    }
  });

  it('createGovernedUiThread wires SharedValues and publishes the decision', () => {
    const latestSeq = { value: 0 };
    const framesBehind = { value: 0 };
    const nowMs = { value: 0 };
    const out = {
      present: { value: 0 },
      alphaQ12: { value: 0 },
      presentSeq: { value: 0 },
      action: { value: 0 },
    };
    const tick = createGovernedUiThread({
      policy: CadencePolicyKind.LATEST_WINS,
      latestSeq,
      framesBehind,
      nowMs,
      out,
    });

    // Three ticks: seq advances on 1 and 3 — LATEST_WINS presents both,
    // elides the middle one.
    latestSeq.value = 10;
    nowMs.value = 1;
    tick();
    expect(out.present.value).toBe(1);
    expect(out.presentSeq.value).toBe(10);

    latestSeq.value = 10; // unchanged — elided
    nowMs.value = 2;
    tick();
    expect(out.present.value).toBe(0);
    expect(out.presentSeq.value).toBe(10);

    latestSeq.value = 14; // jump of 4: 3 coalesced BY DECISION
    nowMs.value = 3;
    tick();
    expect(out.present.value).toBe(1);
    expect(out.presentSeq.value).toBe(14);
  });

  it('the ladder degrades a suppressed Reseed to Snapshot (cooldown honored worklet-side)', () => {
    const ladder = createLadderState();
    const decision = createDecision();
    ladderStep(ladder, 64, 1000, decision, 0);
    expect(decision.action).toBe(3); // Reseed
    ladderStep(ladder, 64, 1050, decision, 3); // inside cooldown
    expect(decision.action).toBe(2); // degraded to Snapshot
    expect(decision.actionChanged).toBe(true);
    ladderStep(ladder, 64, 1300, decision, 2); // past cooldown
    expect(decision.action).toBe(3);
    expect(ladder.reseeds).toBe(2);
    expect(ladder.decidedDrops).toBe(0); // Snapshot/Reseed add no drops
  });

  it('closed policy set — an unknown kind throws', () => {
    const cadence = createCadenceState(99);
    const decision = createDecision();
    expect(() => cadenceStep(cadence, 1, decision)).toThrow(/unknown cadence policy kind/);
  });

  it('PC2 telescoping holds across the flattened BURST machine', () => {
    const cadence = createCadenceState(CadencePolicyKind.BURST_COALESCE);
    const decision = createDecision();
    let state = 0x1234abcd;
    let latest = 0;
    for (let i = 0; i < 10000; i++) {
      state = xorshift32(state);
      latest += state % 5;
      cadenceStep(cadence, latest, decision);
    }
    expect(cadence.coalescedByDecision).toBe(
      cadence.lastPresentedSeq - cadence.presents
    );
    expect(cadence.presents).toBeGreaterThan(0);
  });
});
