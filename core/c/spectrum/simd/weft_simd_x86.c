// weft_simd_x86.c — AVX2 + AVX-512F/BW kernel implementations (Pillar 5, D-52).
//
// House pattern: per-function target attributes inside one baseline TU —
// the binary carries every engine fat, execution is gated by the runtime
// probe in weft_simd.c, and no caller ever needs -march flags.
//
// BIT-EXACTNESS notes per kernel (contracts in weft_simd.h):
//   normalize: vsubps -> vmulps, TWO roundings, never fused (a subtract
//     feeding a multiply has no fused ISA form) — identical bits at any
//     width, and identical to the scalar two-op sequence.
//   delta codecs: u32 modular arithmetic; the vector prefix scan is block
//     sums + block offsets (exact integer algebra), the encoder carries
//     the previous vector IN REGISTER so in-place buffers stay correct.
//   dot: lane-per-output blocking over j; each output's accumulation is a
//     strict ascending-k chain of single vfmadd ops == scalar fmaf chain.
//   fletcher: chunked recurrence with ramps [C..1] per chunk (C=32/16 on
//     AVX-512, 16/8 on AVX2) — chunk-invariance proven in the header.
//
// IN-PLACE discipline: every kernel may be called with dst == src. Vector
// bodies load the full input block BEFORE storing the output block, and
// cross-block dependencies travel through registers, never through
// re-reads of already-overwritten memory.

#include "weft_simd.h"

#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Shared scalar tail helpers (identical math to the oracle — bit-exact by
// construction, so tails may run element-wise without an oracle divergence)
// ---------------------------------------------------------------------------

static inline uint32_t ws_x_fold32(uint32_t s) {
    while (s > 0xFFFFu) {
        s = (s & 0xFFFFu) + (s >> 16);
    }
    return s;
}

// ===========================================================================
// AVX-512 engines (target: avx512f + avx512bw)
// ===========================================================================

__attribute__((target("avx512f,avx512bw")))
static void ws_normalize_avx512(float* dst, const float* src, size_t elems,
                                float f_min, float f_rs) {
    const __m512 vmin = _mm512_set1_ps(f_min);
    const __m512 vrs  = _mm512_set1_ps(f_rs);
    size_t i = 0;
    for (; i + 16 <= elems; i += 16) {
        __m512 v = _mm512_loadu_ps(src + i);
        v = _mm512_sub_ps(v, vmin);   // two roundings, never contracted
        v = _mm512_mul_ps(v, vrs);
        _mm512_storeu_ps(dst + i, v);
    }
    for (; i < elems; i++) {
        dst[i] = (src[i] - f_min) * f_rs;
    }
}

// Encoder: out = v - [prev[15], v[0..14]] via one valignd; prev travels in
// a register, so dst == src is safe (input block is read before written).
__attribute__((target("avx512f,avx512bw")))
static void ws_delta_enc_avx512(uint32_t* dst, const uint32_t* src,
                                size_t elems, uint32_t seed) {
    if (elems == 0) {
        return;
    }
    if (elems == 1) {
        dst[0] = src[0] - seed;
        return;
    }
    __m512i prev = _mm512_set1_epi32((int)src[0]);   // lane 15 feeds out[0]
    size_t i = 0;
    for (; i + 16 <= elems; i += 16) {
        const __m512i v = _mm512_loadu_si512((const void*)(src + i));
        const __m512i sh = _mm512_alignr_epi32(v, prev, 15);
        __m512i out = _mm512_sub_epi32(v, sh);
        if (i == 0) {
            out = _mm512_mask_blend_epi32(
                1u, out, _mm512_set1_epi32((int)(src[0] - seed)));
        }
        _mm512_storeu_si512((void*)(dst + i), out);
        prev = v;
    }
    size_t t = i;
    uint32_t prev_s;
    if (t == 0) {
        prev_s = src[0];   // capture BEFORE the in-place dst[0] write
        dst[0] = prev_s - seed;
        t = 1;
    } else {
        // Lane 15 of the previous vector (set1(15) permutexvar selects it
        // into lane 0; shuffle_i32x4 alone can only move whole 128-bit blocks).
        prev_s = (uint32_t)_mm512_cvtsi512_si32(
            _mm512_permutexvar_epi32(_mm512_set1_epi32(15), prev));
    }
    for (; t < elems; t++) {
        const uint32_t cur = src[t];   // capture BEFORE the in-place write
        dst[t] = cur - prev_s;
        prev_s = cur;
    }
}

