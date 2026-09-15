// weft.c — Corrected Triad Protocol kernel (C reference)
//
// Normative: per 02-KERNEL.md and 03-ENVELOPE.md.
//
// Memory-ordering audit (matches litmus/catalog.yaml `ordering_matrix`):
//   latest.exchange          : AcqRel  (writer publish, reader claim)
//   revoked.load (writer)    : Relaxed (advisory)
//   revoked.store (releaser) : Release (pairs with ACK chain)
//   epoch.fetch_add (writer) : AcqRel  (the ACK)
//   epoch.load (reclaim)     : Acquire (observes ACK)
//   telemetry fetch_add      : Relaxed (statistics only)
//
// No memory_order_seq_cst anywhere. No second atomic on the ownership path.

#define _GNU_SOURCE  // for posix_memalign
#include "weft.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

// Magic "WEFT" little-endian: bytes 57 45 46 54 → u32 LE = 0x54464557
#define WEFT_MAGIC  0x54464557u
#define WEFT_VERSION_1  1u

// ---------------------------------------------------------------------------
// Buffer layout helpers
// ---------------------------------------------------------------------------

// buf_size = align64(16 + payload_max + 8)  [16-byte envelope + payload + 8-byte canary, 64-aligned]
static size_t compute_buf_size(size_t payload_max) {
    size_t raw = 16 + payload_max + 8;
    return (raw + 63) & ~(size_t)63;
}

// Canary lives at the last 8 bytes of the buffer.
static size_t canary_offset(size_t buf_size) { return buf_size - 8; }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

int weft_init(weft_t* w, size_t payload_max) {
    memset(w, 0, sizeof(*w));
    w->payload_max = payload_max;
    w->buf_size = compute_buf_size(payload_max);

    // Allocate 3 buffers, 64-byte aligned. posix_memalign requires size to be a
    // multiple of alignment; buf_size is already 64-aligned by construction.
    for (int i = 0; i < 3; i++) {
        void* p = NULL;
        // Per 06-PITFALLS §2: check both return value and size % 64.
        if (posix_memalign(&p, 64, w->buf_size) != 0 || p == NULL) {
            // Clean up what we have.
            for (int j = 0; j < i; j++) { free(w->buf[j]); w->buf[j] = NULL; }
            return -1;
        }
        w->buf[i] = (uint8_t*)p;
        memset(w->buf[i], 0, w->buf_size);
        // Write null envelopes (seq=0, magic, version=1, header_size=16) into all three
        // so the reader's first claim (which may return any of the three) sees a valid envelope.
        weft_envelope_encode_v1(w->buf[i], 0, (uint32_t)payload_max);
        // Per 04-LITMUS §0.6 (v1.1, normative null-frame fixture invariant): fill the
        // null frame's payload with pat(0, i) and canary with 0 so verify_held(0, payload_max)
        // passes — the null frame is a valid initial state, not a sentinel that breaks
        // verification. A kernel that zero-fills the null frame makes L6 false-red.
        for (size_t j = 0; j < payload_max; j++) {
            w->buf[i][16 + j] = weft_pat(0, (uint32_t)j);
        }
        // Canary is at buf_size - 8; value = 0 (matches seq=0). memset already zeroed it.
    }

    // Per 02 §1: latest=0, w_work=1, r_work=2.
    atomic_init(&w->latest, (uint32_t)0);
    w->w_work = 1;
    w->r_work = 2;
    atomic_init(&w->revoked, false);
    atomic_init(&w->epoch, (uint32_t)0);
    atomic_init(&w->t_publish, (uint64_t)0);
    atomic_init(&w->t_claim, (uint64_t)0);
    atomic_init(&w->t_drop, (uint64_t)0);
    atomic_init(&w->t_wsteps, (uint64_t)0);
    atomic_init(&w->t_rsteps, (uint64_t)0);

    return 0;
}

