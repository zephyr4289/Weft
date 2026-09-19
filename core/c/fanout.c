// fanout.c — RFC 0004 fan-out ring, C driver layer (see fanout.h for the
// layout contract, the protocol, and the memory-ordering rationale).

#include "fanout.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Ordering regime
// ---------------------------------------------------------------------------
// Default: fenced acq/rel (see fanout.h). A/B: -DWEFT_FANOUT_SEQ_CST=1 gives
// the TS-equivalent all-seq_cst stamps. Payload words are relaxed-atomic in
// BOTH regimes (race-free by construction; the bracket carries the ordering).

#ifndef WEFT_FANOUT_SEQ_CST
#define WEFT_FANOUT_SEQ_CST 0
#endif

static inline void fan_stamp_store(_Atomic uint64_t* a, uint64_t v) {
#if WEFT_FANOUT_SEQ_CST
    atomic_store_explicit(a, v, memory_order_seq_cst);
#else
    atomic_store_explicit(a, v, memory_order_release);
#endif
}

static inline uint64_t fan_stamp_load(const _Atomic uint64_t* a) {
#if WEFT_FANOUT_SEQ_CST
    return atomic_load_explicit(a, memory_order_seq_cst);
#else
    return atomic_load_explicit(a, memory_order_acquire);
#endif
}

// Control-block indices (i64 slots in the TS port — same byte offsets).
enum { FAN_IDX_LATEST = 0, FAN_IDX_PUBLISHES = 1, FAN_IDX_SLOTSEQ = 2 };

static inline size_t fan_payload_base(unsigned slot_count) { return 16 + 8 * (size_t)slot_count; }

