// frame_cursor.dart — per-reader freshness telemetry (driver layer)
//
// WHY EXISTS: RFC-0008. The Triad Protocol drops intermediate frames by
// design (latest-wins IS the semantics of display) — but "dropped" was
// invisible to the reader. The kernel envelope already carries the fix:
// `seq` increments once per publish, so any reader can compute exactly how
// many frames were published between its own claims and never seen:
//
//     framesBehind = seq_now - seq_prev - 1
//
// ZERO kernel changes: no protocol version bump (the kernel surface stays
// frozen — 02 §2.2). The reader diffs its own claimed envelope seqs. Draw
// code can use this as a control signal: adapt detail level (LOD), skip
// decorative work, or flag degradation when it happens.
//
// SINGLE-ISOLATE: valid exactly like the kernel it wraps — writer and
// reader on one event loop (see core/dart/weft.dart).
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

import 'dart:typed_data';

import 'weft.dart';

/// One claim's freshness report.
class FrameClaim {
  /// Envelope seq of the claimed frame (the frame the reader now holds).
  final int seq;
  /// Frames published between the previous claim and this one that this
  /// reader never saw. 0 on the first claim (the null frame is the
  /// baseline: seq 0). A decreasing seq is treated as a writer reset —
  /// no drop accounting across the reset.
  final int framesBehind;
  /// True on the first claim of this cursor (no prior baseline).
  final bool first;
  /// Live payload view of the reader-held buffer (zero-copy; exclusively
  /// the reader's until its next claim). Absolute offset 0 = payload start.
  final Uint8List payload;
  FrameClaim(this.seq, this.framesBehind, this.first, this.payload);
}

class FrameCursor {
  int _lastSeq = 0;
  bool _hasClaimed = false;

  /// Accumulated dropped frames across the cursor's lifetime (advisory).
  int totalDropped = 0;
  /// Number of claims made through this cursor.
  int claims = 0;

  /// Claim the freshest frame and report freshness. Draw-phase hot path:
  /// one exchange, one envelope read, integer math.
  FrameClaim claim(Weft weft) {
    weft.claim();
    final seq = weft.rSeq();
    final first = !_hasClaimed;
    var framesBehind = 0;
    if (_hasClaimed && seq > _lastSeq) {
      framesBehind = seq - _lastSeq - 1;
    }
    _hasClaimed = true;
    totalDropped += framesBehind;
    _lastSeq = seq;
    claims++;
    return FrameClaim(seq, framesBehind, first, weft.rReadSlice(16, weft.payloadMax));
  }
}
