// weft_simd_rvv.c — RISC-V Vector 1.0 kernel implementations (Pillar 5, D-52).
//
// HONESTY BOUNDARY: this TU compiles to an EMPTY translation unit unless
// the riscv-port shard builds it with -march=rv64gcv (guarded by
// __riscv_v_intrinsic). The executable-verified surface of Pillar 5 is
// x86_64; the RVV leg is oracle-swept on its native runner. Bit-exactness
// across ISAs holds by the same construction arguments (weft_simd.h).
//
// RVV strategy (VL-agnostic where free, fixed-VL where the algorithm is
// cleaner): the delta codec prefix scan uses vslide1up-based Hillis-Steele
// doubling at runtime VL; the fletcher recurrence fixes AVL=16 per chunk
// (ramps are chunk-local constants); normalize/dot run at full runtime VL.
//
//   normalize: vfsub.vv -> vfmul.vv (two roundings, no fused sub-mul)
//   delta codecs: vslide1up lane shifts, block carry in a scalar
//   dot: lane-per-output blocking; strict ascending-k vfmacc.vv chains
//     (RISC-V V mandates single-rounding fused multiply-accumulate)
//   fletcher: chunked recurrence C=16 via vsetvl(16) + vmacc ramps

#include "weft_simd.h"

#include <string.h>

#if defined(__riscv) && defined(__riscv_v_intrinsic)
#include <riscv_vector.h>

static inline uint32_t ws_r_fold32(uint32_t s) {
    while (s > 0xFFFFu) {
        s = (s & 0xFFFFu) + (s >> 16);
    }
    return s;
}

static void ws_normalize_rvv(float* dst, const float* src, size_t elems,
                             float f_min, float f_rs) {
    const size_t vlmax = __riscv_vlenb() / 4;
    size_t i = 0;
    while (i < elems) {
        const size_t vl = (elems - i < vlmax) ? elems - i : vlmax;
        vfloat32m1_t v = __riscv_vle32_v_f32m1(src + i, vl);
        v = __riscv_vfsub_vf_f32m1(v, f_min, vl);   // two roundings, no fuse
        v = __riscv_vfmul_vf_f32m1(v, f_rs, vl);
        __riscv_vse32_v_f32m1(dst + i, v, vl);
        i += vl;
    }
}

static void ws_delta_enc_rvv(uint32_t* dst, const uint32_t* src,
                             size_t elems, uint32_t seed) {
    if (elems == 0) {
        return;
    }
    if (elems == 1) {
        dst[0] = src[0] - seed;
        return;
    }
    const size_t vlmax = __riscv_vlenb() / 4;
    size_t i = 0;
    // First block's lane-0 "previous" is the SEED: out[0] = src[0] - seed
    // falls out of the generic shift for free.
    uint32_t prev_lane_last = seed;
    while (i + vlmax <= elems) {
        const size_t vl = vlmax;
        vuint32m1_t v = __riscv_vle32_v_u32m1(src + i, vl);
        // [prev, v[0..vl-2]] in one vslide1up (lane 0 = scalar, lane i = v[i-1])
        vuint32m1_t sh = __riscv_vslide1up_vx_u32m1(v, (int)prev_lane_last, vl);
        const vuint32m1_t out = __riscv_vsub_vv_u32m1(v, sh, vl);
        // Capture the block's last ORIGINAL element BEFORE the store (in-place)
        prev_lane_last = src[i + vl - 1];
        __riscv_vse32_v_u32m1(dst + i, out, vl);
        i += vl;
    }
    size_t t = i;
    uint32_t prev_s;
    if (t == 0) {
        prev_s = src[0];   // capture BEFORE the in-place dst[0] write
        dst[0] = prev_s - seed;
        t = 1;
    } else {
        prev_s = prev_lane_last;
    }
    for (; t < elems; t++) {
        const uint32_t cur = src[t];   // capture BEFORE the in-place write
        dst[t] = cur - prev_s;
        prev_s = cur;
    }
}

