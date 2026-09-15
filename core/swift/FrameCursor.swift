// FrameCursor.swift — per-reader freshness telemetry (driver layer)
//
// WHY EXISTS: RFC-0008. The Triad Protocol drops intermediate frames by
// design (latest-wins IS the semantics of display) — but "dropped" was
// invisible to the reader. The kernel envelope already carries the fix:
// `seq` increments once per publish, so any reader can compute exactly how
// many frames were published between its own claims and never seen:
//
//     framesBehind = seq_now - seq_prev - 1
//
// ZERO kernel changes: no second atomic, no protocol version bump (the
// kernel surface stays frozen — 02 §2.2). The reader diffs its own claimed
// envelope seqs. Draw code can use this as a control signal: adapt detail
// level (LOD), skip decorative work, or flag degradation when it happens.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

import Foundation

/// One claim's freshness report.
public struct FrameClaim {
    /// Envelope seq of the claimed frame (the frame the reader now holds).
    public let seq: UInt32
    /// Frames published between the previous claim and this one that this
    /// reader never saw. 0 on the first claim (the null frame is the
    /// baseline: seq 0). A decreasing seq is treated as a writer reset —
    /// no drop accounting across the reset.
    public let framesBehind: UInt32
    /// True on the first claim of this cursor (no prior baseline).
    public let first: Bool
    /// Zero-copy live pointer into the reader-held payload (rLivePtr(16)) —
    /// exclusively the reader's until its next claim (RFC-0001 §4.3).
    /// Absolute offset 0 = payload start.
    public let payload: UnsafeRawPointer?
}

/// Per-reader freshness cursor. NOT Sendable: like the Weft's reader side,
/// one cursor belongs to one reader thread (the draw thread).
public final class FrameCursor {
    private var lastSeq: UInt32 = 0
    private var hasClaimed = false

    /// Accumulated dropped frames across the cursor's lifetime (advisory).
    public private(set) var totalDropped: UInt32 = 0
    /// Number of claims made through this cursor.
    public private(set) var claims: UInt32 = 0

    public init() {}

    /// Claim the freshest frame and report freshness. Draw-phase hot path:
    /// one exchange, one envelope read, integer math. Wait-free (I3).
    @discardableResult
    public func claim(_ weft: Weft) -> FrameClaim {
        _ = weft.claim()
        let seq = weft.rSeq()
        let first = !hasClaimed
        var framesBehind: UInt32 = 0
        if hasClaimed && seq > lastSeq {
            framesBehind = seq - lastSeq - 1
        }
        hasClaimed = true
        totalDropped &+= framesBehind
        lastSeq = seq
        claims &+= 1
        return FrameClaim(seq: seq, framesBehind: framesBehind, first: first, payload: weft.rLivePtr(16))
    }
}
