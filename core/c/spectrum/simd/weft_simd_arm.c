// weft_simd_arm.c — ARM64 NEON + SVE2 kernel implementations (Pillar 5, D-52).
//
// HONESTY BOUNDARY (per the repo's per-port culture): this TU compiles to
// an EMPTY translation unit outside aarch64 — the executable-verified
// surface of Pillar 5 is x86_64; NEON/SVE2 legs are compiled and oracle-
// swept on the linux-arm64 / apple-packages CI runners. Bit-exactness
// across ISAs holds by the same construction arguments as x86 (see
// weft_simd.h); the runners prove it per-ISA with the same battery.
//
// NEON (128-bit, 4x f32 / 4x u32 lanes):
//   normalize: vsubq_f32 -> vmulq_f32 (two roundings, no fused sub-mul).
//   delta codecs: 4-lane block scheme; the encoder carries the previous
//     vector in register (in-place safe), the decoder runs a 2-step
//     Hillis-Steele scan built from vextq_u32 zero-padded lane shifts.
//   dot: lane-per-output blocking with vfmaq_f32 (single-rounding fused).
//   fletcher: chunked recurrence, C=8 (16B) then C=4 (8B) chunks —
//     vmovl widening, vmulq_u32 ramp dots, vaddvq_u32 horizontals.
//
// SVE2 (vector-length agnostic; compiled only with __ARM_FEATURE_SVE2):
//   normalize/dot/fletcher go VL-wide (svsub/svmul, svmla chains,
//     svaddv horizontals). The delta codecs deliberately reuse the NEON
//     4-lane block scheme: bit-exactness of K2 is ALGEBRAIC (modular
//     integer sums are order- and grouping-free), so the codec's width
//     is a throughput knob, not a correctness surface — and SVE2's
//     cross-lane shift primitive (svext) requires a compile-time
//     immediate that a runtime VL cannot provide. Documented, honest.

#include "weft_simd.h"

#include <string.h>

#if defined(__aarch64__)

#include <arm_neon.h>

#if defined(__linux__)
#include <sys/auxv.h>
#endif

static inline uint32_t ws_a_fold32(uint32_t s) {
    while (s > 0xFFFFu) {
        s = (s & 0xFFFFu) + (s >> 16);
    }
    return s;
}

// ---------------------------------------------------------------------------
// NEON kernels
// ---------------------------------------------------------------------------

static void ws_normalize_neon(float* dst, const float* src, size_t elems,
                              float f_min, float f_rs) {
    const float32x4_t vmin = vdupq_n_f32(f_min);
    const float32x4_t vrs = vdupq_n_f32(f_rs);
    size_t i = 0;
    for (; i + 4 <= elems; i += 4) {
        float32x4_t v = vld1q_f32(src + i);
        v = vsubq_f32(v, vmin);   // two roundings, never fused
        v = vmulq_f32(v, vrs);
        vst1q_f32(dst + i, v);
    }
    for (; i < elems; i++) {
        dst[i] = (src[i] - f_min) * f_rs;
    }
}

static void ws_delta_enc_neon(uint32_t* dst, const uint32_t* src,
                              size_t elems, uint32_t seed) {
    if (elems == 0) {
        return;
    }
    if (elems == 1) {
        dst[0] = src[0] - seed;
        return;
    }
    const uint32x4_t zero = vdupq_n_u32(0);
    uint32x4_t prev = vdupq_n_u32(src[0]);   // lane 3 feeds out[0]
    size_t i = 0;
    for (; i + 4 <= elems; i += 4) {
        const uint32x4_t v = vld1q_u32(src + i);
        // [prev[3], v[0], v[1], v[2]]: vextq gives [0, v0, v1, v2], then
        // lane 0 is patched from the register-carried prev[3].
        uint32x4_t sh = vextq_u32(zero, v, 3);           // [0, v0, v1, v2]
        sh = vsetq_lane_u32(vgetq_lane_u32(prev, 3), sh, 0);
        uint32x4_t out = vsubq_u32(v, sh);
        if (i == 0) {
            out = vsetq_lane_u32(src[0] - seed, out, 0);   // src[0] - seed
        }
        vst1q_u32(dst + i, out);
        prev = v;
    }
    size_t t = i;
    uint32_t prev_s;
    if (t == 0) {
        prev_s = src[0];   // capture BEFORE the in-place dst[0] write
        dst[0] = prev_s - seed;
        t = 1;
    } else {
        prev_s = vgetq_lane_u32(prev, 3);
    }
    for (; t < elems; t++) {
        const uint32_t cur = src[t];   // capture BEFORE the in-place write
        dst[t] = cur - prev_s;
        prev_s = cur;
    }
}

