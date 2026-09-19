// fanout_batch.c — batch publish over the frozen fan-out ring (see
// fanout_batch.h for the contract and the windowed-stamping invariant).

#include "fanout_batch.h"

#include <stdatomic.h>

// Documented wire offsets (fanout.h RING LAYOUT contract — the same table
// every port and the WFSH session header share):
//   ctrl[0]      latestSeq    _Atomic u64
//   ctrl[1]      publishes    _Atomic u64
//   ctrl[2 + k]  slotSeq[k]   _Atomic u64
#define FB_IDX_LATEST    0
#define FB_IDX_PUBLISHES 1
#define FB_IDX_SLOTSEQ   2

uint64_t weft_publish_batch(weft_fanout_t* f, const weft_batch_frame_t* frames,
                            size_t n) {
    if (!f || !f->ring || !frames) return 0;
    if (n == 0) return 0;
    // Atomic refusal: validate EVERY frame before anything is begun.
    // A malformed frame mid-batch must not half-publish the ring.
    for (size_t i = 0; i < n; i++) {
        const size_t len = frames[i].len;
        if (len > f->payload_bytes || (len % 4) != 0) return 0;
        if (!frames[i].src && len != 0) return 0;
    }

    const unsigned m = f->slot_count;
    const uint64_t seq0 = f->w_seq;

    // Phase 1 — begin + fill each frame under the frozen bracket.
    // begin() bumps w_seq, invalidates the slot (SeqCst store + fence —
    // P1) and returns the cursor; the fill is the relaxed-atomic word
    // loop (the fill() discipline, replicated because fill() drives the
    // single-slot w_cursor state machine).
    // When the batch walks past depth M, stamp each departed frame's slot
    // BEFORE the begin that reuses it invalidates the slot — the
    // stamp-vs-content invariant then holds at every instant (FB4).
    for (size_t i = 0; i < n; i++) {
        if (n > m && i >= m) {
            const uint64_t s = seq0 + (i - m) + 1;
            const unsigned k = (unsigned)((s - 1) % m);
            atomic_store_explicit(f->ctrl + FB_IDX_SLOTSEQ + k, s,
                                  memory_order_release);
        }
        uint8_t* cursor = weft_fanout_begin(f);
        if (frames[i].src != NULL && frames[i].len != 0) {
            _Atomic uint32_t* dst = (_Atomic uint32_t*)cursor;
            const uint32_t* src = (const uint32_t*)frames[i].src;
            const size_t words = frames[i].len / 4;
            for (size_t w = 0; w < words; w++) {
                atomic_store_explicit(dst + w, src[w], memory_order_relaxed);
            }
        }
    }

    // Phase 2 — the commit: stamp the last min(n, M) begun slots in batch
    // order (Release), then ONE publication flip (latestSeq, Release) and
    // one telemetry add. Intermediate frames beyond depth M were stamped
    // windowed in phase 1; the flip publishes the batch as a unit.
    const size_t tail = (n < (size_t)m) ? n : (size_t)m;
    const uint64_t last_seq = seq0 + n;
    for (size_t j = 0; j < tail; j++) {
        const uint64_t s = last_seq - tail + 1 + (uint64_t)j;
        const unsigned k = (unsigned)((s - 1) % m);
        atomic_store_explicit(f->ctrl + FB_IDX_SLOTSEQ + k, s,
                              memory_order_release);
    }
    atomic_store_explicit(f->ctrl + FB_IDX_LATEST, last_seq,
                          memory_order_release);
    atomic_fetch_add_explicit(f->ctrl + FB_IDX_PUBLISHES, (uint64_t)n,
                              memory_order_relaxed);
    return last_seq;
}

uint64_t weft_publish_batch_commit(weft_fanout_t* f, size_t n) {
    if (!f || !f->ring || n == 0) return 0;
    if (n > f->w_seq) return 0;  // more commits than begun frames — refused
    const unsigned m = f->slot_count;
    // Only the last min(n, M) frames still own their slots; earlier ones
    // were invalidated by later begins (their stamps were never written,
    // so the invariant is safe — they surface as `dropped` in readers).
    const size_t tail = (n < (size_t)m) ? n : (size_t)m;
    const uint64_t last_seq = f->w_seq;
    for (size_t j = 0; j < tail; j++) {
        const uint64_t s = last_seq - tail + 1 + (uint64_t)j;
        const unsigned k = (unsigned)((s - 1) % m);
        atomic_store_explicit(f->ctrl + FB_IDX_SLOTSEQ + k, s,
                              memory_order_release);
    }
    atomic_store_explicit(f->ctrl + FB_IDX_LATEST, last_seq,
                          memory_order_release);
    atomic_fetch_add_explicit(f->ctrl + FB_IDX_PUBLISHES, (uint64_t)n,
                              memory_order_relaxed);
    return last_seq;
}