static void ws_delta_dec_rvv(uint32_t* dst, const uint32_t* src,
                             size_t elems, uint32_t seed) {
    const size_t vlmax = __riscv_vlenb() / 4;
    size_t i = 0;
    uint32_t carry = seed;
    while (i + vlmax <= elems) {
        const size_t vl = vlmax;
        vuint32m1_t v = __riscv_vle32_v_u32m1(src + i, vl);
        // Hillis-Steele inclusive scan: shift down k lanes (zero fill via
        // vslidedown), add; k = 1, 2, 4, ... < vl (runtime VL, so loop the
        // doubling count from the log2 of vl — branchless per iteration).
        for (size_t k = 1; k < vl; k <<= 1) {
            vuint32m1_t sh = __riscv_vslidedown_vx_u32m1(v, (long)k, vl);
            v = __riscv_vadd_vv_u32m1(v, sh, vl);
        }
        // Block total: reduce-add of the scanned block == lane vl-1.
        const uint32_t block_total =
            (uint32_t)__riscv_vredsum_vs_u32m1_u64(v, __riscv_vmv_v_x_u32m1(0, vl), vl);
        const vuint32m1_t out = __riscv_vadd_vx_u32m1(v, (int)carry, vl);
        __riscv_vse32_v_u32m1(dst + i, out, vl);
        carry += block_total;
        i += vl;
    }
    for (; i < elems; i++) {
        carry = carry + src[i];
        dst[i] = carry;
    }
}

static void ws_dot_rvv(float* c, const float* a, const float* b,
                       uint32_t m, uint32_t k, uint32_t n,
                       uint32_t lda, uint32_t ldb, uint32_t ldc) {
    const size_t vlmax = __riscv_vlenb() / 4;
    for (uint32_t i = 0; i < m; i++) {
        const float* arow = a + (size_t)i * lda;
        float* crow = c + (size_t)i * ldc;
        uint32_t j = 0;
        while (j < n) {
            const size_t vl = ((size_t)n - j < vlmax) ? (size_t)n - j : vlmax;
            vfloat32m1_t acc = __riscv_vfmv_v_f_f32m1(0.0f, vl);
            for (uint32_t kk = 0; kk < k; kk++) {
                vfloat32m1_t bv = __riscv_vle32_v_f32m1(b + (size_t)kk * ldb + j, vl);
                acc = __riscv_vfmacc_vf_f32m1(acc, arow[kk], bv, vl);
            }
            __riscv_vse32_v_f32m1(crow + j, acc, vl);
            j += (uint32_t)vl;
        }
    }
}

static uint32_t ws_fletcher_rvv(const void* data, size_t bytes,
                                uint32_t stamp) {
    const unsigned char* p = (const unsigned char*)data;
    const size_t words = bytes / 2;
    uint32_t sum1 = 0;
    uint32_t sum2 = 0;
    size_t off = 0;

    // Fixed C=16 chunks: ramps are compile-time constants [16..1].
    const size_t vl = 16;
    // ramp[0]=16 .. ramp[15]=1 built once (element-wise immediate loads).
    uint32_t ramp_arr[16];
    for (uint32_t t = 0; t < 16; t++) {
        ramp_arr[t] = 16 - t;
    }
    const vuint32m1_t ramp = __riscv_vle32_v_u32m1(ramp_arr, vl);

    for (; off + 16 <= words; off += 16) {
        // Load 16 u16 words widened to u32 (vwideroadu).
        vuint16m1_t w16 = __riscv_vle16_v_u16m1((const uint16_t*)(const void*)(p + off * 2), vl);
        vuint32m1_t w = __riscv_vzext_vf_u32m1(w16, vl);
        const uint32_t sum1c =
            (uint32_t)__riscv_vredsum_vs_u32m1_u64(w, __riscv_vmv_v_x_u32m1(0, vl), vl);
        const vuint32m1_t d = __riscv_vmul_vv_u32m1(w, ramp, vl);
        const uint32_t wsum2 =
            (uint32_t)__riscv_vredsum_vs_u32m1_u64(d, __riscv_vmv_v_x_u32m1(0, vl), vl);
        sum2 = ws_r_fold32(sum2 + 16u * sum1 + wsum2);
        sum1 = ws_r_fold32(sum1 + sum1c);
    }

    for (; off < words; off++) {
        uint16_t w;
        memcpy(&w, p + off * 2, sizeof(w));
        sum1 = ws_r_fold32(sum1 + (uint32_t)w);
        sum2 = ws_r_fold32(sum2 + sum1);
    }

    uint32_t r = (sum2 << 16) | sum1;
    r ^= stamp * 0x9E3779B9u;
    r *= 0x85EBCA6Bu;
    r ^= r >> 13;
    r *= 0xC2B2AE35u;
    r ^= r >> 16;
    return r;
}

const weft_simd_kernels_t weft_simd_rvv_kernels = {
    .normalize        = ws_normalize_rvv,
    .delta_encode     = ws_delta_enc_rvv,
    .delta_decode     = ws_delta_dec_rvv,
    .dot_f32          = ws_dot_rvv,
    .seqlock_checksum = ws_fletcher_rvv,
    .impl_name        = "rvv",
};

#endif // __riscv && __riscv_v_intrinsic
