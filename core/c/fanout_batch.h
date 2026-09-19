// fanout_batch.h — N-frame batch publish for the RFC-0004 fan-out ring
// (issue #17 task 3: Writer Batching)
//
// WHY EXISTS: high-throughput writers (video encoders, telemetry dumps,
// replay tooling) publish BURSTS of frames. Single-frame publish flips the
// ring's ONE publication point (latestSeq) per frame — the single most
// contended word in the system: every reader polls it, so under load each
// flip is a MESI line bounce across the writer's and every polling reader's
// core. Batching amortizes the publication flip: N frames enter the ring
// under the exact same per-slot brackets, then ONE commit phase publishes
// them — N slot stamps (Release, per-slot) + ONE latestSeq store (Release)
// + one telemetry add.
//
// The issue's "single atomic_exchange per batch" maps to THIS layer's
// single publication flip: the kernel (weft.c) is 1-writer/1-reader with
// three buffers and its exchange IS the whole protocol — it has no
// multi-slot batch surface to amortize (a batch there is just "the newest
// frame wins", which weft_publish already expresses). Batching lives where
// the slots are: the fan-out ring. weft_publish itself is UNTOUCHED (the
// issue's backward-compatibility requirement) — this module ADDS the batch
// path beside it, driving the ring through the frozen public surface
// (the attach_writer precedent for out-of-module ring drivers).
//
// SEMANTICS (the contract FB-series pins):
//   weft_publish_batch(f, frames, n):
//     - frames[i] = { src, len } — len %4 == 0, len <= payload_bytes
//       (the fill contract); NULL src skips the fill (begin-only frame).
//     - Assigns seq w_seq+1 .. w_seq+n to the n frames (w_seq advances by
//       n; the caller's next single-frame begin continues from w_seq+n).
//     - Per frame: the EXACT frozen bracket — begin() invalidate (SeqCst
//       store + fence, P1) then a relaxed-atomic word fill (the fill()
//       discipline, replicated in-module because fill() is tied to the
//       single-slot w_cursor state).
//     - Commit phase: slotSeq[k_i] <- seq_i as Release stores, in batch
//       order; then ONE latestSeq <- seq_n (Release); publishes += n.
//     - Returns the LAST frame's seq (== the new latestSeq), or 0 on
//       refused input (n==0, bad len, NULL ring) — a detectable no-op,
//       the weft_fanout_publish convention. Partial fills are refused
//       atomically: nothing is published when any frame is malformed.
//   READER VIEW: identical to a fast single-frame writer. A reader polling
//     during the batch sees old frames until the flip; after the flip the
//     freshest frame is the batch's LAST frame; `dropped` telescopes
//     exactly (L - lastSeq - 1). Intermediate frames are individually
//     claimable by seq (per-slot stamps) until overwritten.
//   n > ring depth M: LEGAL. The batch walks the ring; a slot reused
//     within the batch is stamped for its EARLIER frame BEFORE the later
//     begin() invalidates it (windowed stamping) — the stamp-vs-content
//     invariant holds at every instant, so no reader can validate a
//     transient mismatch (FB4's torture pins this). Only the last M
//     frames remain claimable at commit, exactly as if M single publishes
//     had raced ahead of the reader — Law 1's dropped accounting absorbs
//     the rest.
//
// LAW 2: allocates nothing. LAW 1: bounded (one pass + one commit).
// LAW 4: the mapping note above is honesty, not marketing.

#ifndef WEFT_FANOUT_BATCH_H
#define WEFT_FANOUT_BATCH_H

#include <stddef.h>
#include <stdint.h>

#include "fanout.h"

#ifdef __cplusplus
extern "C" {
#endif

/// One frame of a batch: source bytes + length (the fill contract).
typedef struct {
    const void* src;  ///< Payload source (NULL = begin-only frame, no fill)
    size_t len;       ///< Bytes to fill (multiple of 4, <= payload_bytes)
} weft_batch_frame_t;

/// Publish `n` frames as one batch (see header contract). Returns the last
/// frame's seq (the new latestSeq), or 0 on refused input — nothing is
/// published on refusal.
uint64_t weft_publish_batch(weft_fanout_t* f, const weft_batch_frame_t* frames,
                            size_t n);

/// The commit phase alone, for writers that prefer begin()/fill() per
/// frame themselves: stamps the begun-but-unpublished slots [w_seq-n+1 ..
/// w_seq] in order, then flips latestSeq ONCE. Returns the new latestSeq,
/// or 0 when n == 0 / n > w_seq / f is not a live broadcaster.
/// CONTRACT: the caller must have begun exactly n frames since the last
/// publish/batch (a begun frame is stamped by ITS begin order — this
/// helper reads the ring's own slot layout, no extra state).
uint64_t weft_publish_batch_commit(weft_fanout_t* f, size_t n);

#ifdef __cplusplus
}
#endif

#endif // WEFT_FANOUT_BATCH_H