static void ws_delta_dec_neon(uint32_t* dst, const uint32_t* src,
                              size_t elems, uint32_t seed) {
    uint32_t carry = seed;
    size_t i = 0;
    for (; i + 4 <= elems; i += 4) {
        uint32x4_t v = vld1q_u32(src + i);
        const uint32x4_t zero = vdupq_n_u32(0);
        // Inclusive prefix scan, steps k=1 then k=2:
        // shift down k lanes with zero fill = vextq_u32(zero, v, 4-k).
        uint32x4_t t1 = vextq_u32(zero, v, 3);   // [0, v0, v1, v2]
        v = vaddq_u32(v, t1);
        uint32x4_t t2 = vextq_u32(zero, v, 2);   // [0, 0, s0, s1]
        v = vaddq_u32(v, t2);
        const uint32_t block_total = vgetq_lane_u32(v, 3);
        const uint32x4_t out = vaddq_u32(v, vdupq_n_u32(carry));
        vst1q_u32(dst + i, out);
        carry += block_total;   // modular
    }
    for (; i < elems; i++) {
        carry = carry + src[i];
        dst[i] = carry;
    }
}

static void ws_dot_neon(float* c, const float* a, const float* b,
                        uint32_t m, uint32_t k, uint32_t n,
                        uint32_t lda, uint32_t ldb, uint32_t ldc) {
    for (uint32_t i = 0; i < m; i++) {
        const float* arow = a + (size_t)i * lda;
        float* crow = c + (size_t)i * ldc;
        uint32_t j = 0;
        for (; j + 4 <= n; j += 4) {
            float32x4_t acc = vdupq_n_f32(0.0f);
            for (uint32_t kk = 0; kk < k; kk++) {
                acc = vfmaq_f32(acc, vld1q_f32(b + (size_t)kk * ldb + j),
                                vdupq_n_f32(arow[kk]));
            }
            vst1q_f32(crow + j, acc);
        }
        for (; j < n; j++) {
            float acc = 0.0f;
            for (uint32_t kk = 0; kk < k; kk++) {
                acc = fmaf(arow[kk], b[(size_t)kk * ldb + j], acc);
            }
            crow[j] = acc;
        }
    }
}

static uint32_t ws_fletcher_neon(const void* data, size_t bytes,
                                 uint32_t stamp) {
    const unsigned char* p = (const unsigned char*)data;
    const size_t words = bytes / 2;
    uint32_t sum1 = 0;
    uint32_t sum2 = 0;
    size_t off = 0;

    // C=8 (16 bytes): widen two u16 halves to u32, ramps [8..5] and [4..1].
    static const uint32_t ramp8_lo_arr[4] = { 8, 7, 6, 5 };
    static const uint32_t ramp8_hi_arr[4] = { 4, 3, 2, 1 };
    static const uint32_t ramp4_arr[4] = { 4, 3, 2, 1 };
    const uint32x4_t ramp8l = vld1q_u32(ramp8_lo_arr);
    const uint32x4_t ramp8h = vld1q_u32(ramp8_hi_arr);
    const uint32x4_t ramp4 = vld1q_u32(ramp4_arr);

    for (; off + 8 <= words; off += 8) {
        const uint16x8_t w8 = vld1q_u16((const uint16_t*)(const void*)(p + off * 2));
        const uint32x4_t wlo = vmovl_u16(vget_low_u16(w8));
        const uint32x4_t whi = vmovl_u16(vget_high_u16(w8));
        const uint32_t sum1c = vaddvq_u32(wlo) + vaddvq_u32(whi);
        const uint32x4_t dlo = vmulq_u32(wlo, ramp8l);
        const uint32x4_t dhi = vmulq_u32(whi, ramp8h);
        const uint32_t wsum2 = vaddvq_u32(dlo) + vaddvq_u32(dhi);
        sum2 = ws_a_fold32(sum2 + 8u * sum1 + wsum2);
        sum1 = ws_a_fold32(sum1 + sum1c);
    }

    for (; off + 4 <= words; off += 4) {
        const uint16x4_t w4 = vld1_u16((const uint16_t*)(const void*)(p + off * 2));
        const uint32x4_t w = vmovl_u16(w4);
        const uint32_t sum1c = vaddvq_u32(w);
        const uint32x4_t d = vmulq_u32(w, ramp4);
        const uint32_t wsum2 = vaddvq_u32(d);
        sum2 = ws_a_fold32(sum2 + 4u * sum1 + wsum2);
        sum1 = ws_a_fold32(sum1 + sum1c);
    }

    for (; off < words; off++) {
        uint16_t w;
        memcpy(&w, p + off * 2, sizeof(w));
        sum1 = ws_a_fold32(sum1 + (uint32_t)w);
        sum2 = ws_a_fold32(sum2 + sum1);
    }

    uint32_t r = (sum2 << 16) | sum1;
    r ^= stamp * 0x9E3779B9u;
    r *= 0x85EBCA6Bu;
    r ^= r >> 13;
    r *= 0xC2B2AE35u;
    r ^= r >> 16;
    return r;
}

const weft_simd_kernels_t weft_simd_neon_kernels = {
    .normalize        = ws_normalize_neon,
    .delta_encode     = ws_delta_enc_neon,
    .delta_decode     = ws_delta_dec_neon,
    .dot_f32          = ws_dot_neon,
    .seqlock_checksum = ws_fletcher_neon,
    .impl_name        = "neon",
};

