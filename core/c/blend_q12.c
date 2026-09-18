// blend_q12.c — SIMD-accelerated Q12 raster alpha-blend (see blend_q12.h).
//
// The scalar reference IS the spec: bit-for-bit the arithmetic of
// GovernedFanoutConsumer.blendQ12 (Kotlin), cadence.ts (TS), Governor.swift
// and governor.dart — per channel ((a*inv + b*alpha) >> 12) on the four
// 8-bit channels of each little-endian RGBA8888 word, inv = 4096 - alpha.
// Every vector path must reproduce it exactly; blend_test.c enforces that
// on every build (scalar vs sse41 vs avx2 vs neon across the full alpha
// range on random buffers), so a divergent path is a build failure, not a
// rendering artifact someone discovers on a device.

#include "blend_q12.h"

#include <stdio.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

#define WEFT_BLEND_ONE_Q12 4096u

// ---------------------------------------------------------------------------
// Scalar reference (the spec).
// ---------------------------------------------------------------------------

void weft_blend_q12_scalar(const uint32_t *prev, const uint32_t *newest,
                           uint32_t *out, size_t words, unsigned alpha_q12) {
    if (alpha_q12 > WEFT_BLEND_ONE_Q12) alpha_q12 = WEFT_BLEND_ONE_Q12;
    const unsigned inv = WEFT_BLEND_ONE_Q12 - alpha_q12;
    for (size_t i = 0; i < words; i++) {
        const uint32_t a = prev[i];
        const uint32_t b = newest[i];
        const uint32_t r = (((a & 0xffu) * inv + (b & 0xffu) * alpha_q12) >> 12);
        const uint32_t g = ((((a >> 8) & 0xffu) * inv +
                             ((b >> 8) & 0xffu) * alpha_q12) >> 12);
        const uint32_t bl = ((((a >> 16) & 0xffu) * inv +
                              ((b >> 16) & 0xffu) * alpha_q12) >> 12);
        const uint32_t al = ((((a >> 24) & 0xffu) * inv +
                              ((b >> 24) & 0xffu) * alpha_q12) >> 12);
        out[i] = r | (g << 8) | (bl << 16) | (al << 24);
    }
}

#if defined(__x86_64__) || defined(__i386__)
// ---------------------------------------------------------------------------
// x86: SSE4.1 (16 bytes / 4 pixels per op) and AVX2 (32 bytes / 8 pixels).
//
// Lane plan per pixel (32-bit lane): split the RGBA word into two 16-bit
// sublanes — [r, 0] and [b, 0] via the 0x00FF00FF mask on the raw word and
// on the >>8 word for [g, 0] / [a, 0]. Each channel sits in a signed-16
// sublane whose partner is ZERO, so _mm_madd_epi16 with the weight packed
// into the matching sublane ([inv, 0] = set1_epi32(inv) for inv < 2^16)
// multiplies exactly one channel per madd — no overflow (products <=
// 255*4096 < 2^20), no cross-channel mixing. Numerators for the four
// channels live in four separate vectors, are shifted right by 12, and are
// recombined with byte shifts: out = R | G<<8 | B<<16 | A<<24.
// ---------------------------------------------------------------------------

