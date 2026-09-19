// weft_f16_codec.c — Float16 Quantization Codec & Dirty-Region Mask, C driver layer
//
// LAW 3: Mechanism, not policy. Zero kernel modification.

#include "weft_f16_codec.h"
#include <math.h>
#include <string.h>

typedef union {
    float f;
    uint32_t u;
} weft_f32_bits_t;

typedef union {
    uint32_t u;
    float f;
} weft_u32_bits_t;

uint16_t weft_f32_to_f16(float val) {
    weft_f32_bits_t bits;
    bits.f = val;
    uint32_t u32 = bits.u;

    uint16_t sign = (uint16_t)((u32 >> 16) & 0x8000);
    int exp = (int)((u32 >> 23) & 0xFF) - 127;
    uint32_t mant = u32 & 0x007FFFFF;

    // Special case: NaN or Infinity
    if (exp == 128) {
        if (mant == 0) {
            return sign | 0x7C00u; // Infinity
        } else {
            return sign | 0x7C00u | (uint16_t)(mant >> 13) | 1u; // NaN
        }
    }

    // Overflow to Infinity
    if (exp > 15) {
        return sign | 0x7C00u;
    }

    // Subnormal or Underflow to Zero
    if (exp < -14) {
        if (exp < -24) {
            return sign; // Underflow to zero
        }
        mant |= 0x00800000u; // Add implicit leading 1 bit
        int shift = -14 - exp;
        uint16_t sub_mant = (uint16_t)(mant >> (shift + 13));
        return sign | sub_mant;
    }

    // Normal half-precision number
    uint16_t h_exp = (uint16_t)((exp + 15) << 10);
    uint16_t h_mant = (uint16_t)(mant >> 13);
    return sign | h_exp | h_mant;
}

float weft_f16_to_f32(uint16_t val) {
    uint32_t sign = (uint32_t)(val & 0x8000u) << 16;
    uint32_t exp = (val >> 10) & 0x1Fu;
    uint32_t mant = val & 0x03FFu;

    weft_u32_bits_t bits;

    // Special case: NaN or Infinity
    if (exp == 0x1Fu) {
        if (mant == 0) {
            bits.u = sign | 0x7F800000u;
        } else {
            bits.u = sign | 0x7F800000u | (mant << 13);
        }
        return bits.f;
    }

    // Subnormal or Zero
    if (exp == 0) {
        if (mant == 0) {
            bits.u = sign;
            return bits.f;
        }
        // Normalize subnormal
        int e = -14;
        while ((mant & 0x0400u) == 0) {
            mant <<= 1;
            e--;
        }
        mant &= 0x03FFu;
        uint32_t f_exp = (uint32_t)(e + 127) << 23;
        uint32_t f_mant = mant << 13;
        bits.u = sign | f_exp | f_mant;
        return bits.f;
    }

    // Normal float
    uint32_t f_exp = (uint32_t)(exp + (127 - 15)) << 23;
    uint32_t f_mant = mant << 13;
    bits.u = sign | f_exp | f_mant;
    return bits.f;
}

void weft_ring_publish_f16(weft_fanout_t* f, const float* src, int count) {
    if (!f || !src || count <= 0) return;

    uint16_t* dst = (uint16_t*)weft_fanout_begin(f);
    if (!dst) return;
    for (int i = 0; i < count; i++) {
        dst[i] = weft_f32_to_f16(src[i]);
    }
    weft_fanout_publish(f);
}

int weft_ring_claim_f16(weft_fanout_reader_t* r, float* dst, int max_count, uint64_t* out_seq) {
    if (!r || !dst || max_count <= 0) return 0;

    const weft_fanout_claim_t* claim = weft_fanout_claim(r);
    if (!claim || !claim->fresh) return 0;

    const uint16_t* src = (const uint16_t*)weft_fanout_view(r);
    if (!src) return 0;

    int count = (int)(r->payload_bytes / sizeof(uint16_t));
    if (count > max_count) count = max_count;

    for (int i = 0; i < count; i++) {
        dst[i] = weft_f16_to_f32(src[i]);
    }

    if (out_seq) {
        *out_seq = claim->seq;
    }
    return 1;
}

uint64_t weft_dirty_mask_compute(const float* prev, const float* curr, int rows, int cols_per_row, float epsilon) {
    if (!curr || rows <= 0 || cols_per_row <= 0) return 0ULL;
    if (rows > 64) rows = 64; // Capped to 64 rows for 64-bit mask

    uint64_t mask = 0ULL;
    for (int r = 0; r < rows; r++) {
        int offset = r * cols_per_row;
        int row_dirty = 0;
        if (!prev) {
            row_dirty = 1;
        } else {
            for (int c = 0; c < cols_per_row; c++) {
                if (fabsf(curr[offset + c] - prev[offset + c]) > epsilon) {
                    row_dirty = 1;
                    break;
                }
            }
        }
        if (row_dirty) {
            mask |= (1ULL << r);
        }
    }
    return mask;
}

int weft_dirty_copy_f32(float* dst, const float* src, uint64_t dirty_mask, int rows, int cols_per_row) {
    if (!dst || !src || rows <= 0 || cols_per_row <= 0) return 0;
    if (rows > 64) rows = 64;

    int copied = 0;
    for (int r = 0; r < rows; r++) {
        if (dirty_mask & (1ULL << r)) {
            memcpy(&dst[r * cols_per_row], &src[r * cols_per_row], (size_t)cols_per_row * sizeof(float));
            copied++;
        }
    }
    return copied;
}

int weft_adaptive_should_skip_publish(uint32_t reader_frames_behind, uint32_t threshold) {
    return (reader_frames_behind > threshold) ? 1 : 0;
}
