// verified_mb.h — VerifiedWeft multi-buffer batch verification (SIMD lanes).
//
// WHY EXISTS: weft_vw_batch_decode_verify (Series 6) verifies records
// serially: each record costs ~4-5 sequential SHA-256 compressions through
// the single-stream pipeline (ipad re-seed, variable data, outer finish),
// even with SHA-NI/ARM-CE active. The records in a batch are INDEPENDENT
// messages under ONE key — the textbook multi-buffer shape. This module
// verifies up to 8 (AVX2) / 4 (NEON) records per pass through the vector
// registers via sha256_mb, with the ipad/opad blocks amortized ONCE per key
// instead of once per record.
//
// SEMANTIC CONTRACT — identical to weft_vw_batch_decode_verify, by test
// (VMB3/VMB4 sweep both): same result codes in the same situations, same
// stop-at-first-bad-record behavior, same *n_verified / *bytes_consumed
// meaning, same zero-copy views. Acceleration changes WHERE the cycles go,
// never what a consumer may conclude. The scalar fallback (no vector
// backend) produces the same results at serial speed.
//
// NEW CAPABILITY — weft_vw_scan_magic: SIMD scan for the next "WEFT"
// envelope magic at ANY byte offset. The serial batch path checks magic only
// at record boundaries and stops at the first bad one; a stream consumer
// that wants to RESYNC past a corrupted span (crash-tolerant .weftrec
// ingestion) had no in-tree primitive for it. The scan is vectorized on
// AVX2 (byte-mask AND chain) and NEON (phase-shifted dword compares),
// scalar elsewhere, and is what the resync idiom builds on:
//
//   while (off + 48 <= len) {
//     r = batch_decode_verify_mb(key, src+off, len-off, ...);
//     off += *bytes_consumed;
//     if (r != OK) { if (!scan_magic(src, len, off+1, &off2)) break; off = off2; }
//   }
//
// Layer discipline: driver layer; weft.c/weft.h untouched; no allocation on
// any path here (per-batch staging lives on the caller's stack frames via
// the fixed-size local in the entry point).

#ifndef WEFT_VERIFIED_MB_H
#define WEFT_VERIFIED_MB_H

#include <stddef.h>
#include <stdint.h>

#include "verified.h"

/// Payload cap for the SIMD verify path: records at or under this size go
/// through the lane-batched staging; larger records (and the rest of their
/// stream) fall back to the serial pre-keyed verifier with identical
/// semantics — the pad-block amortization they give up is negligible at that
/// size, and the stack staging stays bounded (declared, not hidden).
#define WEFT_VW_MB_MAX_PAYLOAD 4096

/// Pre-keyed multi-buffer verifier state. inner_after_ipad / outer_after_opad
/// are the H states after compressing the HMAC pad blocks — the amortization
/// that makes the per-record cost "variable data + outer finish" only.
/// No allocation; init once per (key, stream).
typedef struct weft_vw_mb {
    uint32_t inner_after_ipad[8];
    uint32_t outer_after_opad[8];
} weft_vw_mb_t;

/// Pre-key from a derived auth key (weft_vw_derive_key output).
void weft_vw_mb_init(weft_vw_mb_t* v, const uint8_t auth_key[WEFT_VW_KEY_LEN]);

/// Multi-buffer mirror of weft_vw_batch_decode_verify — identical semantics
/// (see the header's contract block); verification runs in vector-lane
/// batches when the CPU offers a multi-buffer backend, and routes to the
/// serial implementation otherwise (identical results, serial speed).
weft_vw_result_t weft_vw_batch_decode_verify_mb(const uint8_t auth_key[WEFT_VW_KEY_LEN],
                                                const uint8_t* src, size_t src_len,
                                                weft_vw_record_view_t* views,
                                                size_t views_cap,
                                                size_t* n_verified,
                                                size_t* bytes_consumed);

/// Find the next offset >= from where src[o..o+4] equals the envelope magic
/// "WEFT". Returns 1 with *out_off set, 0 when no candidate exists. Bounds:
/// only offsets with o+4 <= len are candidates. Vectorized on AVX2/NEON.
int weft_vw_scan_magic(const uint8_t* src, size_t len, size_t from, size_t* out_off);

#endif // WEFT_VERIFIED_MB_H
