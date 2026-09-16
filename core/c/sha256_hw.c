// sha256_hw.c — accelerated SHA-256 compression (Series 6).
//
// WHY EXISTS: RFC 0005's benchmark section deferred hardware acceleration
// ("requires hardware AES-NI / ARMv8 crypto instructions for < 1.0 µs" per
// frame). The pure-C scalar path measured 574 K encode / 971 K verify
// frames/s in this tree; the lead's Series-6 target is > 1,000 K/s for both.
// This file supplies the two industry-standard acceleration paths with
// runtime CPU dispatch:
//
//   x86-64  Intel SHA extensions (SHA256RNDS2/MSG1/MSG2) — Zen 1+, Ice Lake+
//   aarch64 ARMv8 Crypto Extensions (FEAT_SHA256)        — every Apple silicon,
//                                                           Graviton 2+, ARMv8.4+
//
// The scalar path in sha256.c remains the normative reference. Both
// accelerated transforms produce bit-identical digests — proven by V8
// (fixture vectors + randomized buffers swept under BOTH regimes) and by the
// bench runner's scalar/auto A/B, not by assertion.
//
// Provenance: the x86 transform follows the public-domain intrinsics layout
// published by Intel and adapted by Sean Gulley (miTLS) and Jeffrey Walton
// (SHA-Intrinsics sample); the message-schedule rotation was independently
// re-derived from FIPS 180-4 §6.2.2 during this port and cross-checked
// against that layout before integration. The aarch64 transform follows the
// documented vsha256hq_u32/vsha256h2q_u32/vsha256su0q_u32/vsha256su1q_u32
// idiom (ARM A-profile intrinsics reference).
//
// Layer discipline: driver-layer module. weft.c/weft.h untouched; no
// allocation; no new library dependencies (intrinsics headers only).
//
// Honesty boundary: the x86 path is executable-verified in this sandbox's
// CI (SHA-NI present in the runner CPU, V-series + ASAN + A/B bench). The
// aarch64 path is compile-guarded for ARM runners (apple-packages /
// linux-arm64 CI) and is NOT executable-tested on the x86_64 sandbox —
// declared, per the repo's per-port honesty culture (WO-P4 decision 4).

#include "sha256.h"

#include <stddef.h>

// ---------------------------------------------------------------------------
// x86-64: Intel SHA extensions
// ---------------------------------------------------------------------------

#if defined(__x86_64__) || defined(_M_X64)

#if defined(__GNUC__)
#include <immintrin.h>
#endif