static int fan_geometry_ok(size_t payload_bytes, unsigned slot_count) {
    return payload_bytes != 0 && (payload_bytes % 4) == 0 && slot_count >= 2 &&
           slot_count <= WEFT_FANOUT_MAX_SLOTS;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

size_t weft_fanout_ring_bytes(size_t payload_bytes, unsigned slot_count) {
    if (!fan_geometry_ok(payload_bytes, slot_count)) return 0;
    return fan_payload_base(slot_count) + (size_t)slot_count * payload_bytes;
}

// ---------------------------------------------------------------------------
// Broadcaster
// ---------------------------------------------------------------------------

int weft_fanout_init(weft_fanout_t* f, size_t payload_bytes, unsigned slot_count) {
    if (!f) return -1;
    memset(f, 0, sizeof(*f));
    if (!fan_geometry_ok(payload_bytes, slot_count)) return -1;
    const size_t bytes = weft_fanout_ring_bytes(payload_bytes, slot_count);
    void* ring = NULL;
    if (posix_memalign(&ring, 64, bytes) != 0) return -1;
    memset(ring, 0, bytes); // latestSeq=0, publishes=0, all slotSeq=0 (invalidated)
    f->ring = (uint8_t*)ring;
    f->ctrl = (_Atomic uint64_t*)ring;
    f->payload_bytes = payload_bytes;
    f->slot_count = slot_count;
    f->w_seq = 0;
    f->w_slot = 0;
    f->w_cursor = NULL;
    f->owns_ring = 1;
    return 0;
}

int weft_fanout_attach_writer(weft_fanout_t* f, void* ring, size_t ring_bytes,
                              size_t payload_bytes, unsigned slot_count) {
    if (!f || !ring) return -1;
    // Refuse to attach over a live broadcaster: the memset below would wipe
    // owns_ring and silently LEAK an init-allocated ring. Caller error —
    // documented, and contained (state stays intact, nothing freed).
    if (f->ring) return -1;
    memset(f, 0, sizeof(*f));
    if (!fan_geometry_ok(payload_bytes, slot_count)) return -1;
    if (ring_bytes != weft_fanout_ring_bytes(payload_bytes, slot_count)) return -1;
    // The ring must already be a valid ring (ctrl zeroed or written by its
    // producer per the layout contract). Continue the frame numbering from
    // latestSeq so per-slot stamps stay monotonic across the producer handoff.
    _Atomic uint64_t* ctrl = (_Atomic uint64_t*)ring;
    const uint64_t latest = fan_stamp_load(ctrl + FAN_IDX_LATEST);
    f->ring = (uint8_t*)ring;
    f->ctrl = ctrl;
    f->payload_bytes = payload_bytes;
    f->slot_count = slot_count;
    f->w_seq = latest;
    f->w_slot = 0;
    f->w_cursor = NULL;
    f->owns_ring = 0;
    return 0;
}

uint8_t* weft_fanout_begin(weft_fanout_t* f) {
    f->w_seq += 1;
    const unsigned k = (unsigned)((f->w_seq - 1) % f->slot_count);
    // FI1 bracket, first half: invalidate BEFORE the fill. SeqCst store + a
    // SeqCst fence BEFORE the cursor is returned — property P1 (fanout.h):
    // no fill word may become visible before the invalidate stamp does.
    atomic_store_explicit(f->ctrl + FAN_IDX_SLOTSEQ + k, 0, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
    f->w_slot = k;
    f->w_cursor = f->ring + fan_payload_base(f->slot_count) + (size_t)k * f->payload_bytes;
    return f->w_cursor;
}

int weft_fanout_fill(weft_fanout_t* f, const void* src, size_t len) {
    if (!f->w_cursor) return -1;
    if (len > f->payload_bytes || (len % 4) != 0) return -1;
    _Atomic uint32_t* dst = (_Atomic uint32_t*)f->w_cursor;
    const uint32_t* s = (const uint32_t*)src;
    const size_t words = len / 4;
    for (size_t w = 0; w < words; w++) {
        // Relaxed atomic word store: race-free against a concurrent reader
        // copy; ordering is carried by the stamp bracket, not the words.
        atomic_store_explicit(dst + w, s[w], memory_order_relaxed);
    }
    return (int)words;
}

uint64_t weft_fanout_publish(weft_fanout_t* f) {
    if (f->w_seq == 0) return 0; // no begin() ever ran — detectable no-op (TS parity)
    const unsigned k = (f->w_slot);
    // FI1 bracket, second half: stamp (Release orders the fill below it),
    // then flip the publication point. Both Release — see fanout.h.
    fan_stamp_store(f->ctrl + FAN_IDX_SLOTSEQ + k, f->w_seq);
    fan_stamp_store(f->ctrl + FAN_IDX_LATEST, f->w_seq);
    atomic_fetch_add_explicit(f->ctrl + FAN_IDX_PUBLISHES, 1, memory_order_relaxed);
    return f->w_seq;
}

void weft_fanout_destroy(weft_fanout_t* f) {
    if (!f || !f->ring) return;
    if (f->owns_ring) free(f->ring);
    f->ring = NULL;
    f->ctrl = NULL;
    f->w_cursor = NULL;
    f->owns_ring = 0;
}

weft_fanout_t* weft_fanout_new(size_t payload_bytes, unsigned slot_count) {
    weft_fanout_t* f = (weft_fanout_t*)calloc(1, sizeof(weft_fanout_t));
    if (!f) return NULL;
    if (weft_fanout_init(f, payload_bytes, slot_count) != 0) {
        free(f);
        return NULL;
    }
    return f;
}

void weft_fanout_free(weft_fanout_t* f) {
    if (!f) return;
    weft_fanout_destroy(f);
    free(f);
}

const void* weft_fanout_ring(const weft_fanout_t* f) { return f ? (const void*)f->ring : NULL; }

void weft_fanout_debug_stats(const weft_fanout_t* f, weft_fanout_debug_t* out) {
    out->latest_seq = fan_stamp_load(f->ctrl + FAN_IDX_LATEST);
    out->publishes = fan_stamp_load(f->ctrl + FAN_IDX_PUBLISHES);
    out->slot_count = f->slot_count;
    out->payload_bytes = f->payload_bytes;
    for (unsigned k = 0; k < f->slot_count; k++) {
        out->slot_stamps[k] = fan_stamp_load(f->ctrl + FAN_IDX_SLOTSEQ + k);
    }
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

int weft_fanout_reader_init(weft_fanout_reader_t* r, const void* ring, size_t ring_bytes,
                            size_t payload_bytes, unsigned slot_count) {
    if (!r || !ring) return -1;
    memset(r, 0, sizeof(*r));
    if (!fan_geometry_ok(payload_bytes, slot_count)) return -1;
    if (ring_bytes != weft_fanout_ring_bytes(payload_bytes, slot_count)) return -1;
    void* target = NULL;
    if (posix_memalign(&target, 64, payload_bytes) != 0) return -1;
    r->ring = (uint8_t*)ring;
    r->ctrl = (_Atomic uint64_t*)ring;
    r->payload_bytes = payload_bytes;
    r->slot_count = slot_count;
    r->target = (uint32_t*)target;
    r->last_seq = 0;
    const uint8_t* payload = (const uint8_t*)ring + fan_payload_base(slot_count);
    for (unsigned k = 0; k < slot_count; k++) {
        r->slot_words[k] = (const _Atomic uint32_t*)(payload + (size_t)k * payload_bytes);
    }
    r->rec.fresh = false;
    r->rec.seq = 0;
    r->rec.dropped = 0;
    r->n_reads = 0; r->n_fresh = 0; r->n_drops = 0; r->n_skip = 0; r->n_exhausted = 0;
    return 0;
}

const weft_fanout_claim_t* weft_fanout_claim(weft_fanout_reader_t* r) {
    weft_fanout_claim_t* rec = &r->rec;
#ifndef NDEBUG
    if (r->ring == NULL || r->ctrl == NULL || r->target == NULL ||
        r->slot_count < 2 || r->slot_count > WEFT_FANOUT_MAX_SLOTS ||
        r->payload_bytes == 0 || (r->payload_bytes & 3) != 0) {
        rec->fresh = false;
        rec->seq = r->last_seq;
        rec->dropped = 0;
        r->n_skip++;
        return rec;
    }
#endif
    uint64_t L = fan_stamp_load(r->ctrl + FAN_IDX_LATEST);
    if (L == 0 || L == r->last_seq) {
        rec->fresh = false;
        rec->seq = r->last_seq;
        rec->dropped = 0;
        return rec;
    }
    for (int attempt = 0; attempt < WEFT_FANOUT_MAX_CLAIM_ATTEMPTS; attempt++) {
        const unsigned k = (unsigned)((L - 1) % r->slot_count);
        const uint64_t sB = fan_stamp_load(r->ctrl + FAN_IDX_SLOTSEQ + k);
        if (sB != L) {
            // Slot mid-overwrite (stamp 0) or already re-stamped by a newer
            // frame. Re-read latestSeq: unchanged -> graceful skip (Law 1);
            // changed -> a newer frame completed, chase it.
            const uint64_t L2 = fan_stamp_load(r->ctrl + FAN_IDX_LATEST);
            if (L2 == L) {
                r->n_skip++;
                rec->fresh = false;
                rec->seq = r->last_seq;
                rec->dropped = 0;
                return rec;
            }
            L = L2;
            continue;
        }
        // Stamp matches frame L: copy, then re-validate.
        const _Atomic uint32_t* src = r->slot_words[k];
        const size_t words = r->payload_bytes / 4;
        for (size_t w = 0; w < words; w++) {
            r->target[w] = atomic_load_explicit(src + w, memory_order_relaxed);
        }
        // Property P2 (fanout.h): the copy is ordered before the
        // revalidation load by a SeqCst fence — if the copy observed any
        // word of an overwrite, the revalidation below must observe the
        // invalidate-or-newer stamp.
        atomic_thread_fence(memory_order_seq_cst);
        const uint64_t sA = fan_stamp_load(r->ctrl + FAN_IDX_SLOTSEQ + k);
        if (sA == L) {
            // Consistent frame L (FI2: per-slot stamps are strictly
            // monotonic, so an unchanged stamp proves no overwrite began
            // during the copy).
            const uint64_t dropped = L - r->last_seq - 1;
            r->n_drops += dropped;
            r->last_seq = L;
            r->n_fresh++;
            rec->fresh = true;
            rec->seq = r->last_seq;
            rec->dropped = dropped;
            return rec;
        }
        // Torn copy detected — retry on the newest completed frame.
        L = fan_stamp_load(r->ctrl + FAN_IDX_LATEST);
    }
    // Bounded retries exhausted: keep the last consistent frame. Counted,
    // never silent, never a spin (Law 1).
    r->n_exhausted++;
    rec->fresh = false;
    rec->seq = r->last_seq;
    rec->dropped = 0;
    return rec;
}

const void* weft_fanout_view(const weft_fanout_reader_t* r) { return r->target; }

void weft_fanout_reader_stats(const weft_fanout_reader_t* r, weft_fanout_stats_t* out) {
    out->reads = r->n_reads;
    out->fresh = r->n_fresh;
    out->drops = r->n_drops;
    out->skipped_mid_overwrite = r->n_skip;
    out->torn_exhausted = r->n_exhausted;
}

void weft_fanout_reader_destroy(weft_fanout_reader_t* r) {
    if (!r) return;
    free(r->target);
    r->target = NULL;
    r->ring = NULL;
    r->ctrl = NULL;
}

weft_fanout_reader_t* weft_fanout_reader_new(const void* ring, size_t ring_bytes,
                                             size_t payload_bytes, unsigned slot_count) {
    weft_fanout_reader_t* r = (weft_fanout_reader_t*)calloc(1, sizeof(weft_fanout_reader_t));
    if (!r) return NULL;
    if (weft_fanout_reader_init(r, ring, ring_bytes, payload_bytes, slot_count) != 0) {
        free(r);
        return NULL;
    }
    return r;
}

void weft_fanout_reader_free(weft_fanout_reader_t* r) {
    if (!r) return;
    weft_fanout_reader_destroy(r);
    free(r);
}