__attribute__((target("sse4.1")))
void weft_blend_q12_sse41(const uint32_t *prev, const uint32_t *newest,
                          uint32_t *out, size_t words, unsigned alpha_q12) {
    if (alpha_q12 > WEFT_BLEND_ONE_Q12) alpha_q12 = WEFT_BLEND_ONE_Q12;
    const unsigned inv = WEFT_BLEND_ONE_Q12 - alpha_q12;
    const __m128i mask = _mm_set1_epi32((int)0x00FF00FFu);
    const __m128i wLo = _mm_set1_epi32((int)inv);          /* sublane [inv, 0] */
    const __m128i wHi = _mm_set1_epi32((int)(inv << 16));  /* sublane [0, inv] */
    const __m128i aLo = _mm_set1_epi32((int)alpha_q12);
    const __m128i aHi = _mm_set1_epi32((int)(alpha_q12 << 16));
    size_t i = 0;
    for (; i + 4 <= words; i += 4) {
        const __m128i va = _mm_loadu_si128((const __m128i *)(prev + i));
        const __m128i vb = _mm_loadu_si128((const __m128i *)(newest + i));
        const __m128i aL = _mm_and_si128(va, mask);
        const __m128i bL = _mm_and_si128(vb, mask);
        const __m128i aH = _mm_and_si128(_mm_srli_epi32(va, 8), mask);
        const __m128i bH = _mm_and_si128(_mm_srli_epi32(vb, 8), mask);
        /* numerators per channel (each <= 2*255*4096 < 2^21, exact) */
        const __m128i nR = _mm_add_epi32(_mm_madd_epi16(aL, wLo),
                                         _mm_madd_epi16(bL, aLo));
        const __m128i nB = _mm_add_epi32(_mm_madd_epi16(aL, wHi),
                                         _mm_madd_epi16(bL, aHi));
        const __m128i nG = _mm_add_epi32(_mm_madd_epi16(aH, wLo),
                                         _mm_madd_epi16(bH, aLo));
        const __m128i nA = _mm_add_epi32(_mm_madd_epi16(aH, wHi),
                                         _mm_madd_epi16(bH, aHi));
        const __m128i r = _mm_srli_epi32(nR, 12);
        const __m128i g = _mm_srli_epi32(nG, 12);
        const __m128i b = _mm_srli_epi32(nB, 12);
        const __m128i a = _mm_srli_epi32(nA, 12);
        const __m128i px = _mm_or_si128(
            _mm_or_si128(r, _mm_slli_epi32(g, 8)),
            _mm_or_si128(_mm_slli_epi32(b, 16), _mm_slli_epi32(a, 24)));
        _mm_storeu_si128((__m128i *)(out + i), px);
    }
    if (i < words) {
        weft_blend_q12_scalar(prev + i, newest + i, out + i, words - i,
                              alpha_q12);
    }
}

__attribute__((target("avx2")))
void weft_blend_q12_avx2(const uint32_t *prev, const uint32_t *newest,
                         uint32_t *out, size_t words, unsigned alpha_q12) {
    if (alpha_q12 > WEFT_BLEND_ONE_Q12) alpha_q12 = WEFT_BLEND_ONE_Q12;
    const unsigned inv = WEFT_BLEND_ONE_Q12 - alpha_q12;
    const __m256i mask = _mm256_set1_epi32((int)0x00FF00FFu);
    const __m256i wLo = _mm256_set1_epi32((int)inv);
    const __m256i wHi = _mm256_set1_epi32((int)(inv << 16));
    const __m256i aLo = _mm256_set1_epi32((int)alpha_q12);
    const __m256i aHi = _mm256_set1_epi32((int)(alpha_q12 << 16));
    size_t i = 0;
    for (; i + 8 <= words; i += 8) {
        const __m256i va = _mm256_loadu_si256((const __m256i *)(prev + i));
        const __m256i vb = _mm256_loadu_si256((const __m256i *)(newest + i));
        const __m256i aL = _mm256_and_si256(va, mask);
        const __m256i bL = _mm256_and_si256(vb, mask);
        const __m256i aH = _mm256_and_si256(_mm256_srli_epi32(va, 8), mask);
        const __m256i bH = _mm256_and_si256(_mm256_srli_epi32(vb, 8), mask);
        const __m256i nR = _mm256_add_epi32(_mm256_madd_epi16(aL, wLo),
                                            _mm256_madd_epi16(bL, aLo));
        const __m256i nB = _mm256_add_epi32(_mm256_madd_epi16(aL, wHi),
                                            _mm256_madd_epi16(bL, aHi));
        const __m256i nG = _mm256_add_epi32(_mm256_madd_epi16(aH, wLo),
                                            _mm256_madd_epi16(bH, aLo));
        const __m256i nA = _mm256_add_epi32(_mm256_madd_epi16(aH, wHi),
                                            _mm256_madd_epi16(bH, aHi));
        const __m256i r = _mm256_srli_epi32(nR, 12);
        const __m256i g = _mm256_srli_epi32(nG, 12);
        const __m256i b = _mm256_srli_epi32(nB, 12);
        const __m256i a = _mm256_srli_epi32(nA, 12);
        const __m256i px = _mm256_or_si256(
            _mm256_or_si256(r, _mm256_slli_epi32(g, 8)),
            _mm256_or_si256(_mm256_slli_epi32(b, 16), _mm256_slli_epi32(a, 24)));
        _mm256_storeu_si256((__m256i *)(out + i), px);
    }
    if (i < words) {
        weft_blend_q12_scalar(prev + i, newest + i, out + i, words - i,
                              alpha_q12);
    }
}
#endif /* x86 */

