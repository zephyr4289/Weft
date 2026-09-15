// cursor.ts — FrameCursor: per-reader freshness telemetry (driver layer)
//
// WHY EXISTS: RFC-0008. The Triad Protocol drops intermediate frames by
// design (latest-wins IS the semantics of display) — but "dropped" was
// invisible to the reader. The kernel carries the fix in its envelope: the
// `seq` counter increments once per publish, so any reader can compute
// exactly how many frames were published between its own claims and never
// seen:
//
//     framesBehind = seq_now - seq_prev - 1
//
// ZERO kernel changes: no second atomic, no protocol version bump, no
// frozen-surface breach. The reader diffs its own claimed envelope seqs.
// This turns "dropped frames" from invisible loss into a CONTROL SIGNAL —
// draw code can adapt detail level (LOD), skip decorative work, or flag
// degradation precisely when it happens.
//
// Zero allocation per claim (Law 2): the payload is the cached rLive()
// view; the cursor holds no per-frame state beyond integers.

import { Weft } from './index';

/// One claim's freshness report.
export interface FrameClaim {
  /// Envelope seq of the claimed frame (the frame the reader now holds).
  seq: number;
  /// Frames published between the previous claim and this one that this
  /// reader never saw. 0 on the first claim (the null frame is the
  /// baseline: seq 0). u32 wrap is handled by treating a decreasing seq
  /// as a writer reset (no drop accounting across the reset).
  framesBehind: number;
  /// True on the first claim of this cursor (no prior baseline).
  first: boolean;
  /// Zero-allocation live payload view of the reader-held buffer —
  /// exclusively the reader's until its next claim (RFC-0001 §4.3).
  payload: Uint8Array;
}

export class FrameCursor {
  private lastSeq = 0;
  private hasClaimed = false;
  /// Accumulated dropped frames across the cursor's lifetime (advisory).
  totalDropped = 0;
  /// Number of claims made through this cursor.
  claims = 0;

  /// Claim the freshest frame and report freshness. Draw-phase hot path:
  /// one exchange inside weft.claim(), one envelope read, integer math.
  claim(weft: Weft): FrameClaim {
    weft.claim();
    const seq = weft.rSeq();
    const first = !this.hasClaimed;
    let framesBehind = 0;
    if (this.hasClaimed && seq > this.lastSeq) {
      framesBehind = seq - this.lastSeq - 1;
    }
    this.hasClaimed = true;
    this.totalDropped += framesBehind;
    this.lastSeq = seq;
    this.claims++;
    return { seq, framesBehind, first, payload: weft.rLive() };
  }
}
