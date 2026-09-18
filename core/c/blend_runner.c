// blend_runner.c — the Q12 blend benchmark (Series 8).
//
// Measures the per-frame blend cost the cadence consumers pay, scalar vs
// the runtime-dispatched SIMD path, at 1080p / 4K / 8K frame sizes:
//
//   1080p = 1920*1080 words = 2,073,600 u32   ( 8.3 MB per buffer)
//   4K    = 3840*2160 words = 8,294,400 u32   (33.2 MB per buffer)
//   8K    = 7680*4320 words = 33,177,600 u32  (132.7 MB per buffer)
//
// The lead's bar: sub-50-microsecond synthesis at 4K/8K. Whether that bar
// is met on a given machine is a MEMORY-BANDWIDTH question (33 MB x 3
// buffers must move per blend); the runner reports the measured numbers —
// ns/frame and achieved GB/s — and the ALU-side speedup over scalar, so
// the honest claim is on the table per machine. 60 Hz at 4K needs
// ~6 GB/s of blend traffic; the SIMD path's GB/s number IS the headroom
// statement.

#include "blend_q12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t xorshift32(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static double bench(void (*fn)(const uint32_t *, const uint32_t *, uint32_t *,
                               size_t, unsigned),
                    const uint32_t *prev, const uint32_t *newest,
                    uint32_t *out, size_t words, unsigned alpha,
                    int iters) {
    /* warmup (JIT-free, but cache/TLB warm) */
    for (int i = 0; i < 3; i++) fn(prev, newest, out, words, alpha);
    const double t0 = now_ns();
    for (int i = 0; i < iters; i++) fn(prev, newest, out, words, alpha);
    const double t1 = now_ns();
    return (t1 - t0) / iters;
}

int main(void) {
    static const struct {
        const char *name;
        size_t words;
        int iters;
    } SIZES[] = {
        /* hot: 256x256 words x 3 bufs ~ 786 KB — LLC-resident, the regime
         * the governed consumer actually runs in (the two-frame history is
         * re-read every tick as alpha advances; the ALU advantage of SIMD
         * shows here, uncloaked by DRAM) */
        {"256x256-hot", 256u * 256u, 3000},
        {"1080p", 1920u * 1080u, 40},
        {"4K", 3840u * 2160u, 20},
        {"8K", 7680u * 4320u, 6},
    };
    const unsigned alpha = 1234; /* mid-ramp alpha (30% into the window) */

    printf("== blend_runner: dispatch=%s ==\n",
           weft_blend_q12_impl_name(weft_blend_q12_best()));

    for (size_t s = 0; s < sizeof(SIZES) / sizeof(SIZES[0]); s++) {
        const size_t words = SIZES[s].words;
        uint32_t *prev = malloc(words * sizeof(uint32_t));
        uint32_t *newest = malloc(words * sizeof(uint32_t));
        uint32_t *out = malloc(words * sizeof(uint32_t));
        if (!prev || !newest || !out) {
            fprintf(stderr, "blend_runner: OOM at %s\n", SIZES[s].name);
            return 1;
        }
        uint32_t st = 0xC0FFEE;
        for (size_t i = 0; i < words; i++) {
            st = xorshift32(st); prev[i] = st;
            st = xorshift32(st); newest[i] = st;
        }
        const double ns_scalar =
            bench(weft_blend_q12_scalar, prev, newest, out, words, alpha,
                  SIZES[s].iters);
        const double ns_best =
            bench(weft_blend_q12, prev, newest, out, words, alpha,
                  SIZES[s].iters);
        const double bytes = (double)words * 4.0 * 3.0; /* read 2, write 1 */
        const double gbps_scalar = bytes / (ns_scalar) * 1e9 / 1e9;
        const double gbps_best = bytes / (ns_best) * 1e9 / 1e9;
        printf("%s (%zu words): scalar %8.0f ns/frame (%5.1f GB/s) | "
               "%s %8.0f ns/frame (%5.1f GB/s) | speedup %.2fx\n",
               SIZES[s].name, words, ns_scalar, gbps_scalar,
               weft_blend_q12_impl_name(weft_blend_q12_best()), ns_best,
               gbps_best, ns_scalar / ns_best);
        free(prev);
        free(newest);
        free(out);
    }
    return 0;
}