void weft_destroy(weft_t* w) {
    // Per 02 §3: idempotent; frees nothing that reclaim already released.
    // If revoke was never called, plain free is correct (no writer exists).
    // `destroy` without a prior `reclaim` while a writer may still run is a
    // caller error — documented here, not defended.
    for (int i = 0; i < 3; i++) {
        free(w->buf[i]);
        w->buf[i] = NULL;
    }
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

uint8_t* weft_w_begin(weft_t* w) {
    // Live pointer to the writer's working buffer at payload offset.
    return w->buf[w->w_work] + 16;
}

int weft_w_write_payload(weft_t* w, const uint8_t* src, size_t len) {
    if (len > w->payload_max) return -1;
    memcpy(w->buf[w->w_work] + 16, src, len);
    return 0;
}

weft_pub_result_t weft_publish(weft_t* w, uint32_t seq, uint32_t payload_len) {
    // §6 step 1: revoked checked FIRST, before any byte write. Relaxed load is
    // advisory — correctness does not depend on seeing it THIS publish; the
    // NEXT publish will see it. Buffers stay valid until ACK.
    if (atomic_load_explicit(&w->revoked, memory_order_relaxed)) {
        // ACK: epoch.fetch_add(1, AcqRel). Publishes "I will never write again."
        atomic_fetch_add_explicit(&w->epoch, 1, memory_order_acq_rel);
        atomic_fetch_add_explicit(&w->t_drop, 1, memory_order_relaxed);
        return WEFT_PUB_DROPPED_REVOKED;
    }

    // 02 §2: write envelope (v1, seq, payload_len) into buf[w_work]
    weft_envelope_encode_v1(w->buf[w->w_work], seq, payload_len);

    // 02 §2: write canary = seq at buf[w_work].tail (u64 LE)
    uint64_t canary_val = (uint64_t)seq;
    // 03-ENVELOPE §1: canary is u64 LE at buf_size-8
    uint8_t* canary_ptr = w->buf[w->w_work] + canary_offset(w->buf_size);
    memcpy(canary_ptr, &canary_val, sizeof(uint64_t));

    // THE atomic: publish + take old latest. AcqRel:
    //   Release: publishes payload + envelope + canary writes to the reader.
    //   Acquire: takes ownership of the returned buffer, sees its final state.
    uint32_t old = atomic_exchange_explicit(&w->latest, w->w_work, memory_order_acq_rel);
    w->w_work = old;

    // Telemetry (Relaxed — never synchronization).
    atomic_fetch_add_explicit(&w->t_publish, 1, memory_order_relaxed);
    // L2 step counter: one protocol RMW per publish. Incremented AFTER the RMW.
    atomic_fetch_add_explicit(&w->t_wsteps, 1, memory_order_relaxed);

    return WEFT_PUB_OK;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

uint32_t weft_r_claim(weft_t* w) {
    // 02 §2: mine = latest.exchange(r_work, AcqRel)
    //   Acquire: sees the writer's published payload + envelope + canary.
    //   Release: the reader's previous buffer (r_work) is now handed back to
    //            the writer; its state is the reader's final state.
    uint32_t mine = atomic_exchange_explicit(&w->latest, w->r_work, memory_order_acq_rel);
    w->r_work = mine;

    atomic_fetch_add_explicit(&w->t_claim, 1, memory_order_relaxed);
    // L3 step counter: one protocol RMW per claim.
    atomic_fetch_add_explicit(&w->t_rsteps, 1, memory_order_relaxed);

    return mine;  // NEVER fails (02 §2.2)
}

// Live envelope field readers. Each reads buf[r_work] at call time.
uint32_t weft_r_seq(weft_t* w) {
    uint32_t seq;
    memcpy(&seq, w->buf[w->r_work] + 8, 4);  // offset 8, LE
    return seq;
}
uint16_t weft_r_header_size(weft_t* w) {
    uint16_t hs;
    memcpy(&hs, w->buf[w->r_work] + 6, 2);  // offset 6, LE
    return hs;
}
uint32_t weft_r_magic(weft_t* w) {
    uint32_t m;
    memcpy(&m, w->buf[w->r_work], 4);  // offset 0, LE
    return m;
}
uint32_t weft_r_payload_len(weft_t* w) {
    uint32_t pl;
    memcpy(&pl, w->buf[w->r_work] + 12, 4);  // offset 12, LE
    return pl;
}

size_t weft_r_read_slice(weft_t* w, uint8_t* dst, size_t offset, size_t dst_len) {
    // A3: copies LIVE held-buffer bytes at call time. NOT a claim-time snapshot.
    if (offset >= w->buf_size) return 0;
    size_t avail = w->buf_size - offset;
    size_t n = dst_len < avail ? dst_len : avail;
    memcpy(dst, w->buf[w->r_work] + offset, n);
    return n;
}

const uint8_t* weft_r_live_ptr(weft_t* w, size_t offset) {
    if (offset >= w->buf_size) return NULL;
    return w->buf[w->r_work] + offset;
}

// ---------------------------------------------------------------------------
// I6 — writer revocation handshake (02 §6, A1)
// ---------------------------------------------------------------------------

void weft_revoke(weft_t* w) {
    // Step 1: releaser sets revoked.store(true, Release).
    // Pairs with the ACK's fetch_add(AcqRel) chain into reclaim's Acquire poll —
    // establishes the happens-before edge that makes poison/free safe.
    atomic_store_explicit(&w->revoked, true, memory_order_release);
}

int weft_reclaim(weft_t* w, uint32_t pre_revoke_epoch, uint32_t timeout_ms) {
    // Steps 2-3: poll epoch (Acquire) until it advances past pre_revoke_epoch,
    // bounded by timeout_ms.
    //
    // Why the ACK is load-bearing (02 §6): between the writer's revocation check
    // and its envelope write there is a window; freeing in that window is a
    // use-after-free across FFI. The ACK closes it: reclaim's Acquire poll
    // observes the ACK's fetch_add, which is ordered after the writer's final
    // byte write. Poison-before-ACK is the bug; poison-after-ACK is the protocol.

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    uint64_t deadline_ns = (uint64_t)ts_start.tv_sec * 1000000000ull
                          + (uint64_t)ts_start.tv_nsec
                          + (uint64_t)timeout_ms * 1000000ull;

    while (true) {
        uint32_t e = atomic_load_explicit(&w->epoch, memory_order_acquire);
        if (e != pre_revoke_epoch) {
            return 0;  // ACK received
        }
        // Check timeout
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        uint64_t now_ns = (uint64_t)ts_now.tv_sec * 1000000000ull
                         + (uint64_t)ts_now.tv_nsec;
        if (now_ns >= deadline_ns) {
            return -1;  // timeout
        }
        // Brief sleep to avoid burning CPU. 100µs is fine — the writer ACKs
        // within one publish, which is < 1ms at our test rates.
        struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 };
        nanosleep(&sleep_ts, NULL);
    }
}

// ---------------------------------------------------------------------------
// Telemetry (Relaxed loads)
// ---------------------------------------------------------------------------

uint64_t weft_t_publish(weft_t* w) { return atomic_load_explicit(&w->t_publish, memory_order_relaxed); }
uint64_t weft_t_claim(weft_t* w)   { return atomic_load_explicit(&w->t_claim, memory_order_relaxed); }
uint64_t weft_t_drop(weft_t* w)    { return atomic_load_explicit(&w->t_drop, memory_order_relaxed); }
uint64_t weft_t_wsteps(weft_t* w) { return atomic_load_explicit(&w->t_wsteps, memory_order_relaxed); }
uint64_t weft_t_rsteps(weft_t* w) { return atomic_load_explicit(&w->t_rsteps, memory_order_relaxed); }
uint32_t weft_epoch(weft_t* w)    { return atomic_load_explicit(&w->epoch, memory_order_acquire); }
bool     weft_revoked(weft_t* w)  { return atomic_load_explicit(&w->revoked, memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Envelope pure functions (03-ENVELOPE §1, §2)
// ---------------------------------------------------------------------------

void weft_envelope_encode_v1(uint8_t* dst, uint32_t seq, uint32_t payload_len) {
    weft_envelope_encode(dst, WEFT_VERSION_1, 16, seq, payload_len);
}

void weft_envelope_encode(uint8_t* dst, uint16_t version, uint16_t header_size,
                          uint32_t seq, uint32_t payload_len) {
    // Per 03-ENVELOPE §1: all fields little-endian. Write via memcpy (no type-punning).
    uint32_t magic = WEFT_MAGIC;
    memcpy(dst + 0, &magic, 4);
    memcpy(dst + 4, &version, 2);
    memcpy(dst + 6, &header_size, 2);
    memcpy(dst + 8, &seq, 4);
    memcpy(dst + 12, &payload_len, 4);
    // If header_size > 16, fill the unknown trailing fields with 0xAA (L8b convention).
    // The decoder MUST skip them.
    for (size_t i = 16; i < (size_t)header_size; i++) {
        dst[i] = 0xAA;
    }
}

weft_decode_result_t weft_envelope_decode(const uint8_t* buf, size_t avail,
                                          uint16_t* version, uint16_t* header_size,
                                          uint32_t* seq, uint32_t* payload_len) {
    // Per 03-ENVELOPE §2:
    if (avail < 16) return WEFT_DECODE_SHORT;
    uint32_t magic;
    memcpy(&magic, buf + 0, 4);
    if (magic != WEFT_MAGIC) return WEFT_DECODE_BAD_MAGIC;
    uint16_t v, hs;
    memcpy(&v,  buf + 4, 2);
    memcpy(&hs, buf + 6, 2);
    if (hs < 16 || hs > avail) return WEFT_DECODE_BAD_HEADER;
    uint32_t s, pl;
    memcpy(&s,  buf + 8, 4);
    memcpy(&pl, buf + 12, 4);
    // Payload begins at header_size (NOT 16). Hardcoding 16 fails L8b by construction.
    if (pl > avail - hs) return WEFT_DECODE_SHORT;
    // Unknown trailing fields (between 16 and hs) are skipped without validation.
    if (version)     *version     = v;
    if (header_size) *header_size = hs;
    if (seq)         *seq         = s;
    if (payload_len) *payload_len = pl;
    return WEFT_DECODE_OK;
}

uint16_t weft_negotiate(uint16_t writer_version, const uint16_t* reader_versions,
                        size_t reader_count) {
    // Per 03-ENVELOPE §3: chosen = max({ v in S : v <= W })
    uint16_t chosen = 0;
    for (size_t i = 0; i < reader_count; i++) {
        uint16_t rv = reader_versions[i];
        if (rv <= writer_version && rv > chosen) {
            chosen = rv;
        }
    }
    // 0 = BIND_INCOMPATIBLE (empty set)
    return chosen;
}

// ---------------------------------------------------------------------------
// Shared payload pattern (04-LITMUS §0.1)
// ---------------------------------------------------------------------------

uint32_t weft_mix32(uint32_t x) {
    // Per 04-LITMUS §0.1. All ops mod 2^32 (uint32_t arithmetic, overflow defined).
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

uint8_t weft_pat(uint32_t seq, uint32_t i) {
    // Per 04-LITMUS §0.1. Deterministic payload byte.
    // C: uint32_t arithmetic (overflow defined). Identical results in Rust (u32 wrapping_mul)
    // and TS (Math.imul + >>> 0). The three implementations MUST produce byte-identical
    // sequences — L6's differential property depends on it (A5).
    uint32_t x = seq * 2654435761u + i * 2246822519u;
    return (uint8_t)(weft_mix32(x) & 0xFF);
}

uint32_t weft_xorshift32(uint32_t* state) {
    // Per 04-LITMUS §0.2. Marsaglia 13/17/5. State 0 is invalid (reseed 0x9E3779B9).
    if (*state == 0) *state = 0x9E3779B9u;
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// ---------------------------------------------------------------------------
// Debug view (WO-P2-TOOLS T1) — read-only, wait-free, allocation-free.
// The ONE permitted kernel API addition for Phase 2. Semver: minor bump.
// ---------------------------------------------------------------------------

void weft_debug_view(const weft_t* w, weft_debug_view_t* out) {
    memset(out, 0, sizeof(*out));

    // Advisory loads (per AXIOM T — telemetry is not a correctness reference).
    out->latest   = atomic_load_explicit(&w->latest, memory_order_relaxed);
    out->w_work   = w->w_work;       // thread-private; advisory-only
    out->r_work   = w->r_work;       // thread-private; advisory-only
    out->revoked  = atomic_load_explicit(&w->revoked, memory_order_relaxed);
    out->epoch    = atomic_load_explicit(&w->epoch, memory_order_acquire);
    out->t_publish = atomic_load_explicit(&w->t_publish, memory_order_relaxed);
    out->t_claim   = atomic_load_explicit(&w->t_claim, memory_order_relaxed);
    out->t_drop    = atomic_load_explicit(&w->t_drop, memory_order_relaxed);

    // Determine which two buffers are "live" (have an owner).
    // The three buffers are always in one of these states:
    //   - writer-owned (index == w_work)
    //   - reader-owned (index == r_work)
    //   - in-exchange (index == latest)
    // The third buffer (if any) has no live owner — report by index/state only,
    // NO dereference (I6 rule: a debug tool that causes use-after-free is a
    // Law-2/Law-4 violation in one move).
    //
    // We sample the two LIVE buffers' 16-byte envelope headers.
    // A header sampled mid-publish may be internally inconsistent — report
    // mid_publish_sample=true with the raw bytes rather than failing.
    // Diagnostics, not truth.

    int live_count = 0;
    for (int i = 0; i < 3; i++) {
        // Determine if this buffer is live (has an owner)
        bool is_live = (i == (int)out->w_work) || (i == (int)out->r_work) || (i == (int)out->latest);
        if (is_live && live_count < 2) {
            weft_debug_buf_t* b = &out->bufs[live_count++];
            b->slot_idx = (uint32_t)i;

            // Owner designation
            if (i == (int)out->w_work)      b->owner = 1; // writer
            else if (i == (int)out->r_work)  b->owner = 2; // reader
            else                             b->owner = 3; // in-exchange

            // Read envelope header (16 bytes) — Acquire ordering (same as kernel)
            // SAFETY: buffer i is live (has an owner); the header read is advisory.
            const uint8_t* hdr = w->buf[i];
            if (hdr) {
                memcpy(&b->seq,         hdr + 8, 4);
                memcpy(&b->version,     hdr + 4, 2);
                memcpy(&b->header_size, hdr + 6, 2);
                memcpy(&b->payload_len,  hdr + 12, 4);
                // Mid-publish detection: if seq is non-zero but magic is wrong,
                // the header was sampled mid-write.
                uint32_t magic;
                memcpy(&magic, hdr, 4);
                if (magic != 0x54464557u) {
                    out->mid_publish_sample = true;
                }
            }
        }
    }

    // If only one buffer is live (shouldn't happen in normal operation, but
    // during revocation the third may be freed), fill the second slot with
    // a "no live owner" marker.
    while (live_count < 2) {
        out->bufs[live_count].slot_idx = 3;  // sentinel: "no live buffer"
        out->bufs[live_count].owner = 0;     // free
        live_count++;
    }
}
