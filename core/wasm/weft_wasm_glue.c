// weft_wasm_glue.c — the WASM (Emscripten) export surface (issue #18-1).
//
// Design notes:
//  - The frozen C core compiles to wasm32 as-is; this glue only renames the
//    surface into a compact, JS-friendly ABI (w-prefixed, no structs
//    crossing, out-params for multi-value results).
//  - Zero-copy contract: payload/ring pointers are returned as raw wasm
//    linear-memory offsets; the TS binding exposes Uint32Array/Uint8Array
//    VIEWS over the same memory — no copies between C and JS, ever.
//  - Single-threaded flavor (default): the full kernel + fan-out protocol
//    semantics (the L/F/FS/FB batteries are single-threaded or
//    writer/reader-alternating by construction). The pthread flavor
//    (-pthread, SharedArrayBuffer) is a build option for multi-worker
//    browser apps — needs COOP/COEP, same as the TS SAB port.
//  - u64 seqs cross via wasm BigInt (WASM_BIGINT=1 — Baseline everywhere
//    modern; the binding asserts it at load).

#include <stdlib.h>
#include <stdint.h>
#include "weft.h"
#include "fanout.h"
#include "fanout_batch.h"

// ---- kernel (Triad) --------------------------------------------------------

weft_t* wweft_new(size_t payload_max) {
    weft_t* w = (weft_t*)calloc(1, sizeof(weft_t));
    if (!w) return NULL;
    if (weft_init(w, payload_max) != 0) { free(w); return NULL; }
    return w;
}

void wweft_free(weft_t* w) {
    if (!w) return;
    weft_destroy(w);
    free(w);
}

int wweft_publish(weft_t* w, uint32_t seq, const uint8_t* src, size_t len) {
    if (len && weft_w_write_payload(w, src, len) != 0) return -1;
    return (int)weft_publish(w, seq, (uint32_t)len);
}

uint64_t wweft_claim_seq(weft_t* w) {
    (void)weft_r_claim(w);
    return weft_r_seq(w);
}

const uint8_t* wweft_payload_ptr(weft_t* w) {
    return weft_r_live_ptr(w, weft_r_header_size(w));
}

size_t wweft_payload_len(weft_t* w) { return weft_r_payload_len(w); }

uint32_t wweft_epoch(weft_t* w) { return weft_epoch(w); }

int wweft_revoke(weft_t* w) {
    weft_revoke(w);
    return (int)weft_epoch(w);
}

int wweft_reclaim(weft_t* w, uint32_t pre_revoke_epoch, uint32_t timeout_ms) {
    return weft_reclaim(w, pre_revoke_epoch, timeout_ms);
}

uint64_t wweft_t_publish(weft_t* w) { return weft_t_publish(w); }
uint64_t wweft_t_claim(weft_t* w) { return weft_t_claim(w); }
uint64_t wweft_t_drop(weft_t* w) { return weft_t_drop(w); }
uint64_t wweft_t_wsteps(weft_t* w) { return weft_t_wsteps(w); }
uint64_t wweft_t_rsteps(weft_t* w) { return weft_t_rsteps(w); }

// ---- fan-out (RFC-0004 + the #17-3 batch) ----------------------------------

weft_fanout_t* wfan_new(size_t payload_bytes, unsigned slot_count) {
    return weft_fanout_new(payload_bytes, slot_count);
}

void wfan_free(weft_fanout_t* f) { weft_fanout_free(f); }

uint64_t wfan_publish(weft_fanout_t* f, const void* src, size_t len) {
    if (!weft_fanout_begin(f)) return 0;
    if (weft_fanout_fill(f, src, len) < 0) return 0;
    return weft_fanout_publish(f);
}

// Batch publish: srcs/lens point to arrays IN WASM MEMORY (the binding
// builds them via HEAPU32/BigUint64Array views — zero-copy end to end).
uint64_t wfan_publish_batch(weft_fanout_t* f, const void* const* srcs,
                            const uint64_t* lens, size_t n) {
    if (n == 0 || n > 4096) return 0;
    // Heap, NOT stack: 4096 frames x 16 B is 64 KiB — an entire default
    // wasm stack. Stack arrays of that size are how wasm ports die silently.
    weft_batch_frame_t* frames =
        (weft_batch_frame_t*)malloc(n * sizeof(weft_batch_frame_t));
    if (!frames) return 0;
    for (size_t i = 0; i < n; i++) {
        frames[i].src = srcs[i];
        // lens cross as explicit u64 (wasm32's size_t is 32-bit — the
        // binding writes BigUint64Array; a size_t* here would read the
        // high half of every other entry as 0 and silently no-op fills).
        frames[i].len = (lens[i] > (uint64_t)((size_t)-1)) ? 0 : (size_t)lens[i];
    }
    const uint64_t last = weft_publish_batch(f, frames, n);
    free(frames);
    return last;
}

uint8_t* wfan_ring(weft_fanout_t* f) { return (uint8_t*)weft_fanout_ring(f); }

size_t wfan_ring_bytes(size_t payload_bytes, unsigned slot_count) {
    return weft_fanout_ring_bytes(payload_bytes, slot_count);
}

weft_fanout_reader_t* wfr_new(const void* ring, size_t ring_bytes,
                              size_t payload_bytes, unsigned slot_count) {
    return weft_fanout_reader_new(ring, ring_bytes, payload_bytes, slot_count);
}

void wfr_free(weft_fanout_reader_t* r) { weft_fanout_reader_free(r); }

// Claim; returns fresh (1/0) and writes seq/dropped through out-pointers
// (wasm-memory addresses the binding supplies).
int wfr_claim(weft_fanout_reader_t* r, uint64_t* seq_out,
              uint64_t* dropped_out) {
    const weft_fanout_claim_t* c = weft_fanout_claim(r);
    if (seq_out) *seq_out = c->seq;
    if (dropped_out) *dropped_out = c->dropped;
    return c->fresh ? 1 : 0;
}

const uint32_t* wfr_view(weft_fanout_reader_t* r) {
    return (const uint32_t*)weft_fanout_view(r);
}