// Decoder: inclusive in-block prefix scan (Hillis-Steele doubling over
// lanes) + block carry. Exact modular integer algebra: identical to the
// scalar running sum for ANY lane grouping.
__attribute__((target("avx512f,avx512bw")))
static void ws_delta_dec_avx512(uint32_t* dst, const uint32_t* src,
                                size_t elems, uint32_t seed) {
    uint32_t carry = seed;
    size_t i = 0;
    for (; i + 16 <= elems; i += 16) {
        __m512i v = _mm512_loadu_si512((const void*)(src + i));
        v = _mm512_add_epi32(
            v, _mm512_maskz_permutexvar_epi32(0xFFFEu,
                _mm512_setr_epi32(15, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                                  12, 13, 14), v));
        v = _mm512_add_epi32(
            v, _mm512_maskz_permutexvar_epi32(0xFFFCu,
                _mm512_setr_epi32(14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                  11, 12, 13), v));
        v = _mm512_add_epi32(
            v, _mm512_maskz_permutexvar_epi32(0xFFF0u,
                _mm512_setr_epi32(12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8,
                                  9, 10, 11), v));
        v = _mm512_add_epi32(
            v, _mm512_maskz_permutexvar_epi32(0xFF00u,
                _mm512_setr_epi32(8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3,
                                  4, 5, 6, 7), v));
        // Block total BEFORE the carry add: scanned lane 15.
        const uint32_t block_total = (uint32_t)_mm512_cvtsi512_si32(
            _mm512_permutexvar_epi32(_mm512_set1_epi32(15), v));
        const __m512i out = _mm512_add_epi32(v, _mm512_set1_epi32((int)carry));
        _mm512_storeu_si512((void*)(dst + i), out);
        carry += block_total;   // modular
    }
    for (; i < elems; i++) {
        carry = carry + src[i];
        dst[i] = carry;
    }
}