// ---------------------------------------------------------------------------
// SVE2 kernels (only when the TU is compiled with SVE2 enabled)
// ---------------------------------------------------------------------------

#if defined(__ARM_FEATURE_SVE2)
#include <arm_sve.h>

int weft_simd_arm_sve2_runtime(void) {
#if defined(__linux__)
#ifndef HWCAP_SVE2
#define HWCAP_SVE2 0   // ancient kernel headers: honestly unavailable
#endif
    return (getauxval(AT_HWCAP) & HWCAP_SVE2) != 0;
#else
    return 0;   // non-Linux SVE2 hosts: the shard compiles NEON-only
#endif
}

static void ws_normalize_sve2(float* dst, const float* src, size_t elems,
                              float f_min, float f_rs) {
    const svbool_t pg32 = svptrue_b32();
    const svfloat32_t vmin = svdup_f32(f_min);
    const svfloat32_t vrs = svdup_f32(f_rs);
    size_t i = 0;
    while (i < elems) {
        const size_t vl = (size_t)svcntw();
        const size_t n = (elems - i < vl) ? elems - i : vl;
        const svbool_t pg = svwhilelt_b32((uint32_t)i, (uint32_t)elems);
        svfloat32_t v = svld1_f32(pg, src + i);
        v = svsub_f32_x(pg32, v, vmin);   // two roundings, never fused
        v = svmul_f32_x(pg32, v, vrs);
        svst1_f32(pg, dst + i, v);
        i += n;
    }
}

static void ws_dot_sve2(float* c, const float* a, const float* b,
                        uint32_t m, uint32_t k, uint32_t n,
                        uint32_t lda, uint32_t ldb, uint32_t ldc) {
    const svbool_t pgall = svptrue_b32();
    for (uint32_t i = 0; i < m; i++) {
        const float* arow = a + (size_t)i * lda;
        float* crow = c + (size_t)i * ldc;
        uint32_t j = 0;
        while (j < n) {
            const size_t vl = (size_t)svcntw();
            const size_t width = ((size_t)n - j < vl) ? (size_t)n - j : vl;
            const svbool_t pg = svwhilelt_b32(j, n);
            svfloat32_t acc = svdup_f32(0.0f);
            for (uint32_t kk = 0; kk < k; kk++) {
                const svfloat32_t bv = svld1_f32(pg, b + (size_t)kk * ldb + j);
                acc = svmla_f32_x(pgall, acc, bv, svdup_f32(arow[kk]));
            }
            svst1_f32(pg, crow + j, acc);
            j += (uint32_t)width;
        }
    }
}

static uint32_t ws_fletcher_sve2(const void* data, size_t bytes,
                                 uint32_t stamp) {
    const unsigned char* p = (const unsigned char*)data;
    const size_t words = bytes / 2;
    const svbool_t pg32 = svptrue_b32();
    uint32_t sum1 = 0;
    uint32_t sum2 = 0;
    size_t off = 0;
    while (off < words) {
        const size_t vl = (size_t)svcntw();
        const size_t c = (words - off < vl) ? words - off : vl;
        const svbool_t pg = svwhilelt_b32((uint32_t)off, (uint32_t)words);
        const svuint32_t w = svld1uh_u32(pg, (const uint16_t*)(const void*)(p + off * 2));
        // ramp = [c, c-1, ..., 1] over the active lanes
        const svuint32_t idx = svindex_u32(0, 1);
        const svuint32_t ramp = svsub_u32_x(pg, svdup_u32((uint32_t)c), idx);
        const svuint32_t d = svmla_u32_x(pg, svdup_u32(0), w, ramp);   // w*ramp
        const uint64_t sum1c = svaddv_u32(pg, w);
        const uint64_t wsum2 = svaddv_u32(pg, d);
        sum2 = ws_a_fold32(sum2 + (uint32_t)c * sum1 + (uint32_t)wsum2);
        sum1 = ws_a_fold32(sum1 + (uint32_t)sum1c);
        off += c;
    }
    uint32_t r = (sum2 << 16) | sum1;
    r ^= stamp * 0x9E3779B9u;
    r *= 0x85EBCA6Bu;
    r ^= r >> 13;
    r *= 0xC2B2AE35u;
    r ^= r >> 16;
    return r;
}

const weft_simd_kernels_t weft_simd_sve2_kernels = {
    // Delta codecs: the NEON 4-lane block scheme — K2 exactness is
    // algebraic (grouping-free), and svext's lane shift needs a
    // compile-time immediate a runtime VL cannot provide. Documented.
    .normalize        = ws_normalize_sve2,
    .delta_encode     = ws_delta_enc_neon,
    .delta_decode     = ws_delta_dec_neon,
    .dot_f32          = ws_dot_sve2,
    .seqlock_checksum = ws_fletcher_sve2,
    .impl_name        = "sve2",
};
#endif // __ARM_FEATURE_SVE2

#endif // __aarch64__
