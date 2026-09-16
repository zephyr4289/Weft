// governorPolicy.test.ts — RFC-0009 demo wiring: the draw-decision policy.
//
// The governor object itself is pinned by packages/core/test/governor.test.ts
// (G1–G5, three languages). This suite pins the DEMO-LAYER policy that turns
// an action + frame identity into the mechanical draw decision — the same
// module the live panel (FeedFanoutViews) and the B4 proof bench
// (scripts/governor_bench.ts) share.

import { describe, it, expect } from 'vitest';
import {
  FreshnessGovernor,
  GovernorActionKind,
} from '@weft/core';
import { decideDraw } from '../src/modes/governorPolicy';

describe('RFC-0009 demo draw policy', () => {
  it('FastPath + new seq: full draw, no throttle', () => {
    const d = decideDraw({ kind: GovernorActionKind.FastPath, skipN: 0 }, true);
    expect(d).toEqual({ draw: true, reset: false, skipNextPoll: false });
  });

  it('FastPath + same seq: elide the draw AND skip the next poll (the throttle)', () => {
    const d = decideDraw({ kind: GovernorActionKind.FastPath, skipN: 0 }, false);
    expect(d).toEqual({ draw: false, reset: false, skipNextPoll: true });
  });

  it('Skip(n) + new seq: draw the newest once (intermediates already dropped by decision)', () => {
    const d = decideDraw({ kind: GovernorActionKind.Skip, skipN: 3 }, true);
    expect(d).toEqual({ draw: true, reset: false, skipNextPoll: false });
  });

  it('Skip(n) + same seq: elide (drops already counted by the governor)', () => {
    const d = decideDraw({ kind: GovernorActionKind.Skip, skipN: 3 }, false);
    expect(d).toEqual({ draw: false, reset: false, skipNextPoll: false });
  });

  it('Snapshot: always draw (the re-sync frame)', () => {
    for (const seqChanged of [true, false]) {
      const d = decideDraw({ kind: GovernorActionKind.Snapshot, skipN: 0 }, seqChanged);
      expect(d).toEqual({ draw: true, reset: false, skipNextPoll: false });
    }
  });

  it('Reseed: draw + reset the consumer baseline', () => {
    const d = decideDraw({ kind: GovernorActionKind.Reseed, skipN: 0 }, true);
    expect(d).toEqual({ draw: true, reset: true, skipNextPoll: false });
  });

  it('end-to-end wiring smoke: framesBehind -> gov.step() -> decideDraw() ladder', () => {
    // The exact composition the demo panel and the bench run, exercised as
    // one pipeline across the whole ladder (cooldown advances so Reseed is
    // not suppressed).
    const gov = new FreshnessGovernor();
    // A concrete consumer timeline. Behind uses the REAL cursor accounting
    // (RFC-0008: same-seq claim -> 0, never negative; increasing seq ->
    // seq - lastDrawn - 1): seqs 1, 1, 3, 6, 15, 55, 95, 135, 136 — the
    // second claim sees the SAME frame (no publish between polls): the
    // redundant-poll case.
    const seqs = [1, 1, 3, 6, 15, 55, 95, 135, 136];
    const timeline: boolean[] = [];
    let lastDrawn = -1; // the consumer's baseline
    let t = 0;
    for (const seq of seqs) {
      const behind = seq > lastDrawn ? seq - lastDrawn - 1 : 0;
      const action = gov.step(behind, (t += 300)); // past the cooldown each step
      const seqChanged = seq !== lastDrawn;
      const d = decideDraw(action, seqChanged);
      timeline.push(d.draw);
      if (d.draw) lastDrawn = seq;
    }
    // FastPath draws the first frame (behind=1 is still <= fastPathBehind);
    // FastPath ELIDES the redundant poll (the saved memcpy); FastPath draws
    // the next new frame; Skip(1) draws (first DECIDED drop); Snapshot
    // draws; Reseed draws x3 (300 ms steps exceed the 250 ms cooldown — all
    // emitted); FastPath draws again.
    expect(timeline).toEqual([
      true, // behind=1 (seq 1, baseline -1): FastPath, new frame
      false, // behind=0, same seq: FastPath, ELIDED — the saved memcpy
      true, // behind=1: FastPath, new
      true, // behind=2: Skip(1), new — the first DECIDED drop
      true, // behind=8: Snapshot
      true, // behind=39: Reseed
      true, // behind=39: Reseed
      true, // behind=39: Reseed
      true, // behind=0: FastPath, new again
    ]);
    expect(gov.reseeds).toBe(3);
    expect(gov.decidedDrops).toBe(1); // Skip(1) at step 4
  });
});
