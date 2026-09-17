// sha256_mb.h — Multi-buffer SHA-256 (16-way AVX-512 / 8-way AVX2 / 4-way NEON),
// driver layer.
//
// WHY EXISTS: RFC 0005's batch decode-verify path (weft_vw_batch_decode_verify,
// Series 6) verifies records SERIALLY: per record ~4-5 sequential compressions
// (ipad re-seed, variable data, outer finish). The single-stream accelerations
// in sha256_hw.c (SHA-NI / ARM CE) speed each compression but keep the
// serialization. This module processes N INDEPENDENT message streams in one
// pass through the vector registers — the classic multi-buffer shape — which
// is what the batch path actually wants: 16 records' inner hashes advance
// together, 16 outer hashes finish together.
//
// LANE PACKING: the vector register's dword j carries message j's state word /
// schedule word (16 dwords per zmm under AVX-512, 8 under AVX2, 4 per NEON
// q-register). All lanes execute the SAME round sequence on DIFFERENT data —
// digests are bit-identical to the scalar reference by construction (same
// FIPS 180-4 §6.2.2 structure, re-derived; see sha256_mb.c provenance note).
//
// AVX-512 (Series 8): the 16-lane kernel is not just "AVX2, wider". The ISA
// gives SHA-256's round algebra single instructions for what AVX2 spells in
// threes and fives: Ch is one vpternlogd (imm 0xCA) instead of and/andnot/
// xor, Maj one (imm 0xE8) instead of three ands + two xors, every big-sigma /
// small-sigma is one vprord rotate per term instead of shift+shift+or, and
// the three-term XORs collapse into one vpternlogd (imm 0x96). Instruction
// count per compressed byte drops ~3.3x against the AVX2 kernel on top of
// the doubled lane width — the multi-gigabyte-per-core authenticated-stream
// target RFC-0012 measures. Structure stays the review-symmetric W[64] shape
// (see sha256_mb.c) so the bit-identity argument remains checkable by eye.
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
// Honesty boundary: the AVX2 and AVX-512 paths are executable-verified in
// this sandbox (AVX2 + AVX512F present; VMB sweeps every regime; VMB9
// cross-checks the available x86 kernels against each other). The NEON 4-way
// path is compile-guarded for aarch64 and is NOT executable-tested on the
// x86_64 sandbox — declared, per the repo's per-port honesty culture (mirrors
// sha256_hw.c's ARM CE declaration).

#ifndef WEFT_SHA256_MB_H
#define WEFT_SHA256_MB_H

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"  // SHA256_BLOCK_LEN

/// Maximum lanes any backend offers (AVX-512). AVX2 offers 8; NEON 4.
#define WEFT_SHA256_MB_MAX_LANES 16

/// Which multi-buffer backend this build + CPU offer.
typedef enum {
    WEFT_SHA256_MB_NONE = 0,       // no vector backend: use the scalar lane loop
    WEFT_SHA256_MB_X86_AVX2 = 1,   // 8-way, YMM lanes (runtime AVX2 probe)
    WEFT_SHA256_MB_ARM_NEON = 2,   // 4-way, NEON q-regs (compile-guarded aarch64)
    WEFT_SHA256_MB_X86_AVX512 = 3, // 16-way, ZMM lanes (runtime AVX512F probe)
} weft_sha256_mb_impl_t;

/// Lanes the active backend offers (16 AVX-512, 8 AVX2, 4 NEON, 0 none).
int weft_sha256_mb_lanes(void);

/// What the multi-buffer dispatch currently uses (tests/bench report this so
/// evidence logs name the regime).
weft_sha256_mb_impl_t weft_sha256_mb_active_impl(void);

/// Whether `impl` is compiled in for this build AND offered by this CPU
/// (probe only; NEVER a correctness input — the scalar lane loop is always
/// legal). Tests and benches use this to pin or skip per-impl regimes safely.
int weft_sha256_mb_available(weft_sha256_mb_impl_t impl);

/// Pin the scalar lane loop (A/B benchmarking, conformance sweeps). The lane
/// loop calls the Series-6 single-stream transform per lane, so "scalar" here
/// still honors the Series-6 force pins — a pinned-scalar single-stream table
/// gives a fully scalar multi-buffer reference. Test/bench-only API.
void weft_sha256_mb_force_scalar(void);

/// Pin a SPECIFIC vector backend (A/B benchmarking, cross-impl bit-identity
/// gates). Callers must gate on weft_sha256_mb_available(impl): pinning an
/// impl the CPU cannot execute would fault when its kernel runs — the same
/// discipline as every force_* in this tree (documented contract, test/bench
/// only). Returns 0 when the impl is compiled in (execution safety remains
/// the caller's availability check), -1 when this build lacks it entirely.
int weft_sha256_mb_force_impl(weft_sha256_mb_impl_t impl);

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
/// (16, 8 or 4); any lanes value in [1, 16] is legal under the forced-scalar
/// reference (the lane loop has no packing constraint) — that asymmetry is
/// what lets tests sweep 1..16 lane counts against the reference. Returns 0 on
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
