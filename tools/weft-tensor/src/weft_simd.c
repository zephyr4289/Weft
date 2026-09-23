// weft_simd.c — RFC-0017 §2.3: the SIMD [FALLBACK-COPY] kernels.
//
// WHY EXISTS: Law 4 demands the copy road be not just honest (labeled
// [FALLBACK-COPY]) but FAST — when an accelerator or its import road
// refuses (alignment below 16 B, extension absent, big-endian payload),
// the producer's bytes still need to reach the runtime's memory at memory
// bandwidth, not loop-at-a-time. These kernels are that road, and they
// are also the cross-domain ORACLE: the normalize math is the header's
// bit-exactness contract (one exact u8->f32 conversion + ONE rounded
// multiply), so scalar == SIMD == GPU is an ==-gate, not a tolerance.
//
// ISA DISPATCH: x86_64 resolves AVX2 at runtime via __builtin_cpu_supports
// (target attributes — no global -march; the binary stays baseline-portable
// and lights up per-host). aarch64 uses NEON when the compiler exposes it.
// AVX-512 is deliberately NOT claimed for the normalize road: u8->f32 at
// 16-wide lanes is already the conversion bottleneck and AVX-512 would be
// speed theater, not speed (the copy road uses it neither — 256-bit stores
// saturate the write ports at these sizes). Every claim here is about
// STRUCTURE (which ISA serves the road), not marketing numbers (AXIOM T).

#include "weft/weft_accel_common.h"

#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#  include <immintrin.h>
#  define WEFT_SIMD_X86 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
#  include <arm_neon.h>
#  define WEFT_SIMD_ARM 1
#endif

// ---------------------------------------------------------------------------
// Scalar reference (the oracle)
// ---------------------------------------------------------------------------

void weft_ref_normalize_u8_to_f32(float* dst, const uint8_t* src,
                                  size_t n, float scale) {
    for (size_t i = 0; i < n; i++) {
        dst[i] = (float)src[i] * scale;  // exact cvt + ONE rounded multiply
    }
}

// ---------------------------------------------------------------------------
// x86_64 / AVX2 (runtime-resolved; target attributes keep the binary
// baseline-portable — no global -mavx2 anywhere in the build)
// ---------------------------------------------------------------------------

#ifdef WEFT_SIMD_X86

__attribute__((target("avx2")))
static void normalize_avx2(float* dst, const uint8_t* src, size_t n,
                           float scale) {
    size_t i = 0;
    const __m256 vs = _mm256_set1_ps(scale);
    for (; i + 16 <= n; i += 16) {
        __m128i bytes = _mm_loadu_si128((const __m128i*)(src + i)); // 16 u8
        __m256i w0 = _mm256_cvtepu8_epi32(bytes);                   // lo 8 u32
        __m256i w1 = _mm256_cvtepu8_epi32(
            _mm_unpackhi_epi64(bytes, bytes));                      // hi 8 u32
        _mm256_storeu_ps(dst + i,      _mm256_mul_ps(_mm256_cvtepi32_ps(w0), vs));
        _mm256_storeu_ps(dst + i + 8,  _mm256_mul_ps(_mm256_cvtepi32_ps(w1), vs));
    }
    for (; i < n; i++) {
        dst[i] = (float)src[i] * scale;
    }
}

__attribute__((target("avx2")))
static void copy_avx2(float* dst, const float* src, size_t n) {
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        _mm256_storeu_ps(dst + i,      _mm256_loadu_ps(src + i));
        _mm256_storeu_ps(dst + i + 8,  _mm256_loadu_ps(src + i + 8));
        _mm256_storeu_ps(dst + i + 16, _mm256_loadu_ps(src + i + 16));
        _mm256_storeu_ps(dst + i + 24, _mm256_loadu_ps(src + i + 24));
    }
    for (; i < n; i++) {
        dst[i] = src[i];
    }
}

#endif // WEFT_SIMD_X86

// ---------------------------------------------------------------------------
// aarch64 / NEON
// ---------------------------------------------------------------------------

#ifdef WEFT_SIMD_ARM

static void normalize_neon(float* dst, const uint8_t* src, size_t n,
                           float scale) {
    size_t i = 0;
    const float32x4_t vs = vdupq_n_f32(scale);
    for (; i + 8 <= n; i += 8) {
        uint8x8_t  b  = vld1_u8(src + i);
        uint16x8_t w  = vmovl_u8(b);            // u8 -> u16 (exact)
        uint32x4_t lo = vmovl_u16(vget_low_u16(w));
        uint32x4_t hi = vmovl_u16(vget_high_u16(w));
        vst1q_f32(dst + i,     vmulq_f32(vcvtq_f32_u32(lo), vs));
        vst1q_f32(dst + i + 4, vmulq_f32(vcvtq_f32_u32(hi), vs));
    }
    for (; i < n; i++) {
        dst[i] = (float)src[i] * scale;
    }
}

static void copy_neon(float* dst, const float* src, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        vst1q_f32(dst + i,     vld1q_f32(src + i));
        vst1q_f32(dst + i + 4, vld1q_f32(src + i + 4));
    }
    for (; i < n; i++) {
        dst[i] = src[i];
    }
}

#endif // WEFT_SIMD_NEON

// ---------------------------------------------------------------------------
// Dispatch (resolved once; cached; advisory name for evidence)
// ---------------------------------------------------------------------------

enum {
    WEFT_SIMD_SCALAR = 0,
    WEFT_SIMD_AVX2 = 1,
    WEFT_SIMD_NEON = 2,
};

static int weft_simd_resolve(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
#if defined(WEFT_SIMD_X86)
    cached = __builtin_cpu_supports("avx2") ? WEFT_SIMD_AVX2 : WEFT_SIMD_SCALAR;
#elif defined(WEFT_SIMD_ARM)
    cached = WEFT_SIMD_NEON;
#else
    cached = WEFT_SIMD_SCALAR;
#endif
    return cached;
}

const char* weft_simd_isa_name(void) {
    switch (weft_simd_resolve()) {
    case WEFT_SIMD_AVX2: return "avx2";
    case WEFT_SIMD_NEON: return "neon";
    default:             return "scalar";
    }
}

void weft_simd_normalize_u8_to_f32(float* dst, const uint8_t* src,
                                   size_t n, float scale) {
    if (!dst || !src) return;
#if defined(WEFT_SIMD_X86)
    if (weft_simd_resolve() == WEFT_SIMD_AVX2) {
        normalize_avx2(dst, src, n, scale);
        return;
    }
#elif defined(WEFT_SIMD_ARM)
    if (weft_simd_resolve() == WEFT_SIMD_NEON) {
        normalize_neon(dst, src, n, scale);
        return;
    }
#endif
    weft_ref_normalize_u8_to_f32(dst, src, n, scale);
}

void weft_simd_copy_f32(float* dst, const float* src, size_t n) {
    if (!dst || !src) return;
    if (dst == src) return;
#if defined(WEFT_SIMD_X86)
    if (weft_simd_resolve() == WEFT_SIMD_AVX2) {
        copy_avx2(dst, src, n);
        return;
    }
#elif defined(WEFT_SIMD_ARM)
    if (weft_simd_resolve() == WEFT_SIMD_NEON) {
        copy_neon(dst, src, n);
        return;
    }
#endif
    memcpy(dst, src, n * sizeof(float));
}
