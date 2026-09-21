// test_simd_oracle.c — the all-paths-equal bit-exactness gate (Pillar 5, D-52).
//
// WHY EXISTS: Law 5 (bit-exactness) is proven HERE, empirically, for every
// SIMD implementation compiled into this binary: each kernel is run at
// every force-pin against the normative scalar oracle across size classes
// (vector boundaries ±1), element-aligned-but-vector-unaligned buffers,
// in-place (dst == src) variants, seed/stamp sweeps and stride grids, and
// the outputs are compared byte-for-byte. Hand-computed golden fixtures
// (independent of both implementations) anchor the oracle itself.
//
// A single mismatch is a hard failure: bit-exactness is a construction
// claim, and this battery is the witness that the construction survived
// contact with the compiler.

#include "../../../core/c/spectrum/simd/weft_simd.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

// Deterministic PRNG (xorshift32) — no libc rand, reproducible evrywhere.
static uint32_t ws_rng(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

#define POOL_BYTES (2u << 20)   // 2 MiB, 64-aligned

static unsigned char g_pool[POOL_BYTES] __attribute__((aligned(64)));
static unsigned char g_ref[POOL_BYTES] __attribute__((aligned(64)));
static unsigned char g_out[POOL_BYTES] __attribute__((aligned(64)));

static const size_t SIZES[] = {
    0, 1, 2, 3, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
    127, 128, 129, 255, 256, 257, 1000, 4095, 4096, 4097, 16384, 65536,
};
#define NSIZES ((int)(sizeof(SIZES) / sizeof(SIZES[0])))

static const uint32_t SEEDS[] = { 0u, 1u, 7u, 0x9E3779B9u, 0xFFFFFFFFu };
#define NSEEDS ((int)(sizeof(SEEDS) / sizeof(SEEDS[0])))

static const char* IMPL_NAMES[WEFT_SIMD_IMPL_COUNT] = {
    "scalar", "avx2", "avx512", "neon", "sve2", "rvv",
};

// ---------------------------------------------------------------------------
// Per-kernel sweeps: every compiled impl vs the scalar oracle
// ---------------------------------------------------------------------------

static void sweep_normalize(void) {
    const float PAIRS[][2] = {
        { 0.0f, 1.0f / 256.0f }, { 0.0f, 1.0f / 3.0f },
        { -2.5f, 0.1f }, { 100.0f, 1e-7f }, { 0.0f, 0.0f },
    };
    for (int impl = 0; impl < WEFT_SIMD_IMPL_COUNT; impl++) {
        const weft_simd_kernels_t* kt = weft_simd_impl_kernels((weft_simd_impl_t)impl);
        if (kt == NULL) {
            continue;
        }
        CHECK(weft_simd_force_impl(IMPL_NAMES[impl]) == 0, "pin %s", IMPL_NAMES[impl]);
        for (int si = 0; si < NSIZES; si++) {
            const size_t elems = SIZES[si];
            if (elems > POOL_BYTES / 4) {
                continue;
            }
            for (int pi = 0; pi < (int)(sizeof(PAIRS) / sizeof(PAIRS[0])); pi++) {
                const float f_min = PAIRS[pi][0];
                const float f_rs = PAIRS[pi][1];
                // element-aligned but vector-unaligned offsets (4B steps)
                for (size_t off = 0; off <= 12; off += 4) {
                    float* src = (float*)(g_pool + off);
                    uint32_t rs = 0xA5A5A5A5u + (uint32_t)si * 2654435761u + (uint32_t)pi;
                    for (size_t i = 0; i < elems; i++) {
                        uint32_t bits = ws_rng(&rs);
                        memcpy(src + i, &bits, 4);
                    }
                    float* ref = (float*)(g_ref + off);
                    float* out = (float*)(g_out + off);
                    weft_simd_scalar_normalize(ref, src, elems, f_min, f_rs);
                    weft_simd_normalize(out, src, elems, f_min, f_rs);
                    CHECK(memcmp(out, ref, elems * 4) == 0,
                          "normalize %s elems=%zu off=%zu pair=%d",
                          IMPL_NAMES[impl], elems, off, pi);
                    // in-place
                    memcpy(out, src, elems * 4);
                    weft_simd_normalize(out, out, elems, f_min, f_rs);
                    CHECK(memcmp(out, ref, elems * 4) == 0,
                          "normalize-inplace %s elems=%zu pair=%d",
                          IMPL_NAMES[impl], elems, pi);
                }
            }
        }
    }
    weft_simd_force_auto();
}

static void sweep_delta(void) {
    for (int impl = 0; impl < WEFT_SIMD_IMPL_COUNT; impl++) {
        const weft_simd_kernels_t* kt = weft_simd_impl_kernels((weft_simd_impl_t)impl);
        if (kt == NULL) {
            continue;
        }
        CHECK(weft_simd_force_impl(IMPL_NAMES[impl]) == 0, "pin %s", IMPL_NAMES[impl]);
        for (int si = 0; si < NSIZES; si++) {
            const size_t elems = SIZES[si];
            if (elems > POOL_BYTES / 4) {
                continue;
            }
            for (int di = 0; di < NSEEDS; di++) {
                const uint32_t seed = SEEDS[di];
                for (size_t off = 0; off <= 8; off += 4) {
                    uint32_t* src = (uint32_t*)(g_pool + off);
                    uint32_t rs = 0x1234u + (uint32_t)si * 97u + (uint32_t)di;
                    for (size_t i = 0; i < elems; i++) {
                        src[i] = ws_rng(&rs);
                    }
                    uint32_t* ref = (uint32_t*)(g_ref + off);
                    uint32_t* out = (uint32_t*)(g_out + off);

                    weft_simd_scalar_delta_encode(ref, src, elems, seed);
                    weft_simd_delta_encode(out, src, elems, seed);
                    CHECK(memcmp(out, ref, elems * 4) == 0,
                          "delta_enc %s elems=%zu seed=%u off=%u",
                          IMPL_NAMES[impl], elems, seed, (unsigned)off);
                    memcpy(out, src, elems * 4);
                    weft_simd_delta_encode(out, out, elems, seed);
                    CHECK(memcmp(out, ref, elems * 4) == 0,
                          "delta_enc-inplace %s elems=%zu seed=%u",
                          IMPL_NAMES[impl], elems, seed);

                    // decode(encode(x)) == x (modular round-trip)
                    weft_simd_scalar_delta_decode(ref, out, elems, seed);
                    CHECK(memcmp(ref, src, elems * 4) == 0,
                          "roundtrip %s elems=%zu seed=%u",
                          IMPL_NAMES[impl], elems, seed);
                    weft_simd_delta_decode(out, out, elems, seed);
                    CHECK(memcmp(out, src, elems * 4) == 0,
                          "roundtrip-inplace %s elems=%zu seed=%u",
                          IMPL_NAMES[impl], elems, seed);

                    // decode straight from src
                    weft_simd_scalar_delta_decode(ref, src, elems, seed);
                    weft_simd_delta_decode(out, src, elems, seed);
                    CHECK(memcmp(out, ref, elems * 4) == 0,
                          "delta_dec %s elems=%zu seed=%u off=%u",
                          IMPL_NAMES[impl], elems, seed, (unsigned)off);
                }
            }
        }
    }
    weft_simd_force_auto();
}

static void sweep_dot(void) {
    // (m, k, n, lda, ldb, ldc) grids: square, tall, wide, strided, tiny
    static const uint32_t G[][6] = {
        { 1, 1, 1, 1, 1, 1 },
        { 1, 5, 1, 5, 1, 1 },
        { 5, 1, 1, 1, 1, 1 },
        { 3, 7, 2, 8, 3, 4 },
        { 17, 3, 5, 5, 6, 7 },
        { 5, 33, 4, 33, 4, 4 },
        { 8, 64, 2, 64, 2, 2 },
        { 2, 1, 16, 16, 16, 16 },
        { 4, 4, 17, 17, 17, 17 },
        { 16, 16, 16, 16, 16, 16 },
        { 33, 9, 3, 9, 3, 3 },
        { 7, 129, 5, 129, 5, 5 },
    };
    for (int impl = 0; impl < WEFT_SIMD_IMPL_COUNT; impl++) {
        const weft_simd_kernels_t* kt = weft_simd_impl_kernels((weft_simd_impl_t)impl);
        if (kt == NULL) {
            continue;
        }
        CHECK(weft_simd_force_impl(IMPL_NAMES[impl]) == 0, "pin %s", IMPL_NAMES[impl]);
        for (int gi = 0; gi < (int)(sizeof(G) / sizeof(G[0])); gi++) {
            const uint32_t m = G[gi][0], k = G[gi][1], n = G[gi][2];
            const uint32_t lda = G[gi][3], ldb = G[gi][4], ldc = G[gi][5];
            const size_t a_elems = (size_t)(m - 1) * lda + k;
            const size_t b_elems = (size_t)(k - 1) * ldb + n;
            const size_t c_elems = (size_t)(m - 1) * ldc + n;
            CHECK(a_elems * 4 + b_elems * 4 + c_elems * 4 + 64 < POOL_BYTES,
                  "grid fits pool");
            float* a = (float*)g_pool;
            float* b = (float*)(g_pool + a_elems * 4 + 32);
            float* ref = (float*)g_ref;
            float* out = (float*)g_out;
            uint32_t rs = 0xBEEFu + (uint32_t)gi;
            for (size_t i = 0; i < a_elems; i++) {
                uint32_t u = ws_rng(&rs);
                float f;
                memcpy(&f, &u, 4);
                a[i] = f * 0.25f;
            }
            for (size_t i = 0; i < b_elems; i++) {
                uint32_t u = ws_rng(&rs);
                float f;
                memcpy(&f, &u, 4);
                b[i] = f * 0.25f;
            }
            memset(ref, 0xCC, (c_elems + 4) * 4);   // sentinels beyond C
            memset(out, 0xCC, (c_elems + 4) * 4);
            weft_simd_scalar_dot_f32(ref, a, b, m, k, n, lda, ldb, ldc);
            weft_simd_dot_f32(out, a, b, m, k, n, lda, ldb, ldc);
            CHECK(memcmp(out, ref, c_elems * 4) == 0,
                  "dot %s m=%u k=%u n=%u", IMPL_NAMES[impl], m, k, n);
            // kernels touch ONLY [0, c_elems): sentinel words unchanged
            for (size_t s = c_elems; s < c_elems + 4; s++) {
                CHECK(((uint32_t*)out)[s] == 0xCCCCCCCCu,
                      "dot %s sentinel %zu untouched", IMPL_NAMES[impl], s);
            }
        }
    }
    weft_simd_force_auto();
}

static void sweep_fletcher(void) {
    for (int impl = 0; impl < WEFT_SIMD_IMPL_COUNT; impl++) {
        const weft_simd_kernels_t* kt = weft_simd_impl_kernels((weft_simd_impl_t)impl);
        if (kt == NULL) {
            continue;
        }
        CHECK(weft_simd_force_impl(IMPL_NAMES[impl]) == 0, "pin %s", IMPL_NAMES[impl]);
        for (int si = 0; si < NSIZES; si++) {
            const size_t bytes = SIZES[si];
            if (bytes > POOL_BYTES || (bytes & 1u) != 0u) {
                continue;   // even byte counts only (u16 word pairs)
            }
            for (int di = 0; di < NSEEDS; di++) {
                const uint32_t stamp = SEEDS[di];
                uint32_t rs = 0xFEEDu + (uint32_t)si;
                for (size_t i = 0; i < bytes; i++) {
                    g_pool[i] = (unsigned char)ws_rng(&rs);
                }
                // word-aligned + odd-word offsets (2B steps stay word-aligned)
                for (size_t off = 0; off <= 6; off += 2) {
                    const uint32_t ref =
                        weft_simd_scalar_seqlock_checksum(g_pool + off, bytes, stamp);
                    const uint32_t got =
                        weft_simd_seqlock_checksum(g_pool + off, bytes, stamp);
                    CHECK(got == ref,
                          "fletcher %s bytes=%zu stamp=%u off=%u ref=%08X got=%08X",
                          IMPL_NAMES[impl], bytes, stamp, (unsigned)off, ref, got);
                }
            }
        }
        // distinct stamps must (generally) produce distinct digests: keyed
        const uint32_t d0 = weft_simd_seqlock_checksum("ABCD", 4, 0);
        const uint32_t d1 = weft_simd_seqlock_checksum("ABCD", 4, 1);
        CHECK(d0 != d1, "fletcher %s stamp-keyed", IMPL_NAMES[impl]);
    }
    weft_simd_force_auto();
}

// ---------------------------------------------------------------------------
// Golden fixtures (hand-computed, independent of both implementations)
// ---------------------------------------------------------------------------

static void golden_fixtures(void) {
    // Fletcher-32 goldens (see tools/spectrum/tests/ golden generator)
    CHECK(weft_simd_seqlock_checksum("\x00\x01\x02\x03", 4, 0x12345678u) ==
          0x12D0DA0Bu, "golden fletcher 1");
    CHECK(weft_simd_seqlock_checksum("ABCD", 4, 0) == 0xC2DAEB59u,
          "golden fletcher 2");
    CHECK(weft_simd_seqlock_checksum("\xff\xee\xdd\xcc\xbb\xaa\x99\x88", 8,
                                      0xDEADBEEFu) == 0xF85A71C0u,
          "golden fletcher 3");
    CHECK(weft_simd_seqlock_checksum("", 0, 1) == 0x1D62BACBu,
          "golden fletcher 4 (empty)");

    // normalize golden: (x - 0) * (1/256) for x in {0, 128, 256}
    {
        const float src[3] = { 0.0f, 128.0f, 256.0f };
        float out[3];
        weft_simd_normalize(out, src, 3, 0.0f, 1.0f / 256.0f);
        CHECK(out[0] == 0.0f && out[1] == 0.5f && out[2] == 1.0f,
              "golden normalize 1");
        const uint32_t want[3] = { 0x3EAAAAABu, 0x3F2AAAABu, 0x3F800000u };
        const float src2[3] = { 1.0f, 2.0f, 3.0f };
        uint32_t got[3];
        weft_simd_normalize((float*)(void*)got, src2, 3, 0.0f, 1.0f / 3.0f);
        CHECK(memcmp(got, want, 12) == 0, "golden normalize 2 (bit pattern)");
    }

    // delta golden: src = {10, 25, 27, 25}, seed 7 -> {3, 15, 2, -2 mod 2^32}
    {
        const uint32_t src[4] = { 10, 25, 27, 25 };
        uint32_t out[4];
        weft_simd_delta_encode(out, src, 4, 7);
        CHECK(out[0] == 3 && out[1] == 15 && out[2] == 2 && out[3] == 4294967294u,
              "golden delta encode");
        uint32_t back[4];
        weft_simd_delta_decode(back, out, 4, 7);
        CHECK(memcmp(back, src, 16) == 0, "golden delta roundtrip");
    }

    // dot golden: A(2x3) x B(3x2) = [[58, 64], [139, 154]]
    {
        const float a[6] = { 1, 2, 3, 4, 5, 6 };
        const float b[6] = { 7, 8, 9, 10, 11, 12 };
        float c[4];
        weft_simd_dot_f32(c, a, b, 2, 3, 2, 3, 2, 2);
        CHECK(c[0] == 58.0f && c[1] == 64.0f && c[2] == 139.0f && c[3] == 154.0f,
              "golden dot");
    }
}

// ---------------------------------------------------------------------------
// Dispatch API honesty
// ---------------------------------------------------------------------------

static void api_checks(void) {
    weft_simd_force_auto();
#if defined(__aarch64__)
    CHECK(strcmp(weft_simd_active_impl_name(), "neon") == 0 ||
              strcmp(weft_simd_active_impl_name(), "sve2") == 0 ||
              strcmp(weft_simd_active_impl_name(), "scalar") == 0,
          "auto resolves to a compiled arm impl, got %s",
          weft_simd_active_impl_name());
    CHECK(weft_simd_impl_available("scalar") == 1, "scalar available");
    CHECK(weft_simd_impl_available("nope") == 0, "bogus name unavailable");
    CHECK(weft_simd_force_impl("nope") == -1, "bogus pin refused");
    CHECK(weft_simd_force_impl("neon") == 0, "neon pin");
    CHECK(strcmp(weft_simd_active_impl_name(), "neon") == 0, "pinned name");
#elif defined(__riscv)
    CHECK(strcmp(weft_simd_active_impl_name(), "rvv") == 0 ||
              strcmp(weft_simd_active_impl_name(), "scalar") == 0,
          "auto resolves to a compiled riscv impl, got %s",
          weft_simd_active_impl_name());
    CHECK(weft_simd_impl_available("scalar") == 1, "scalar available");
    CHECK(weft_simd_impl_available("nope") == 0, "bogus name unavailable");
    CHECK(weft_simd_force_impl("nope") == -1, "bogus pin refused");
#else
    CHECK(strcmp(weft_simd_active_impl_name(), "avx512") == 0 ||
              strcmp(weft_simd_active_impl_name(), "avx2") == 0 ||
              strcmp(weft_simd_active_impl_name(), "scalar") == 0,
          "auto resolves to a compiled x86 impl, got %s",
          weft_simd_active_impl_name());
    CHECK(weft_simd_impl_available("scalar") == 1, "scalar available");
    CHECK(weft_simd_impl_available("nope") == 0, "bogus name unavailable");
    CHECK(weft_simd_force_impl("nope") == -1, "bogus pin refused");
    CHECK(weft_simd_force_impl("avx512") == 0, "avx512 pin");
    CHECK(strcmp(weft_simd_active_impl_name(), "avx512") == 0, "pinned name");
#endif
    weft_simd_force_scalar();
    CHECK(strcmp(weft_simd_active_impl_name(), "scalar") == 0, "scalar pin");
    weft_simd_force_auto();
}

int main(void) {
    printf("simd-oracle: compiled caps = 0x%X\n", weft_simd_compiled_caps());
    golden_fixtures();
    sweep_normalize();
    sweep_delta();
    sweep_dot();
    sweep_fletcher();
    api_checks();
    if (g_failures != 0) {
        printf("simd-oracle: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("simd-oracle: all paths bit-exact; PASS\n");
    return 0;
}
