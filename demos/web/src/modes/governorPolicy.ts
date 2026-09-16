// governorPolicy.ts — RFC-0009 consumer draw policy, demo layer.
//
// The governor decides WHICH class of action fits the consumer's freshness;
// this module turns an action + frame identity into the mechanical draw
// decision. It exists as a PURE function so the matrix tests can pin it
// (and so the bench and the live demo share one implementation — the
// FAIRNESS PIN discipline).
//
// Policy (documented consumer-side choices, per RFC-0009's "advisory only"
// lean — the governor never touches a Triad, the app interprets):
//
//   FastPath  + new seq   -> DRAW       (full-detail live frame)
//   FastPath  + same seq  -> SKIP       (idempotent redraw elided: the bytes
//                                        on screen are already current — the
//                                        saved memcpy is the proof artifact)
//   Skip(n)   + new seq   -> DRAW       (draw newest once; the n intermediates
//                                        were dropped by decision, counted in
//                                        the governor's own counter, Law 4)
//   Skip(n)   + same seq  -> SKIP       (same elision; drops still counted)
//   Snapshot              -> DRAW       (render one frame from the fresh
//                                        claim; expectations jump to latest
//                                        naturally via the next claim)
//   Reseed                -> DRAW+RESET (draw once, reset the consumer's
//                                        baseline — rate-limited by the
//                                        governor's 250 ms cooldown)
//
// THROTTLE (the fence elider): after a FastPath step with an unchanged seq,
// the consumer has direct evidence it is polling faster than the writer
// publishes. It may skip its NEXT poll's claim entirely — one saved fence.
// Bounded by construction: at most every other poll (duty 1/2), and fully
// self-correcting — a frame published during the skipped poll is not lost,
// it is simply claimed one poll later and reported by framesBehind (the
// governor's own accounting). This is the "throttle" decision from
// RFC-0009's summary, implemented as consumer policy, NOT inside the
// governor (whose action set is closed).

import { GovernorActionKind, type GovernorAction } from '@weft/core';

export interface DrawDecision {
  /** Copy the payload and raster (the memcpy). */
  draw: boolean;
  /** Reset the consumer baseline (a rebuilt/Reseeded view). */
  reset: boolean;
  /** Skip the next poll's claim — one saved fence (FastPath + same seq). */
  skipNextPoll: boolean;
}

/// One draw decision from one governor action. `seqChanged` = the claimed
/// seq differs from the last DRAWN seq.
export function decideDraw(action: GovernorAction, seqChanged: boolean): DrawDecision {
  switch (action.kind) {
    case GovernorActionKind.FastPath:
      if (seqChanged) {
        return { draw: true, reset: false, skipNextPoll: false };
      }
      return { draw: false, reset: false, skipNextPoll: true };
    case GovernorActionKind.Skip:
      // Draw the newest frame (once); intermediates are already gone —
      // latest-wins IS the transport; the governor just counted them.
      return { draw: seqChanged, reset: false, skipNextPoll: false };
    case GovernorActionKind.Snapshot:
      return { draw: true, reset: false, skipNextPoll: false };
    case GovernorActionKind.Reseed:
      return { draw: true, reset: true, skipNextPoll: false };
  }
}
