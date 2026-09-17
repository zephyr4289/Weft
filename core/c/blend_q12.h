// blend_q12.h — SIMD-accelerated Q12 raster alpha-blend, driver layer
// (Series 8; RFC-0009 §cadence PACED_INTERPOLATE / PREDICTIVE_PACED raster op).
//
// WHY EXISTS: the governed consumer's PACED raster is blend(prev, newest,
// alphaQ12/4096) per 32-bit pixel, per 8-bit channel:
//
//     out_c = (prev_c * inv + newest_c * alpha) >> 12,  inv = 4096 - alpha
//
// (the exact arithmetic of GovernedFanoutConsumer.blendQ12 / cadence.ts —
// no rounding term: every port truncates identically). At 4K that is 8.3M
// pixels x 4 channels x 2 multiplies per frame per consumer — ALU-bound in
// the scalar form. This module vectorizes the loop with the same discipline
// the repo's Series-6 multi-buffer SHA-256 established: one scalar
// REFERENCE (the spec), per-target intrinsic paths (x86 SSE4.1 + AVX2,
// ARM NEON), runtime dispatch with __builtin_cpu_supports, and a
// bit-exactness selftest that fails the build if ANY path diverges from
// the reference on ANY golden vector.
//
// OVERFLOW PROOF (why the lane math is exact, not approximate): per channel
// the numerator prev_c*inv + newest_c*alpha <= 255*4096 + 255*4096 =
// 2,090,880 < 2^21. The SSE/AVX paths widen each channel into a 32-bit
// lane before multiplying (madd over 16-bit sublanes whose products are
// <= 255*4096 < 2^20 — no signed overflow, no cross-lane carry), so every
// lane's result is the EXACT scalar value. The selftest proves it across
// the full alpha range 0..4096 on random buffers.
//
// Honesty boundary: SSE4.1/AVX2 are executable-verified in this sandbox
// (x86_64, AVX2 + AVX512F present); the NEON path compiles only on ARM
// (Apple CI / aarch64 legs) and is golden-gated there by the same selftest.
// The dispatch is per-CALL, stateless, and never allocates.

#ifndef WEFT_BLEND_Q12_H
#define WEFT_BLEND_Q12_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Blend implementation selectors (weft_blend_q12_best()). */
typedef enum {
    WEFT_BLEND_SCALAR = 0,
    WEFT_BLEND_SSE41  = 1,
    WEFT_BLEND_AVX2   = 2,
    WEFT_BLEND_NEON   = 3
} weft_blend_impl;

/* The spec (and the portable fallback): per-channel
 *   out_c = (prev_c * (4096 - alpha) + newest_c * alpha) >> 12
 * over `words` packed RGBA8888 (little-endian) pixels. alpha MUST be in
 * 0..4096 (the Q12 protocol range; values outside clamp at the boundary —
 * the cadence policies never emit them). Zero allocation. */
void weft_blend_q12_scalar(const uint32_t *prev, const uint32_t *newest,
                           uint32_t *out, size_t words, unsigned alpha_q12);

/* Runtime-dispatched best path for this CPU (scalar on unknown ISAs).
 * Same contract as the scalar reference; bit-identical output. */
void weft_blend_q12(const uint32_t *prev, const uint32_t *newest,
                    uint32_t *out, size_t words, unsigned alpha_q12);

/* Which implementation the dispatcher will use right now. */
weft_blend_impl weft_blend_q12_best(void);

/* Human-readable implementation name ("scalar"/"sse4.1"/"avx2"/"neon"). */
const char *weft_blend_q12_impl_name(weft_blend_impl impl);

/* Explicit paths (exported for the selftest; do not call directly —
 * the dispatcher owns CPU-feature gating). */
void weft_blend_q12_sse41(const uint32_t *prev, const uint32_t *newest,
                          uint32_t *out, size_t words, unsigned alpha_q12);
void weft_blend_q12_avx2(const uint32_t *prev, const uint32_t *newest,
                         uint32_t *out, size_t words, unsigned alpha_q12);
void weft_blend_q12_neon(const uint32_t *prev, const uint32_t *newest,
                         uint32_t *out, size_t words, unsigned alpha_q12);

#ifdef __cplusplus
}
#endif

#endif /* WEFT_BLEND_Q12_H */
