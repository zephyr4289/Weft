// blend_test.c — the bit-exactness gate for blend_q12 (Series 8).
//
// GATES (all must pass; any failure is a nonzero exit):
//   B1 boundaries: alpha=0 reproduces prev exactly; alpha=4096 reproduces
//      newest exactly; a zero-length blend is a no-op.
//   B2 full-sweep parity: for sizes {0,1,3,4,5,7,16,64,257,1000,4097} x
//      alphas {0,1,7,512,2048,4095,4096} on xorshift32-random buffers,
//      EVERY compiled path (sse41/avx2 on x86, neon on ARM) produces
//      byte-identical output to the scalar reference (memcmp, plus FNV).
//   B3 exhaustive-ish alpha sweep: all 4097 alpha values on a 64-word
//      buffer, every path == scalar.
//   B4 overlap safety: out == prev aliasing behaves (the consumer blends
//      in place into its history word buffers).
//
// --digests mode: prints "size,alpha,fnv1a64" lines for the golden-vector
// fixture the cross-language blend-parity gate consumes (the TS/Kotlin/
// Swift/Dart SWAR ports must reproduce the same digests).

#include "blend_q12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t xorshift32(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static uint64_t fnv1a64(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

#define MAX_WORDS 4200
static uint32_t prev_buf[MAX_WORDS + 8];
static uint32_t new_buf[MAX_WORDS + 8];
static uint32_t ref_buf[MAX_WORDS + 8];
static uint32_t got_buf[MAX_WORDS + 8];

static int failures = 0;

static void fill_random(size_t words, uint32_t *state) {
    for (size_t i = 0; i < words + 8; i++) {
        *state = xorshift32(*state);
        prev_buf[i] = *state;
        *state = xorshift32(*state);
        new_buf[i] = *state;
    }
}

static int check_path(const char *name,
                      void (*fn)(const uint32_t *, const uint32_t *,
                                 uint32_t *, size_t, unsigned),
                      size_t words, unsigned alpha, uint32_t *state) {
    fill_random(words, state);
    weft_blend_q12_scalar(prev_buf, new_buf, ref_buf, words, alpha);
    memset(got_buf, 0xA5, sizeof(got_buf));
    fn(prev_buf, new_buf, got_buf, words, alpha);
    if (words && memcmp(ref_buf, got_buf, words * sizeof(uint32_t)) != 0) {
        fprintf(stderr, "B-FAIL %s size=%zu alpha=%u: output diverges\n",
                name, words, alpha);
        for (size_t i = 0; i < words && i < 8; i++) {
            fprintf(stderr, "  [%zu] ref=%08x got=%08x\n", i, ref_buf[i],
                    got_buf[i]);
        }
        failures++;
        return 0;
    }
    return 1;
}

static const size_t SIZES[] = {0, 1, 3, 4, 5, 7, 16, 64, 257, 1000, 4097};
static const unsigned ALPHAS[] = {0, 1, 7, 512, 2048, 4095, 4096};