// Lane-per-output j-blocking; per output: strict ascending-k vfmadd chain.
__attribute__((target("avx512f,avx512bw")))
static void ws_dot_avx512(float* c, const float* a, const float* b,
                          uint32_t m, uint32_t k, uint32_t n,
                          uint32_t lda, uint32_t ldb, uint32_t ldc) {
    for (uint32_t i = 0; i < m; i++) {
        const float* arow = a + (size_t)i * lda;
        float* crow = c + (size_t)i * ldc;
        uint32_t j = 0;
        for (; j + 16 <= n; j += 16) {
            __m512 acc = _mm512_setzero_ps();
            for (uint32_t kk = 0; kk < k; kk++) {
                acc = _mm512_fmadd_ps(
                    _mm512_loadu_ps(b + (size_t)kk * ldb + j),
                    _mm512_set1_ps(arow[kk]), acc);
            }
            _mm512_storeu_ps(crow + j, acc);
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

// Chunked Fletcher, C=32 (64B of input per recurrence step) then C=16.
// Ramps are chunk-LOCAL: [32..17] for the low 16 words, [16..1] for the
// high 16 — the cross term (C * sum1_run) carries all global positioning.
__attribute__((target("avx512f,avx512bw")))
static uint32_t ws_fletcher_avx512(const void* data, size_t bytes,
                                   uint32_t stamp) {
    const unsigned char* p = (const unsigned char*)data;
    const size_t words = bytes / 2;
    uint32_t sum1 = 0;
    uint32_t sum2 = 0;
    size_t off = 0;

    const __m512i ramp32_lo = _mm512_setr_epi32(32, 31, 30, 29, 28, 27, 26, 25,
                                                24, 23, 22, 21, 20, 19, 18, 17);
    const __m512i ramp32_hi = _mm512_setr_epi32(16, 15, 14, 13, 12, 11, 10, 9,
                                                8, 7, 6, 5, 4, 3, 2, 1);
    for (; off + 32 <= words; off += 32) {
        const __m512i raw =
            _mm512_loadu_si512((const void*)(p + off * 2));
        const __m512i wlo =
            _mm512_cvtepu16_epi32(_mm512_castsi512_si256(raw));
        const __m512i whi = _mm512_cvtepu16_epi32(
            _mm512_extracti64x4_epi64(raw, 1));
        const uint32_t sum1c = (uint32_t)_mm512_reduce_add_epi32(wlo) +
                               (uint32_t)_mm512_reduce_add_epi32(whi);
        const __m512i dlo = _mm512_mullo_epi32(wlo, ramp32_lo);
        const __m512i dhi = _mm512_mullo_epi32(whi, ramp32_hi);
        const uint32_t wsum2 = (uint32_t)_mm512_reduce_add_epi32(dlo) +
                               (uint32_t)_mm512_reduce_add_epi32(dhi);
        sum2 = ws_x_fold32(sum2 + 32u * sum1 + wsum2);
        sum1 = ws_x_fold32(sum1 + sum1c);
    }

    const __m512i ramp16 = _mm512_setr_epi32(16, 15, 14, 13, 12, 11, 10, 9,
                                             8, 7, 6, 5, 4, 3, 2, 1);
    for (; off + 16 <= words; off += 16) {
        const __m512i w =
            _mm512_cvtepu16_epi32(_mm256_loadu_si256(
                (const __m256i*)(const void*)(p + off * 2)));
        const uint32_t sum1c = (uint32_t)_mm512_reduce_add_epi32(w);
        const uint32_t wsum2 =
            (uint32_t)_mm512_reduce_add_epi32(_mm512_mullo_epi32(w, ramp16));
        sum2 = ws_x_fold32(sum2 + 16u * sum1 + wsum2);
        sum1 = ws_x_fold32(sum1 + sum1c);
    }

    for (; off < words; off++) {
        uint16_t w;
        memcpy(&w, p + off * 2, sizeof(w));
        sum1 = ws_x_fold32(sum1 + (uint32_t)w);
        sum2 = ws_x_fold32(sum2 + sum1);
    }

    uint32_t r = (sum2 << 16) | sum1;
    r ^= stamp * 0x9E3779B9u;
    r *= 0x85EBCA6Bu;
    r ^= r >> 13;
    r *= 0xC2B2AE35u;
    r ^= r >> 16;
    return r;
}

const weft_simd_kernels_t weft_simd_avx512_kernels = {
    .normalize        = ws_normalize_avx512,
    .delta_encode     = ws_delta_enc_avx512,
    .delta_decode     = ws_delta_dec_avx512,
    .dot_f32          = ws_dot_avx512,
    .seqlock_checksum = ws_fletcher_avx512,
    .impl_name        = "avx512",
};

// ===========================================================================
// AVX2 engines (target: avx2 + fma)
// ===========================================================================

__attribute__((target("avx2,fma")))
static void ws_normalize_avx2(float* dst, const float* src, size_t elems,
                              float f_min, float f_rs) {
    const __m256 vmin = _mm256_set1_ps(f_min);
    const __m256 vrs  = _mm256_set1_ps(f_rs);
    size_t i = 0;
    for (; i + 8 <= elems; i += 8) {
        __m256 v = _mm256_loadu_ps(src + i);
        v = _mm256_sub_ps(v, vmin);
        v = _mm256_mul_ps(v, vrs);
        _mm256_storeu_ps(dst + i, v);
    }
    for (; i < elems; i++) {
        dst[i] = (src[i] - f_min) * f_rs;
    }
}

// vpermd cannot reach across the 128-bit lanes the way valignd does, so the
// one-lane shift is a permute (idx = j-1 mod 8) with lane 0 blended from
// the REGISTER-carried previous vector's lane 7 — in-place stays correct.
__attribute__((target("avx2,fma")))
static void ws_delta_enc_avx2(uint32_t* dst, const uint32_t* src,
                              size_t elems, uint32_t seed) {
    if (elems == 0) {
        return;
    }
    if (elems == 1) {
        dst[0] = src[0] - seed;
        return;
    }
    const __m256i idx_sub1 =
        _mm256_setr_epi32(7, 0, 1, 2, 3, 4, 5, 6);
    __m256i prev = _mm256_set1_epi32((int)src[0]);
    size_t i = 0;
    for (; i + 8 <= elems; i += 8) {
        const __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)(src + i));
        __m256i sh = _mm256_permutevar8x32_epi32(v, idx_sub1);
        sh = _mm256_blend_epi32(
            sh, _mm256_set1_epi32((int)(uint32_t)_mm256_extract_epi32(prev, 7)),
            0x01);
        __m256i out = _mm256_sub_epi32(v, sh);
        if (i == 0) {
            out = _mm256_blend_epi32(
                out, _mm256_set1_epi32((int)(src[0] - seed)), 0x01);
        }
        _mm256_storeu_si256((__m256i*)(void*)(dst + i), out);
        prev = v;
    }
    size_t t = i;
    uint32_t prev_s;
    if (t == 0) {
        prev_s = src[0];   // capture BEFORE the in-place dst[0] write
        dst[0] = prev_s - seed;
        t = 1;
    } else {
        prev_s = (uint32_t)_mm256_extract_epi32(prev, 7);
    }
    for (; t < elems; t++) {
        const uint32_t cur = src[t];   // capture BEFORE the in-place write
        dst[t] = cur - prev_s;
        prev_s = cur;
    }
}

__attribute__((target("avx2,fma")))
static void ws_delta_dec_avx2(uint32_t* dst, const uint32_t* src,
                              size_t elems, uint32_t seed) {
    uint32_t carry = seed;
    size_t i = 0;
    for (; i + 8 <= elems; i += 8) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)(src + i));
        const __m256i zero = _mm256_setzero_si256();
        // Hillis-Steele doubling: shift down k lanes, zero the k low lanes,
        // add. blend_imm keeps this branch-free and constant-folded.
        __m256i t1 = _mm256_permutevar8x32_epi32(
            v, _mm256_setr_epi32(7, 0, 1, 2, 3, 4, 5, 6));
        t1 = _mm256_blend_epi32(t1, zero, 0x01);
        v = _mm256_add_epi32(v, t1);
        __m256i t2 = _mm256_permutevar8x32_epi32(
            v, _mm256_setr_epi32(6, 7, 0, 1, 2, 3, 4, 5));
        t2 = _mm256_blend_epi32(t2, zero, 0x03);
        v = _mm256_add_epi32(v, t2);
        __m256i t4 = _mm256_permutevar8x32_epi32(
            v, _mm256_setr_epi32(4, 5, 6, 7, 0, 1, 2, 3));
        t4 = _mm256_blend_epi32(t4, zero, 0x0F);
        v = _mm256_add_epi32(v, t4);
        const uint32_t block_total = (uint32_t)_mm256_extract_epi32(v, 7);
        const __m256i out = _mm256_add_epi32(v, _mm256_set1_epi32((int)carry));
        _mm256_storeu_si256((__m256i*)(void*)(dst + i), out);
        carry += block_total;
    }
    for (; i < elems; i++) {
        carry = carry + src[i];
        dst[i] = carry;
    }
}