/// One transform call = prologue/epilogue shuffles once + `blocks` rounds of
/// the 16-group SHA-NI pipeline (state stays in the rnds2 register domain
/// across blocks — the per-block save/restore is two adds, not shuffles).
__attribute__((target("sha,sse4.1")))
static void sha256_transform_x86_ni(uint32_t state[8], const uint8_t* data,
                                    size_t blocks) {
    __m128i STATE0, STATE1, MSG, T0, T1, T2, T3, TMP;
    const __m128i MASK =
        _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);

    /* state -> rnds2 domain: STATE0 = [F,E,B,A], STATE1 = [H,G,D,C] */
    __m128i L0 = _mm_loadu_si128((const __m128i*)&state[0]);
    __m128i L1 = _mm_loadu_si128((const __m128i*)&state[4]);
    TMP = _mm_shuffle_epi32(L0, 0xB1);            /* [B,A,D,C] */
    STATE1 = _mm_shuffle_epi32(L1, 0x1B);         /* [H,G,F,E] */
    STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);     /* [F,E,B,A] */
    STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0);  /* [H,G,D,C] */

    while (blocks-- != 0) {
        const __m128i SAVE0 = STATE0, SAVE1 = STATE1;

        /* message blocks 0..3, big-endian dwords */
        T0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 0)), MASK);
        T1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 16)), MASK);
        T2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 32)), MASK);
        T3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 48)), MASK);

        /* group 0 (rounds 0-3): consume block 0 — no schedule work yet */
        MSG = _mm_add_epi32(T0, _mm_set_epi64x(0xE9B5DBA5B5C0FBCFULL,
                                               0x71374491428A2F98ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

        /* group 1: consume block 1; msg1(T0,T1) preps block 4 */
        MSG = _mm_add_epi32(T1, _mm_set_epi64x(0xAB1C5ED5923F82A4ULL,
                                               0x59F111F13956C25BULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        T0 = _mm_sha256msg1_epu32(T0, T1);

        /* group 2: consume block 2; msg1(T1,T2) preps block 5 */
        MSG = _mm_add_epi32(T2, _mm_set_epi64x(0x550C7DC3243185BEULL,
                                               0x12835B01D807AA98ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        T1 = _mm_sha256msg1_epu32(T1, T2);

        /* group 3: consume block 3; complete T0 (block 4):
           T0 += alignr(block_3, block_2)[W[9..12]]; msg2(T0, block_3);
           then msg1(T2,T3) preps block 6 (AFTER the alignr read T2). */
        MSG = _mm_add_epi32(T3, _mm_set_epi64x(0xC19BF1749BDC06A7ULL,
                                               0x80DEB1FE72BE5D74ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(T3, T2, 4);              /* W[9..12] */
        T0 = _mm_add_epi32(T0, TMP);
        T0 = _mm_sha256msg2_epu32(T0, T3);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        T2 = _mm_sha256msg1_epu32(T2, T3);

        /* groups 4..14: consume block g (T[g%4]); complete block g+1
           (T[(g+1)%4]) with alignr(T[g%4], T[(g-1)%4])[W[4g-7..4g-4]] +
           msg2(..., T[g%4]); msg1(T[(g-1)%4], T[g%4]) preps block g+3
           (through g=12 only). ORDER MATTERS: the completion's alignr reads
           block g-1 from T[(g-1)%4] BEFORE msg1 repurposes that register. */

        /* group 4: consume T0 (block 4); complete T1; msg1(T3,T0) */
        MSG = _mm_add_epi32(T0, _mm_set_epi64x(0x240CA1CC0FC19DC6ULL,
                                               0xEFBE4786E49B69C1ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(T0, T3, 4);              /* W[13..16] */
        T1 = _mm_add_epi32(T1, TMP);
        T1 = _mm_sha256msg2_epu32(T1, T0);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        T3 = _mm_sha256msg1_epu32(T3, T0);             /* prep block 7 */

        /* group 5: consume T1; complete T2; msg1(T0,T1) */
        MSG = _mm_add_epi32(T1, _mm_set_epi64x(0x76F988DA5CB0A9DCULL,
                                               0x4A7484AA2DE92C6FULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(T1, T0, 4);              /* W[17..20] */
        T2 = _mm_add_epi32(T2, TMP);
        T2 = _mm_sha256msg2_epu32(T2, T1);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        T0 = _mm_sha256msg1_epu32(T0, T1);             /* prep block 8 */

        /* group 6: consume T2; complete T3; msg1(T1,T2) */
        MSG = _mm_add_epi32(T2, _mm_set_epi64x(0xBF597FC7B00327C8ULL,
                                               0xA831C66D983E5152ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(T2, T1, 4);              /* W[21..24] */
        T3 = _mm_add_epi32(T3, TMP);
        T3 = _mm_sha256msg2_epu32(T3, T2);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        T1 = _mm_sha256msg1_epu32(T1, T2);             /* prep block 9 */

        /* group 7: consume T3; complete T0; msg1(T2,T3) */
        MSG = _mm_add_epi32(T3, _mm_set_epi64x(0x1429296706CA6351ULL,
                                               0xD5A79147C6E00BF3ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(T3, T2, 4);              /* W[25..28] */
        T0 = _mm_add_epi32(T0, TMP);
        T0 = _mm_sha256msg2_epu32(T0, T3);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        T2 = _mm_sha256msg1_epu32(T2, T3);             /* prep block 10 */

        /* group 8: consume T0; complete T1; msg1(T3,T0) */
        MSG = _mm_add_epi32(T0, _mm_set_epi64x(0x53380D134D2C6DFCULL,
                                               0x2E1B213827B70A85ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(T0, T3, 4);              /* W[29..32] */
        T1 = _mm_add_epi32(T1, TMP);
        T1 = _mm_sha256msg2_epu32(T1, T0);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        T3 = _mm_sha256msg1_epu32(T3, T0);             /* prep block 11 */

        /* group 9: consume T1; complete T2; msg1(T0,T1) */
        MSG = _mm_add_epi32(T1, _mm_set_epi64x(0x92722C8581C2C92EULL,
                                               0x766A0ABB650A7354ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(T1, T0, 4);              /* W[33..36] */
        T2 = _mm_add_epi32(T2, TMP);
        T2 = _mm_sha256msg2_epu32(T2, T1);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        T0 = _mm_sha256msg1_epu32(T0, T1);             /* prep block 12 */

        /* group 10: consume T2; complete T3; msg1(T1,T2) */
        MSG = _mm_add_epi32(T2, _mm_set_epi64x(0xC76C51A3C24B8B70ULL,
                                               0xA81A664BA2BFE8A1ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(T2, T1, 4);              /* W[37..40] */
        T3 = _mm_add_epi32(T3, TMP);
        T3 = _mm_sha256msg2_epu32(T3, T2);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        T1 = _mm_sha256msg1_epu32(T1, T2);             /* prep block 13 */

        /* group 11: consume T3; complete T0; msg1(T2,T3) */
        MSG = _mm_add_epi32(T3, _mm_set_epi64x(0x106AA070F40E3585ULL,
                                               0xD6990624D192E819ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(T3, T2, 4);              /* W[41..44] */
        T0 = _mm_add_epi32(T0, TMP);
        T0 = _mm_sha256msg2_epu32(T0, T3);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        T2 = _mm_sha256msg1_epu32(T2, T3);             /* prep block 14 */

        /* group 12: consume T0; complete T1; msg1(T3,T0) — last msg1 */
        MSG = _mm_add_epi32(T0, _mm_set_epi64x(0x34B0BCB52748774CULL,
                                               0x1E376C0819A4C116ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(T0, T3, 4);              /* W[45..48] */
        T1 = _mm_add_epi32(T1, TMP);
        T1 = _mm_sha256msg2_epu32(T1, T0);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        T3 = _mm_sha256msg1_epu32(T3, T0);             /* prep block 15 */

        /* group 13: consume T1; complete T2; no msg1 (would prep block 16) */
        MSG = _mm_add_epi32(T1, _mm_set_epi64x(0x682E6FF35B9CCA4FULL,
                                               0x4ED8AA4A391C0CB3ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(T1, T0, 4);              /* W[49..52] */
        T2 = _mm_add_epi32(T2, TMP);
        T2 = _mm_sha256msg2_epu32(T2, T1);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

        /* group 14: consume T2; complete T3; no msg1 */
        MSG = _mm_add_epi32(T2, _mm_set_epi64x(0x8CC7020884C87814ULL,
                                               0x78A5636F748F82EEULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(T2, T1, 4);              /* W[53..56] */
        T3 = _mm_add_epi32(T3, TMP);
        T3 = _mm_sha256msg2_epu32(T3, T2);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

        /* group 15 (rounds 60-63): consume block 15; no schedule work */
        MSG = _mm_add_epi32(T3, _mm_set_epi64x(0xC67178F2BEF9A3F7ULL,
                                               0xA4506CEB90BEFFFAULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

        /* feed-forward in the rnds2 domain, then next block */
        STATE0 = _mm_add_epi32(STATE0, SAVE0);
        STATE1 = _mm_add_epi32(STATE1, SAVE1);
        data += 64;
    }

    /* rnds2 domain -> state layout (inverse of the prologue) */
    TMP = _mm_shuffle_epi32(STATE0, 0x1B);             /* [A,B,E,F] */
    STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);          /* [G,H,C,D] */
    STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0);       /* [A,B,C,D] */
    STATE1 = _mm_alignr_epi8(STATE1, TMP, 8);          /* [E,F,G,H] */
    _mm_storeu_si128((__m128i*)&state[0], STATE0);
    _mm_storeu_si128((__m128i*)&state[4], STATE1);
}

weft_sha256_impl_t weft_sha256_hw_probe(void) {
#if defined(__GNUC__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("sha")) {
        return WEFT_SHA256_X86_SHA_NI;
    }
#endif
    return WEFT_SHA256_SCALAR;
}

weft_sha256_transform_fn weft_sha256_hw_resolve(weft_sha256_impl_t impl) {
    if (impl == WEFT_SHA256_X86_SHA_NI) {
        return &sha256_transform_x86_ni;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// aarch64: ARMv8 Crypto Extensions (FEAT_SHA256)
// ---------------------------------------------------------------------------
// Compile-guarded: built only on aarch64 toolchains. Linux resolves at
// runtime via getauxval(AT_HWCAP) & HWCAP_SHA2; Apple silicon has FEAT_SHA256
// architecturally (any arm64 Mac) so it is compile-time enabled there.
// NOT executable-tested in the x86_64 sandbox — declared (see header comment
// for the honesty boundary); the shape is the documented 16-iteration idiom.
// ---------------------------------------------------------------------------

#elif defined(__aarch64__)

#include <arm_neon.h>

#if defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1 << 3)  // stable UAPI bit (kernel arch/arm64/include/uapi/asm/hwcap.h)
#endif
#endif

/* The CE transform compiles only when the TU targets +crypto (the Makefile
   adds -march=armv8-a+crypto on aarch64; mbedTLS-style). Execution stays
   gated on the runtime probe below, so a +crypto binary still runs safely
   on non-CE hardware — the guarded instructions are unreachable there. */
#if defined(__ARM_FEATURE_CRYPTO)

static const uint32_t WEFT_SHA_K_ARM[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_transform_arm_ce(uint32_t state[8], const uint8_t* data,
                                    size_t blocks) {
    uint32x4_t STATE0 = vld1q_u32(&state[0]);  /* A B C D */
    uint32x4_t STATE1 = vld1q_u32(&state[4]);  /* E F G H */

    while (blocks-- != 0) {
        const uint32x4_t SAVE0 = STATE0, SAVE1 = STATE1;

        /* W[0..15] big-endian; the four schedule vectors rotate every block */
        uint32x4_t W0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 0)));
        uint32x4_t W1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16)));
        uint32x4_t W2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 32)));
        uint32x4_t W3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 48)));

        for (int i = 0; i < 16; i++) {
            uint32x4_t WK;
            if (i < 4) {
                uint32x4_t w = (i == 0) ? W0 : (i == 1) ? W1 : (i == 2) ? W2 : W3;
                WK = vaddq_u32(w, vld1q_u32(&WEFT_SHA_K_ARM[4 * i]));
            } else {
                /* W[i%4] = W[i%4-16] + sigma0(W[..-15]) + W[..-7] + sigma1(W[..-2]):
                   su0 takes (block being extended, next block);
                   su1 completes it with (block-8..-5, block-4..-1). */
                uint32x4_t* wcur = (i % 4 == 0) ? &W0 : (i % 4 == 1) ? &W1
                                    : (i % 4 == 2) ? &W2 : &W3;
                uint32x4_t* wnext = (i % 4 == 3) ? &W0 : wcur + 1;
                uint32x4_t* wback2 = (i % 4 == 0) ? &W2 : (i % 4 == 1) ? &W3
                                      : (i % 4 == 2) ? &W0 : &W1;
                uint32x4_t* wback1 = (i % 4 == 0) ? &W3 : (i % 4 == 1) ? &W0
                                      : (i % 4 == 2) ? &W1 : &W2;
                *wcur = vsha256su0q_u32(*wcur, *wnext);
                *wcur = vsha256su1q_u32(*wcur, *wback2, *wback1);
                WK = vaddq_u32(*wcur, vld1q_u32(&WEFT_SHA_K_ARM[4 * i]));
            }
            const uint32x4_t TMP2 = STATE0;
            STATE0 = vsha256hq_u32(STATE0, STATE1, WK);
            STATE1 = vsha256h2q_u32(STATE1, TMP2, WK);
        }

        STATE0 = vaddq_u32(STATE0, SAVE0);
        STATE1 = vaddq_u32(STATE1, SAVE1);
        data += 64;
    }

    vst1q_u32(&state[0], STATE0);
    vst1q_u32(&state[4], STATE1);
}

#endif  // __ARM_FEATURE_CRYPTO

weft_sha256_impl_t weft_sha256_hw_probe(void) {
#if defined(__ARM_FEATURE_CRYPTO)
    #if defined(__linux__)
        return (getauxval(AT_HWCAP) & HWCAP_SHA2) ? WEFT_SHA256_ARM_CE
                                                  : WEFT_SHA256_SCALAR;
    #else
        return WEFT_SHA256_ARM_CE;  // arm64 Macs: FEAT_SHA256 architectural
    #endif
#else
    return WEFT_SHA256_SCALAR;  // CE transform not compiled (TU lacks +crypto)
#endif
}

weft_sha256_transform_fn weft_sha256_hw_resolve(weft_sha256_impl_t impl) {
#if defined(__ARM_FEATURE_CRYPTO)
    if (impl == WEFT_SHA256_ARM_CE) {
        return &sha256_transform_arm_ce;
    }
#else
    (void)impl;
#endif
    return NULL;
}

// ---------------------------------------------------------------------------
// other architectures: scalar only
// ---------------------------------------------------------------------------

#else

weft_sha256_impl_t weft_sha256_hw_probe(void) {
    return WEFT_SHA256_SCALAR;
}

weft_sha256_transform_fn weft_sha256_hw_resolve(weft_sha256_impl_t impl) {
    (void)impl;
    return NULL;
}

#endif