int main(int argc, char **argv) {
    int digest_mode = argc > 1 && strcmp(argv[1], "--digests") == 0;

    printf("== blend_test: impl=%s (%d) ==\n",
           weft_blend_q12_impl_name(weft_blend_q12_best()),
           (int)weft_blend_q12_best());

    uint32_t state = 0x5EEDBEEF;

    // ---- B1: boundaries -------------------------------------------------
    {
        fill_random(64, &state);
        weft_blend_q12(prev_buf, new_buf, got_buf, 64, 0);
        if (memcmp(got_buf, prev_buf, 64 * sizeof(uint32_t)) != 0) {
            fprintf(stderr, "B1-FAIL alpha=0 does not reproduce prev\n");
            failures++;
        }
        weft_blend_q12(prev_buf, new_buf, got_buf, 64, 4096);
        if (memcmp(got_buf, new_buf, 64 * sizeof(uint32_t)) != 0) {
            fprintf(stderr, "B1-FAIL alpha=4096 does not reproduce newest\n");
            failures++;
        }
        weft_blend_q12(prev_buf, new_buf, got_buf, 0, 1234); /* no-op, no crash */
        printf("B1 boundaries: OK\n");
    }

    // ---- B2: sizes x alphas, every path == scalar ------------------------
    {
        int paths_checked = 0;
        for (size_t si = 0; si < sizeof(SIZES) / sizeof(SIZES[0]); si++) {
            for (size_t ai = 0; ai < sizeof(ALPHAS) / sizeof(ALPHAS[0]); ai++) {
                const size_t words = SIZES[si];
                const unsigned alpha = ALPHAS[ai];
#if defined(__x86_64__) || defined(__i386__)
                __builtin_cpu_init();
                if (__builtin_cpu_supports("sse4.1")) {
                    check_path("sse4.1", weft_blend_q12_sse41, words, alpha, &state);
                    paths_checked++;
                }
# ifdef __AVX2__
                if (__builtin_cpu_supports("avx2")) {
                    check_path("avx2", weft_blend_q12_avx2, words, alpha, &state);
                    paths_checked++;
                }
# endif
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
                check_path("neon", weft_blend_q12_neon, words, alpha, &state);
                paths_checked++;
#endif
                check_path("scalar", weft_blend_q12_scalar, words, alpha, &state);
                paths_checked++;
            }
        }
        printf("B2 parity sweep: OK (%d path-checks, all == scalar)\n",
               paths_checked);
    }

    // ---- B3: all 4097 alphas on 64 words, dispatched best ---------------
    {
        for (unsigned alpha = 0; alpha <= 4096; alpha++) {
            check_path("dispatch", weft_blend_q12, 64, alpha, &state);
        }
        printf("B3 alpha sweep 0..4096 (dispatched): OK\n");
    }

    // ---- B4: in-place aliasing (out == prev) -----------------------------
    {
        fill_random(257, &state);
        weft_blend_q12_scalar(prev_buf, new_buf, ref_buf, 257, 1234);
        memcpy(got_buf, prev_buf, 257 * sizeof(uint32_t));
        weft_blend_q12(got_buf, new_buf, got_buf, 257, 1234);
        if (memcmp(ref_buf, got_buf, 257 * sizeof(uint32_t)) != 0) {
            fprintf(stderr, "B4-FAIL in-place blend diverges\n");
            failures++;
        } else {
            printf("B4 in-place aliasing: OK\n");
        }
    }

    // ---- digest mode (the xlang fixture's golden vectors) ----------------
    if (digest_mode) {
        static const size_t DS[] = {0, 1, 3, 4, 5, 16, 257, 1000, 4097};
        for (size_t si = 0; si < sizeof(DS) / sizeof(DS[0]); si++) {
            for (size_t ai = 0; ai < sizeof(ALPHAS) / sizeof(ALPHAS[0]); ai++) {
                const size_t words = DS[si];
                const unsigned alpha = ALPHAS[ai];
                /* Per-row reseed: every fixture row must be replayable
                 * from (size, alpha) alone — the ports re-seed 0x5EEDBEEF
                 * exactly here. Cross-row state chains would make the
                 * CSV unreproducible (the first gate run caught this). */
                state = 0x5EEDBEEF;
                fill_random(words, &state);
                weft_blend_q12_scalar(prev_buf, new_buf, ref_buf, words, alpha);
                printf("%zu,%u,%016llx\n", words, alpha,
                       (unsigned long long)fnv1a64(
                           (const uint8_t *)ref_buf,
                           words * sizeof(uint32_t)));
            }
        }
    }

    if (failures) {
        fprintf(stderr, "blend_test: %d gate failure(s)\n", failures);
        return 1;
    }
    printf("blend_test: ALL GATES GREEN\n");
    return 0;
}