#if defined(__ARM_NEON) || defined(__aarch64__)
// ---------------------------------------------------------------------------
// ARM NEON (128-bit / 4 pixels). Same widening discipline: channels are
// widened to 32-bit lanes (vmovl), multiplied by the broadcast weights,
// summed, shifted right by 12, narrowed and re-interleaved with vsli-style
// byte shifts. Products <= 255*4096 — exact in u32 lanes.
// ---------------------------------------------------------------------------

void weft_blend_q12_neon(const uint32_t *prev, const uint32_t *newest,
                         uint32_t *out, size_t words, unsigned alpha_q12) {
    if (alpha_q12 > WEFT_BLEND_ONE_Q12) alpha_q12 = WEFT_BLEND_ONE_Q12;
    const unsigned inv = WEFT_BLEND_ONE_Q12 - alpha_q12;
    const uint32x4_t mask = vdupq_n_u32(0x00FF00FFu);
    const uint32x4_t wLo = vdupq_n_u32(inv);
    const uint32x4_t wHi = vdupq_n_u32(inv << 16);
    const uint32x4_t aLo = vdupq_n_u32(alpha_q12);
    const uint32x4_t aHi = vdupq_n_u32(alpha_q12 << 16);
    size_t i = 0;
    for (; i + 4 <= words; i += 4) {
        const uint32x4_t va = vld1q_u32(prev + i);
        const uint32x4_t vb = vld1q_u32(newest + i);
        const uint32x4_t aL = vandq_u32(va, mask);
        const uint32x4_t bL = vandq_u32(vb, mask);
        const uint32x4_t aH = vandq_u32(vshrq_n_u32(va, 8), mask);
        const uint32x4_t bH = vandq_u32(vshrq_n_u32(vb, 8), mask);
        /* per-lane channel extraction (r,g at bits 0-7 via masks; b,a at
         * bits 16-23 via mask+shift — pure u32 lane values afterwards) */
        const uint32x2_t aLl = vget_low_u32(aL);
        const uint32x2_t aLh = vget_high_u32(aL);
        const uint32x2_t bLl = vget_low_u32(bL);
        const uint32x2_t bLh = vget_high_u32(bL);
        const uint32x2_t aHl = vget_low_u32(aH);
        const uint32x2_t aHh = vget_high_u32(aH);
        const uint32x2_t bHl = vget_low_u32(bH);
        const uint32x2_t bHh = vget_high_u32(bH);
        /* channels r,b live in bits 0-15 / 16-31 of each u32 lane: extract
         * them into pure u32 lane values via masks (sublane >> 0 / >> 16) */
        const uint32x2_t aRl = vand_u32(aLl, vdup_n_u32(0xffu));
        const uint32x2_t aBl = vshr_n_u32(vand_u32(aLl, vdup_n_u32(0xff0000u)), 16);
        const uint32x2_t bRl = vand_u32(bLl, vdup_n_u32(0xffu));
        const uint32x2_t bBl = vshr_n_u32(vand_u32(bLl, vdup_n_u32(0xff0000u)), 16);
        const uint32x2_t aRh = vand_u32(aLh, vdup_n_u32(0xffu));
        const uint32x2_t aBh = vshr_n_u32(vand_u32(aLh, vdup_n_u32(0xff0000u)), 16);
        const uint32x2_t bRh = vand_u32(bLh, vdup_n_u32(0xffu));
        const uint32x2_t bBh = vshr_n_u32(vand_u32(bLh, vdup_n_u32(0xff0000u)), 16);
        const uint32x2_t aGl = vand_u32(aHl, vdup_n_u32(0xffu));
        const uint32x2_t aAl = vshr_n_u32(vand_u32(aHl, vdup_n_u32(0xff0000u)), 16);
        const uint32x2_t bGl = vand_u32(bHl, vdup_n_u32(0xffu));
        const uint32x2_t bAl = vshr_n_u32(vand_u32(bHl, vdup_n_u32(0xff0000u)), 16);
        const uint32x2_t aGh = vand_u32(aHh, vdup_n_u32(0xffu));
        const uint32x2_t aAh = vshr_n_u32(vand_u32(aHh, vdup_n_u32(0xff0000u)), 16);
        const uint32x2_t bGh = vand_u32(bHh, vdup_n_u32(0xffu));
        const uint32x2_t bAh = vshr_n_u32(vand_u32(bHh, vdup_n_u32(0xff0000u)), 16);
        /* exact per-channel lane math (2 pixels per doubleword vector) */
        const uint32x2_t nRl = vmla_u32(vmul_u32(aRl, vdup_n_u32(inv)),
                                        bRl, vdup_n_u32(alpha_q12));
        const uint32x2_t nRh = vmla_u32(vmul_u32(aRh, vdup_n_u32(inv)),
                                        bRh, vdup_n_u32(alpha_q12));
        const uint32x2_t nGl = vmla_u32(vmul_u32(aGl, vdup_n_u32(inv)),
                                        bGl, vdup_n_u32(alpha_q12));
        const uint32x2_t nGh = vmla_u32(vmul_u32(aGh, vdup_n_u32(inv)),
                                        bGh, vdup_n_u32(alpha_q12));
        const uint32x2_t nBl = vmla_u32(vmul_u32(aBl, vdup_n_u32(inv)),
                                        bBl, vdup_n_u32(alpha_q12));
        const uint32x2_t nBh = vmla_u32(vmul_u32(aBh, vdup_n_u32(inv)),
                                        bBh, vdup_n_u32(alpha_q12));
        const uint32x2_t nAl = vmla_u32(vmul_u32(aAl, vdup_n_u32(inv)),
                                        bAl, vdup_n_u32(alpha_q12));
        const uint32x2_t nAh = vmla_u32(vmul_u32(aAh, vdup_n_u32(inv)),
                                        bAh, vdup_n_u32(alpha_q12));
        const uint32x2_t r0 = vshr_n_u32(nRl, 12);
        const uint32x2_t r1 = vshr_n_u32(nRh, 12);
        const uint32x2_t g0 = vshr_n_u32(nGl, 12);
        const uint32x2_t g1 = vshr_n_u32(nGh, 12);
        const uint32x2_t b0 = vshr_n_u32(nBl, 12);
        const uint32x2_t b1 = vshr_n_u32(nBh, 12);
        const uint32x2_t a0 = vshr_n_u32(nAl, 12);
        const uint32x2_t a1 = vshr_n_u32(nAh, 12);
        const uint32x2_t px0 = vorr_u32(
            vorr_u32(r0, vshl_n_u32(g0, 8)),
            vorr_u32(vshl_n_u32(b0, 16), vshl_n_u32(a0, 24)));
        const uint32x2_t px1 = vorr_u32(
            vorr_u32(r1, vshl_n_u32(g1, 8)),
            vorr_u32(vshl_n_u32(b1, 16), vshl_n_u32(a1, 24)));
        vst1_u32(out + i, px0);
        vst1_u32(out + i + 2, px1);
    }
    if (i < words) {
        weft_blend_q12_scalar(prev + i, newest + i, out + i, words - i,
                              alpha_q12);
    }
}
#endif /* ARM NEON */

