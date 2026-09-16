//! # RFC 0008 FrameCursor — freshness telemetry, Rust driver layer
//!
//! Mirrors `packages/core/src/cursor.ts`, `core/kotlin/FrameCursor.kt`,
//! `core/swift/FrameCursor.swift`, `core/dart/frame_cursor.dart`, and
//! `core/c/frame_cursor.{h,c}` — completing the six-language matrix.
//!
//! The kernel drops intermediate frames by design (latest-wins IS the
//! semantics of display); the envelope `seq` the reader already holds turns
//! that invisible loss into a control signal:
//!
//! ```text
//! framesBehind = seq_now − seq_prev − 1
//! ```
//!
//! ZERO kernel changes: the cursor rides the ordinary wait-free claim
//! (`Weft::claim`) and the live envelope read (`r_seq`, an A3 live read),
//! then diffs its own baseline. u32 wrap / writer reset (a decreasing seq)
//! resets accounting rather than reporting a burst.
//!
//! Law 2: `claim` allocates nothing — the `FrameClaim` is returned by value
//! (a 9-byte Copy struct), state is integers only.

use crate::Weft;

/// One claim's freshness report (the TS `FrameClaim` minus the payload view —
/// in Rust the live view is the kernel's own `r_live_ptr`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FrameClaim {
    /// Envelope seq of the frame the reader now holds.
    pub seq: u32,
    /// Frames published between the previous claim and this one that this
    /// reader never saw. 0 on the first claim (the null frame is the
    /// baseline: seq 0).
    pub frames_behind: u32,
    /// True on the first claim of this cursor (no prior baseline).
    pub first: bool,
}

/// Per-reader freshness cursor. Driver-layer state only: the `Weft` it
/// samples is neither retained, modified, nor owned.
#[derive(Debug)]
pub struct FrameCursor {
    last_seq: u32,
    has_claimed: bool,
    /// Accumulated `frames_behind` across the cursor's lifetime (advisory,
    /// AXIOM T).
    pub total_dropped: u32,
    /// Number of claims made through this cursor (advisory).
    pub claims: u32,
}

impl Default for FrameCursor {
    fn default() -> Self {
        Self::new()
    }
}

impl FrameCursor {
    /// A fresh cursor: no baseline yet.
    pub fn new() -> Self {
        Self { last_seq: 0, has_claimed: false, total_dropped: 0, claims: 0 }
    }

    /// Claim the freshest frame on the kernel reader and report freshness:
    /// the ordinary wait-free claim (one exchange, RFC 0001 §4.3), then the
    /// live envelope seq read, then the delta. Zero allocation.
    pub fn claim(&mut self, weft: &Weft) -> FrameClaim {
        weft.claim();
        let seq = weft.r_seq();
        let first = !self.has_claimed;
        let mut frames_behind = 0;
        if self.has_claimed && seq > self.last_seq {
            frames_behind = seq - self.last_seq - 1; // u32, exact while gap < 2^31
        }
        // A decreasing seq (u32 wrap / writer reset) resets accounting rather
        // than reporting a huge burst — RFC 0008's declared semantics.
        self.has_claimed = true;
        self.total_dropped += frames_behind;
        self.last_seq = seq;
        self.claims += 1;
        FrameClaim { seq, frames_behind, first }
    }

    /// Reset accounting (a re-attach / new stream baseline): advisory
    /// accumulators are zeroed; the next claim reports `first: true`.
    pub fn reset(&mut self) {
        *self = Self::new();
    }
}
