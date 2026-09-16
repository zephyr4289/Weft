// frame_cursor.h — RFC 0008: FrameCursor freshness telemetry, C driver layer
//
// WHY EXISTS: RFC 0008 (Accepted as design). The kernel drops intermediate
// frames by design (latest-wins IS the semantics of display) — but "dropped"
// was invisible to the reader. The envelope already carries the fix: `seq`
// increments once per publish, so any reader can compute exactly how many
// frames were published between its own claims and never seen:
//
//     framesBehind = seq_now - seq_prev - 1
//
// ZERO kernel changes: no second atomic, no protocol version bump, no
// frozen-surface breach — the cursor reads the reader-held envelope's seq
// (the kernel's public weft_r_seq, an A3 live read) and diffs it against the
// cursor's private baseline. This mirrors the four Heddle-language ports
// (packages/core/src/cursor.ts, core/kotlin/FrameCursor.kt,
// core/swift/FrameCursor.swift, core/dart/frame_cursor.dart) and completes
// the six-language matrix together with the Rust port (core/rust/src/frame_cursor.rs).
//
// Turned from invisible loss into a CONTROL SIGNAL: draw code can lower
// detail level when behind, restore it when caught up, and telemetry can
// quantify "now-ness" per reader.
//
// u32 wrap: a DECREASING seq (u32 wrap / writer reset) resets accounting
// rather than reporting a huge burst — the RFC's declared semantics. The
// difference is computed in u32 arithmetic, exact while the gap is < 2^31
// (the same bound the kernel's own litmus uses for staleness).
//
// LAW 2: update() allocates nothing (integer state + one envelope read).
// LAW 1: wait-free by construction (rides the kernel claim's exchange).
// LAW 4: framesBehind measures frames published between THIS reader's
//        claims that it never saw — not a global drop counter, not latency.

#ifndef WEFT_FRAME_CURSOR_H
#define WEFT_FRAME_CURSOR_H

#include <stdint.h>
#include <stdbool.h>

#include "weft.h"

/// One claim's freshness report (mirrors the TS FrameClaim minus the payload
/// view — in C the payload view is the kernel's own weft_r_live_ptr()).
typedef struct {
    uint32_t seq;           ///< Envelope seq of the frame the reader now holds
    uint32_t frames_behind; ///< Frames published between claims, never seen (0 on first)
    bool first;             ///< True on the first claim of this cursor (no baseline)
} weft_frame_sample_t;

/// Per-reader freshness cursor. Driver-layer state only — the weft_t it
/// samples is not retained, not modified, and not owned.
typedef struct {
    uint32_t last_seq;      ///< Private baseline (envelope seq of the previous claim)
    bool has_claimed;       ///< False until the first update()
    uint32_t total_dropped; ///< Accumulated framesBehind across the lifetime (advisory)
    uint32_t claims;        ///< Number of updates made through this cursor (advisory)
    weft_frame_sample_t rec; ///< Identity-stable sample record (mutated per update)
} weft_frame_cursor_t;

/// Initialize a cursor (equivalent to zero-init; provided for symmetry with
/// the rest of the C API surface). Zero allocation.
void weft_frame_cursor_init(weft_frame_cursor_t* c);

/// Claim the freshest frame on the kernel reader and report freshness:
/// performs the ordinary wait-free claim (weft_r_claim — one exchange, the
/// kernel hot path), reads the claimed envelope's seq, computes the delta.
/// Returns the cursor-owned sample record (identity-stable, mutated in
/// place — read it synchronously, do not retain across updates).
///
/// The weft_t pointer is const: the claim is a kernel-reader operation on
/// the reader's OWN handle; the cursor adds no kernel state of any kind.
const weft_frame_sample_t* weft_frame_cursor_update(weft_frame_cursor_t* c, weft_t* w);

/// Reset accounting (a re-attach / new stream baseline). Advisory
/// accumulators are zeroed; the next update reports first=true.
void weft_frame_cursor_reset(weft_frame_cursor_t* c);

#endif // WEFT_FRAME_CURSOR_H