__attribute__((target("avx2,fma")))
static void ws_dot_avx2(float* c, const float* a, const float* b,
                        uint32_t m, uint32_t k, uint32_t n,
                        uint32_t lda, uint32_t ldb, uint32_t ldc) {
    for (uint32_t i = 0; i < m; i++) {
        const float* arow = a + (size_t)i * lda;
        float* crow = c + (size_t)i * ldc;
        uint32_t j = 0;
        for (; j + 8 <= n; j += 8) {
            __m256 acc = _mm256_setzero_ps();
            for (uint32_t kk = 0; kk < k; kk++) {
                acc = _mm256_fmadd_ps(
                    _mm256_loadu_ps(b + (size_t)kk * ldb + j),
                    _mm256_set1_ps(arow[kk]), acc);
            }
            _mm256_storeu_ps(crow + j, acc);
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

// Chunked Fletcher, C=16 (32B per step) then C=8 (16B).
__attribute__((target("avx2,fma")))
static uint32_t ws_fletcher_avx2(const void* data, size_t bytes,
                                 uint32_t stamp) {
    const unsigned char* p = (const unsigned char*)data;
    const size_t words = bytes / 2;
    uint32_t sum1 = 0;
    uint32_t sum2 = 0;
    size_t off = 0;

    const __m256i ramp16_lo = _mm256_setr_epi32(16, 15, 14, 13, 12, 11, 10, 9);
    const __m256i ramp16_hi = _mm256_setr_epi32(8, 7, 6, 5, 4, 3, 2, 1);
    for (; off + 16 <= words; off += 16) {
        const __m256i raw =
            _mm256_loadu_si256((const __m256i*)(const void*)(p + off * 2));
        const __m256i wlo =
            _mm256_cvtepu16_epi32(_mm256_castsi256_si128(raw));
        const __m256i whi =
            _mm256_cvtepu16_epi32(_mm256_extracti128_si256(raw, 1));
        const __m256i dlo = _mm256_mullo_epi32(wlo, ramp16_lo);
        const __m256i dhi = _mm256_mullo_epi32(whi, ramp16_hi);
        // sum1c: horizontal add of the widened words (wlo + whi)
        const __m256i wsum_vec = _mm256_add_epi32(wlo, whi);
        const __m128i s4 = _mm_add_epi32(
            _mm256_castsi256_si128(wsum_vec), _mm256_extracti128_si256(wsum_vec, 1));
        const __m128i s2 =
            _mm_add_epi32(s4, _mm_shuffle_epi32(s4, 0xB1));   // pair swap
        const uint32_t sum1c =
            (uint32_t)_mm_cvtsi128_si32(s2) + (uint32_t)_mm_extract_epi32(s2, 2);
        // wsum2: horizontal add of the ramp dot products (dlo + dhi)
        const __m256i dsum_vec = _mm256_add_epi32(dlo, dhi);
        const __m128i q4 = _mm_add_epi32(
            _mm256_castsi256_si128(dsum_vec), _mm256_extracti128_si256(dsum_vec, 1));
        const __m128i q2 =
            _mm_add_epi32(q4, _mm_shuffle_epi32(q4, 0xB1));
        const uint32_t wsum2 =
            (uint32_t)_mm_cvtsi128_si32(q2) + (uint32_t)_mm_extract_epi32(q2, 2);
        sum2 = ws_x_fold32(sum2 + 16u * sum1 + wsum2);
        sum1 = ws_x_fold32(sum1 + sum1c);
    }

    const __m256i ramp8 = _mm256_setr_epi32(8, 7, 6, 5, 4, 3, 2, 1);
    for (; off + 8 <= words; off += 8) {
        const __m256i w = _mm256_cvtepu16_epi32(
            _mm_loadu_si128((const __m128i*)(const void*)(p + off * 2)));
        const __m256i d = _mm256_mullo_epi32(w, ramp8);
        const __m128i s4 = _mm_add_epi32(
            _mm256_castsi256_si128(d), _mm256_extracti128_si256(d, 1));
        const __m128i s2 =
            _mm_add_epi32(s4, _mm_shuffle_epi32(s4, 0xB1));
        const uint32_t wsum2 =
            (uint32_t)_mm_cvtsi128_si32(s2) + (uint32_t)_mm_extract_epi32(s2, 2);
        // sum1c: horizontal add of w
        const __m128i u4 = _mm_add_epi32(
            _mm256_castsi256_si128(w), _mm256_extracti128_si256(w, 1));
        const __m128i u2 =
            _mm_add_epi32(u4, _mm_shuffle_epi32(u4, 0xB1));
        const uint32_t sum1c =
            (uint32_t)_mm_cvtsi128_si32(u2) + (uint32_t)_mm_extract_epi32(u2, 2);
        sum2 = ws_x_fold32(sum2 + 8u * sum1 + wsum2);
        sum1 = ws_x_fold32(sum1 + sum1c);
    }

    for (; off < words; off++) {
        uint16_t w;
        memcpy(&w, p + off * 2, sizeof(w));
        sum1 = ws_x_fold32(sum1 + (uint32_t)w);
        sum2 = ws_x_fold32(sum2 + sum1);
    }

    uint32_t r = (sum2 << 16) | sum1;
    r ^= stamp * 0x9E3779B9u;
    r *= 0x85EBCA6Bu;
    r ^= r >> 13;
    r *= 0xC2B2AE35u;
    r ^= r >> 16;
    return r;
}

const weft_simd_kernels_t weft_simd_avx2_kernels = {
    .normalize        = ws_normalize_avx2,
    .delta_encode     = ws_delta_enc_avx2,
    .delta_decode     = ws_delta_dec_avx2,
    .dot_f32          = ws_dot_avx2,
    .seqlock_checksum = ws_fletcher_avx2,
    .impl_name        = "avx2",
};

#endif // __x86_64__
