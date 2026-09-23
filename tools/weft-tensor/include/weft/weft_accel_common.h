// weft_accel_common.h — RFC-0017 §2.2: shared accelerator plumbing.
//
// WHY EXISTS: all four backends share the same discipline and the same
// support machinery — an honest capability path label per road (Law 4),
// a monotonic nanosecond clock with a fixed-window latency histogram
// (the bench's p50/p95/p99 without a single allocation — Law 1), the
// frozen-kernel identity hash (Law 3: a kernel is identified by the
// bytes the pipeline was built from; the ID asserted at init catches a
// wrong/old shader linked at runtime, complementing CI's byte-identity
// rebuild gate), and the SIMD [FALLBACK-COPY] kernels (Law 4's labeled
// copy road — the SAME normalize math as the frozen GPU kernels so the
// cross-domain bit-exactness gate is exact equality, not tolerance).
//
// THE NORMALIZE CONTRACT (bit-exactness by construction): the camera
// preprocess road everywhere in this pillar is
//     out[i] = (f32)src[i] * scale        // ONE IEEE-754 multiply
// u8 -> f32 conversion is exact, a single multiply is a single rounded
// op on every IEEE platform (CPU scalar, AVX2/AVX-512/NEON, GLSL/WGSL/
// MSL), and there is no add for a compiler to fuse into an FMA — so
// scalar, SIMD and GPU outputs are bit-identical and the gate is ==.
// (A bias/affine road exists in the kernels but is deliberately frozen
// OUT of v1's cross-domain gate: fma(v, s, b) vs mul-then-add may differ
// in the last ulp between shader compilers; when a bias is needed the
// per-backend tests document the 1-ulp tolerance instead of pretending.)

#ifndef WEFT_ACCEL_COMMON_H
#define WEFT_ACCEL_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "weft_tensor_view.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capability paths (Law 4 — every road carries its label)
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_ACCEL_UNAVAILABLE = 0,  ///< no hardware/runtime; refuse (or the
                                 ///< bench's declared SIM road)
    WEFT_ACCEL_ZERO_COPY = 1,    ///< the buffer the producer made IS the
                                 ///< buffer the accelerator consumes
    WEFT_ACCEL_FALLBACK_COPY = 2,///< labeled SIMD copy (alignment/ext/
                                 ///< endianness refusal — honest, never
                                 ///< silent; printed as [FALLBACK-COPY])
} weft_accel_path_t;

const char* weft_accel_path_name(weft_accel_path_t p);

// ---------------------------------------------------------------------------
// Clock + fixed-window latency stats (Law 1: zero allocations)
// ---------------------------------------------------------------------------

/// Monotonic nanoseconds (CLOCK_MONOTONIC).
uint64_t weft_accel_ns_now(void);

/// Fixed-capacity sample window (no heap; caller-owned).
#define WEFT_ACCEL_STAT_MAX 4096u
typedef struct {
    uint64_t samples[WEFT_ACCEL_STAT_MAX];
    uint32_t n;          ///< samples collected (capped)
    uint32_t overflow;   ///< samples dropped past capacity (counted)
} weft_accel_stats_t;

void           weft_accel_stats_reset(weft_accel_stats_t* s);
void           weft_accel_stats_add(weft_accel_stats_t* s, uint64_t ns);
uint32_t       weft_accel_stats_count(const weft_accel_stats_t* s);
/// Percentile (0..100) over the collected window; 0 when empty. p50/p95/
/// p99 are linear interpolations over the sorted window — sorted into a
/// SECOND caller-provided buffer (the live window stays append-only).
uint64_t weft_accel_stats_pct(const weft_accel_stats_t* s,
                              weft_accel_stats_t* scratch, double pct);

// ---------------------------------------------------------------------------
// Frozen-kernel identity (Law 3)
// ---------------------------------------------------------------------------

/// FNV-1a 64 over the kernel bytes (SPIR-V, MSL source, or WGSL text).
/// Every backend's init computes this over the bytes it actually built
/// its pipeline from and compares against the compile-time expected ID
/// in its header — a mismatch is a hard refusal (wrong/old kernel), the
/// runtime mirror of CI's committed-.spv byte-identity gate.
uint64_t weft_accel_frozen_id(const void* kernel_bytes, size_t n);

// ---------------------------------------------------------------------------
// SIMD kernels — the [FALLBACK-COPY] road + the scalar reference
// ---------------------------------------------------------------------------

/// The preprocessing kernel contract shared by every domain (see the
/// header's normalize contract): `n` u8 samples (interleaved RGBA when
/// src is a camera frame) become `n` f32 samples, each exactly
/// (f32)src[i] * scale. dst must be 4*n bytes, n a multiple of 4 when
/// the input is RGBA (the kernel processes whole pixels).
void weft_simd_normalize_u8_to_f32(float* dst, const uint8_t* src,
                                   size_t n, float scale);

/// Plain f32 copy (the fallback road's transport; AVX2/AVX-512/NEON when
/// the silicon offers it, memcpy-grade otherwise).
void weft_simd_copy_f32(float* dst, const float* src, size_t n);

/// The scalar reference — the oracle the SIMD and GPU legs are compared
/// against, BIT-EXACT (the contract guarantees it: one exact conversion
/// + one rounded multiply).
void weft_ref_normalize_u8_to_f32(float* dst, const uint8_t* src,
                                  size_t n, float scale);

/// Which ISA the SIMD dispatcher resolved ("scalar", "avx2", "avx512",
/// "neon") — evidence lines carry it (AXIOM T: advisory).
const char* weft_simd_isa_name(void);

// ---------------------------------------------------------------------------
// Law 1 audit — the zero-allocation window gate
// ---------------------------------------------------------------------------

/// Install malloc/free counting hooks (glibc). While installed, every
/// malloc/free/calloc/realloc increments a counter. The ONNX/ggml pooled
/// run gates assert the counter does not move across a hot-path call.
/// Returns 0 on success, -1 when hooks are unavailable (non-glibc — the
/// gate skips itself and SAYS so; never pretends).
int  weft_accel_alloc_audit_install(void);
void weft_accel_alloc_audit_remove(void);
uint64_t weft_accel_alloc_count(void);
/// "malloc(3) hooked" or "not hooked (gate self-skips)" for logs.
const char* weft_accel_alloc_audit_state(void);

#ifdef __cplusplus
}
#endif

#endif // WEFT_ACCEL_COMMON_H
