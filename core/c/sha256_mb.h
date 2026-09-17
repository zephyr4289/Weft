// sha256_mb.h — Multi-buffer SHA-256 (8-way AVX2 / 4-way NEON), driver layer.
//
// WHY EXISTS: RFC 0005's batch decode-verify path (weft_vw_batch_decode_verify,
// Series 6) verifies records SERIALLY: per record ~4-5 sequential compressions
// (ipad re-seed, variable data, outer finish). The single-stream accelerations
// in sha256_hw.c (SHA-NI / ARM CE) speed each compression but keep the
// serialization. This module processes N INDEPENDENT message streams in one
// pass through the vector registers — the classic multi-buffer shape — which
// is what the batch path actually wants: 8 records' inner hashes advance
// together, 8 outer hashes finish together.
//
// LANE PACKING: __m256i lane j (dword j) carries message j's state word /
// schedule word. All lanes execute the SAME round sequence on DIFFERENT data —
// digests are bit-identical to the scalar reference by construction (same
// FIPS 180-4 §6.2.2 structure, re-derived; see sha256_mb.c provenance note).
//
// SNAPSHOT SEMANTICS: lanes may declare different block counts. Lane j's
// output state is snapshotted immediately after its LAST block's feed-forward;
// later groups (other lanes still running) load a zero block into that lane
// and its register state becomes garbage — the snapshot is already out. This
// is what lets the HMAC orchestration batch records of mixed payload length.
//
// Layer discipline: driver-layer module. weft.c/weft.h untouched; no
// allocation; intrinsics headers only (no new library dependencies).
//
// Honesty boundary: the AVX2 path is executable-verified in this sandbox
// (AVX2 present; VMB sweeps both regimes). The NEON 4-way path is
// compile-guarded for aarch64 and is NOT executable-tested on the x86_64
// sandbox — declared, per the repo's per-port honesty culture (mirrors
// sha256_hw.c's ARM CE declaration).

#ifndef WEFT_SHA256_MB_H
#define WEFT_SHA256_MB_H

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"  // SHA256_BLOCK_LEN

/// Maximum lanes any backend offers (AVX2). NEON offers 4.
#define WEFT_SHA256_MB_MAX_LANES 8

/// Which multi-buffer backend this build + CPU offer.
typedef enum {
    WEFT_SHA256_MB_NONE = 0,     // no vector backend: use the scalar lane loop
    WEFT_SHA256_MB_X86_AVX2 = 1, // 8-way, YMM lanes (runtime AVX2 probe)
    WEFT_SHA256_MB_ARM_NEON = 2, // 4-way, NEON q-regs (compile-guarded aarch64)
} weft_sha256_mb_impl_t;

/// Lanes the active backend offers (8 AVX2, 4 NEON, 0 none).
int weft_sha256_mb_lanes(void);

/// What the multi-buffer dispatch currently uses (tests/bench report this so
/// evidence logs name the regime).
weft_sha256_mb_impl_t weft_sha256_mb_active_impl(void);

/// Pin the scalar lane loop (A/B benchmarking, conformance sweeps). The lane
/// loop calls the Series-6 single-stream transform per lane, so "scalar" here
/// still honors the Series-6 force pins — a pinned-scalar single-stream table
/// gives a fully scalar multi-buffer reference. Test/bench-only API.
void weft_sha256_mb_force_scalar(void);

/// Restore runtime vector dispatch (the default state).
void weft_sha256_mb_force_auto(void);

/// Compress `nblocks[j]` blocks through lane j in parallel.
///
///   state_in[j]  initial 8-word state of lane j (H0 or a pre-keyed state)
///   msg[j]       lane j's contiguous block sequence; NULL/unused when
///                nblocks[j] == 0
///   state_out[j] lane j's state after its nblocks[j]-th block (snapshot —
///                see header); a copy of state_in when nblocks[j] == 0
///
/// `lanes` must equal weft_sha256_mb_lanes() when a vector backend is active
/// (8 or 4); any lanes value in [1, 8] is legal under the forced-scalar
/// reference (the lane loop has no packing constraint) — that asymmetry is
/// what lets tests sweep 1..8 lane counts against the reference. Returns 0 on
/// success, -1 on an unsupported lanes value for the active backend.
///
/// Zero allocation. All lanes share the round structure; no lane observes
/// another lane's data (independent streams by construction).
int weft_sha256_mb(uint32_t state_out[/*lanes*/][8],
                   const uint32_t state_in[/*lanes*/][8],
                   const uint8_t* const msg[/*lanes*/],
                   const size_t nblocks[/*lanes*/],
                   int lanes);

#endif // WEFT_SHA256_MB_H