// ---------------------------------------------------------------------------
// Runtime dispatch.
// ---------------------------------------------------------------------------

weft_blend_impl weft_blend_q12_best(void) {
#if defined(__ARM_NEON) || defined(__aarch64__)
    return WEFT_BLEND_NEON;
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) return WEFT_BLEND_AVX2;
    if (__builtin_cpu_supports("sse4.1")) return WEFT_BLEND_SSE41;
    return WEFT_BLEND_SCALAR;
#else
    return WEFT_BLEND_SCALAR;
#endif
}

const char *weft_blend_q12_impl_name(weft_blend_impl impl) {
    switch (impl) {
        case WEFT_BLEND_SSE41: return "sse4.1";
        case WEFT_BLEND_AVX2:  return "avx2";
        case WEFT_BLEND_NEON:  return "neon";
        default:               return "scalar";
    }
}

void weft_blend_q12(const uint32_t *prev, const uint32_t *newest,
                    uint32_t *out, size_t words, unsigned alpha_q12) {
    switch (weft_blend_q12_best()) {
#if defined(__x86_64__) || defined(__i386__)
        case WEFT_BLEND_AVX2: weft_blend_q12_avx2(prev, newest, out, words, alpha_q12); return;
        case WEFT_BLEND_SSE41: weft_blend_q12_sse41(prev, newest, out, words, alpha_q12); return;
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
        case WEFT_BLEND_NEON: weft_blend_q12_neon(prev, newest, out, words, alpha_q12); return;
#endif
        default: weft_blend_q12_scalar(prev, newest, out, words, alpha_q12); return;
    }
}
