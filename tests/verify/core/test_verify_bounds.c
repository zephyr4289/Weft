/* ---------------------------------------------------------------------------
 * test_verify_bounds.c — Weft Pillar 8: boundary proof stress tests
 *
 * GATE COVERAGE: G3/G4 input for core/c/verify (mandate §2.D.3):
 *   "Boundary proof stress tests (10,000,000 cycles, 0 failures)".
 *
 * THREE LAYERS:
 *   1. Known-answer edge cases — including the classic overflow traps
 *      (start+len wraparound, stride*count wraparound, UINT64_MAX edges).
 *   2. 10,000,000-cycle randomized differential stress against an
 *      INDEPENDENTLY formulated reference (overflow-checked multiplication
 *      vs the shipped division-based check) — 0 mismatches allowed.
 *   3. Zero-allocation probe (Law 1): this binary is linked with
 *      -Wl,--wrap=malloc,calloc,realloc,strdup and WEFT_VERIFY_WRAP_PROBE;
 *      the steady-state stress loop must record ZERO heap calls.
 *
 * Deterministic: splitmix64 with the fixed seed below (mandate §3.2).
 * ------------------------------------------------------------------------- */

#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "weft_verify.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* malloc interposition probe (Law 1) — active when the binary is     */
/* linked with -Wl,--wrap=... and compiled with -DWEFT_VERIFY_WRAP_PROBE */
/* ------------------------------------------------------------------ */

#ifdef WEFT_VERIFY_WRAP_PROBE
extern void *__real_malloc(size_t n);
extern void *__real_calloc(size_t n, size_t m);
extern void *__real_realloc(void *p, size_t n);
extern void *__real_free(void *p);
extern char *__real_strdup(const char *s);

static unsigned long long g_probe_calls = 0u;

void *__wrap_malloc(size_t n)
{
    g_probe_calls++;
    return __real_malloc(n);
}

void *__wrap_calloc(size_t n, size_t m)
{
    g_probe_calls++;
    return __real_calloc(n, m);
}

void *__wrap_realloc(void *p, size_t n)
{
    g_probe_calls++;
    return __real_realloc(p, n);
}

void __wrap_free(void *p)
{
    g_probe_calls++;
    __real_free(p);
}

char *__wrap_strdup(const char *s)
{
    g_probe_calls++;
    return __real_strdup(s);
}

static unsigned long long weft_probe_calls(void)
{
    return g_probe_calls;
}
#else
static unsigned long long weft_probe_calls(void)
{
    return 0u;
}
#endif

/* ------------------------------------------------------------------ */
/* deterministic PRNG (splitmix64, fixed seed — mandate §3.2)          */
/* ------------------------------------------------------------------ */

static uint64_t g_rng = 0x5745465431303853ULL; /* "WEFT1808" as seed */

static uint64_t weft_rng(void)
{
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* ------------------------------------------------------------------ */
/* independent reference implementations (structurally different)     */
/* ------------------------------------------------------------------ */

static weft_verify_status_t ref_ring_span(uint64_t start, uint64_t len,
                                          uint64_t cap)
{
    if (start > cap) {
        return WEFT_VERIFY_EBOUNDS;
    }
    if (len > cap - start) {
        return WEFT_VERIFY_EOVERFLOW;
    }
    return WEFT_VERIFY_OK;
}

static weft_verify_status_t ref_vlf(uint64_t off, uint64_t size,
                                    uint64_t rec)
{
    if (off > rec || size > rec - off) {
        return (off > rec) ? WEFT_VERIFY_EBOUNDS : WEFT_VERIFY_EOVERFLOW;
    }
    return WEFT_VERIFY_OK;
}

static weft_verify_status_t ref_dma(uint64_t stride, uint64_t elem,
                                    uint64_t count, uint64_t buflen)
{
    if (stride < elem) {
        return WEFT_VERIFY_ESTRIDE;
    }
    if (count != 0u) {
        /* overflow-checked multiplication — a genuinely different route
         * than the shipped division-based fast path */
        if (stride > UINT64_MAX / count) {
            return WEFT_VERIFY_EOVERFLOW;
        }
        if (stride * count > buflen) {
            return WEFT_VERIFY_EOVERFLOW;
        }
    }
    return WEFT_VERIFY_OK;
}

static weft_verify_status_t ref_align(uintptr_t a, size_t al)
{
    if (al == 0u || (al & (al - 1u)) != 0u) {
        return WEFT_VERIFY_EINVALID;
    }
    if ((a & (uintptr_t)(al - 1u)) != 0u) {
        return WEFT_VERIFY_EMISALIGN;
    }
    return WEFT_VERIFY_OK;
}

/* ------------------------------------------------------------------ */
/* layer 1: known-answer edge cases                                   */
/* ------------------------------------------------------------------ */

static int g_failures = 0;

#define CHECK(desc, got, want)                                                \
    do {                                                                      \
        if ((got) != (want)) {                                                \
            fprintf(stderr, "FAIL %s:%d %s: got %d want %d\n", __FILE__,      \
                    __LINE__, (desc), (int)(got), (int)(want));               \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

static void layer1_known_answers(void)
{
    CHECK("idx 0/1", weft_verify_index(0, 1), WEFT_VERIFY_OK);
    CHECK("idx 1/1", weft_verify_index(1, 1), WEFT_VERIFY_EBOUNDS);
    CHECK("idx max/max", weft_verify_index(UINT64_MAX, UINT64_MAX),
          WEFT_VERIFY_EBOUNDS);
    CHECK("idx max-1/max", weft_verify_index(UINT64_MAX - 1u, UINT64_MAX),
          WEFT_VERIFY_OK);
    CHECK("idx 0/0", weft_verify_index(0, 0), WEFT_VERIFY_EBOUNDS);

    CHECK("ring 8/8", weft_verify_ring_index(8, 8), WEFT_VERIFY_EBOUNDS);
    CHECK("ring 7/8", weft_verify_ring_index(7, 8), WEFT_VERIFY_OK);

    CHECK("span 0+8/8", weft_verify_ring_span(0, 8, 8), WEFT_VERIFY_OK);
    CHECK("span 8+8/8 (out of region)", weft_verify_ring_span(8, 8, 8),
          WEFT_VERIFY_EOVERFLOW);
    CHECK("span 9/0/8", weft_verify_ring_span(9, 0, 8), WEFT_VERIFY_EBOUNDS);
    CHECK("span 4+8/8 (overflow trap)",
          weft_verify_ring_span(4, 8, 8), WEFT_VERIFY_EOVERFLOW);
    /* start+len wraps around UINT64_MAX in naive arithmetic */
    CHECK("span max wrap", weft_verify_ring_span(UINT64_MAX - 3u, 8u,
                                                 UINT64_MAX),
          WEFT_VERIFY_EOVERFLOW);
    CHECK("span max exact", weft_verify_ring_span(UINT64_MAX - 3u, 3u,
                                                  UINT64_MAX),
          WEFT_VERIFY_OK);

    CHECK("vlf 0/64/64", weft_verify_vlf_offset(0, 64, 64), WEFT_VERIFY_OK);
    CHECK("vlf 60/8/64 (overflow trap)", weft_verify_vlf_offset(60, 8, 64),
          WEFT_VERIFY_EOVERFLOW);
    CHECK("vlf 64/0/64", weft_verify_vlf_offset(64, 0, 64), WEFT_VERIFY_OK);
    CHECK("vlf 65/0/64", weft_verify_vlf_offset(65, 0, 64),
          WEFT_VERIFY_EBOUNDS);

    CHECK("dma 64/64/10/640", weft_verify_dma_stride(64, 64, 10, 640),
          WEFT_VERIFY_OK);
    CHECK("dma 32/64 (stride<elem)", weft_verify_dma_stride(32, 64, 10, 640),
          WEFT_VERIFY_ESTRIDE);
    CHECK("dma 64/64/10/639", weft_verify_dma_stride(64, 64, 10, 639),
          WEFT_VERIFY_EOVERFLOW);
    /* stride*count wraps in naive arithmetic */
    CHECK("dma wrap trap", weft_verify_dma_stride(UINT64_MAX / 2u + 1u, 1u,
                                                  2u, UINT64_MAX),
          WEFT_VERIFY_EOVERFLOW);
    CHECK("dma count 0", weft_verify_dma_stride(64, 64, 0, 1),
          WEFT_VERIFY_OK);
    CHECK("dma zero stride elem 0", weft_verify_dma_stride(0, 0, 5, 0),
          WEFT_VERIFY_OK);

    CHECK("align 64/64", weft_verify_alignment(64, 64), WEFT_VERIFY_OK);
    CHECK("align 65/64", weft_verify_alignment(65, 64), WEFT_VERIFY_EMISALIGN);
    CHECK("align 96/32", weft_verify_alignment(96, 32), WEFT_VERIFY_OK);
    CHECK("align 3/0", weft_verify_alignment(3, 0), WEFT_VERIFY_EINVALID);
    CHECK("align 3/12", weft_verify_alignment(3, 12), WEFT_VERIFY_EINVALID);
    CHECK("align 0/128", weft_verify_alignment(0, 128), WEFT_VERIFY_OK);

    CHECK("selfcheck", weft_verify_abi_selfcheck(), WEFT_VERIFY_OK);
    CHECK("abi version", (int)weft_verify_abi_version(), 0x0100);
    printf("L1 known-answer edges: done (%d failures)\n", g_failures);
}

/* ------------------------------------------------------------------ */
/* layer 2: 10,000,000-cycle differential stress                      */
/* ------------------------------------------------------------------ */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void layer2_stress(unsigned long long cycles)
{
    unsigned long long mism = 0u;
    unsigned long long viol_seen = 0u;
    uint64_t viol_before = weft_verify_violation_count();
    unsigned long long probe0 = weft_probe_calls();
    double t0 = now_ms();

    for (unsigned long long i = 0; i < cycles; i++) {
        uint64_t a = weft_rng();
        uint64_t b = weft_rng();
        uint64_t c = weft_rng();
        uint64_t d = weft_rng();

        /* bias towards boundary values so edges are densely covered */
        switch (i & 7u) {
        case 0: a = (a & 1u) ? UINT64_MAX : 0u; break;
        case 1: b = (b & 1u) ? UINT64_MAX : 1u; break;
        case 2: c = (c & 7u) ? (c & 0xFFu) : c; break;
        case 3: d = (d & 1u) ? 0u : d; break;
        default: break;
        }

        weft_verify_status_t s1 = weft_verify_ring_span_fast(a, b, c);
        if (s1 != ref_ring_span(a, b, c)) {
            mism++;
        }
        weft_verify_status_t s2 = weft_verify_vlf_offset_fast(a, b, c);
        if (s2 != ref_vlf(a, b, c)) {
            mism++;
        }
        weft_verify_status_t s3 = weft_verify_dma_stride_fast(a, b, c, d);
        if (s3 != ref_dma(a, b, c, d)) {
            mism++;
        }
        weft_verify_status_t s4 =
            weft_verify_alignment_fast((uintptr_t)(a & 0xFFFFFFFFFFFFu),
                                       (size_t)(b & 0x3FFu) | 1u);
        if (s4 != ref_align((uintptr_t)(a & 0xFFFFFFFFFFFFu),
                            (size_t)(b & 0x3FFu) | 1u)) {
            mism++;
        }
        weft_verify_status_t s5 = weft_verify_alignment_fast(
            (uintptr_t)(c & 0xFFFFFFFFu), (size_t)1u << (d & 15u));
        if (s5 != ref_align((uintptr_t)(c & 0xFFFFFFFFu),
                            (size_t)1u << (d & 15u))) {
            mism++;
        }
        /* exercise the recording (cold) path through the checked macros */
        if ((i & 1023u) == 0u) {
            if (!WEFT_BOUNDS_CHECK(a & 3u, 2u)) {
                viol_seen++; /* a&3 in {2,3} is out of bounds — expected */
            }
        }
    }

    double dt = now_ms() - t0;
    uint64_t viol_after = weft_verify_violation_count();
    unsigned long long probe1 = weft_probe_calls();

    printf("L2 stress: %llu cycles, %llu mismatches, %llu macro "
           "violations observed, counter delta %" PRIu64 ", %.1f ms "
           "(%.1f ns/check-set)\n",
           cycles, mism, viol_seen, viol_after - viol_before, dt,
           (cycles > 0u) ? (dt * 1000000.0 / (double)cycles) : 0.0);

    if (mism != 0u) {
        fprintf(stderr, "FAIL: differential mismatches %llu != 0\n", mism);
        g_failures++;
    }
    /* every macro-detected violation must have hit the ledger exactly */
    if (viol_after - viol_before != viol_seen) {
        fprintf(stderr,
                "FAIL: violation ledger delta %" PRIu64 " != observed %llu\n",
                viol_after - viol_before, viol_seen);
        g_failures++;
    }
    if (probe1 != probe0) {
        fprintf(stderr,
                "FAIL(Law1): stress loop performed %llu heap call(s)\n",
                probe1 - probe0);
        g_failures++;
    }
}

/* ------------------------------------------------------------------ */
/* layer 3: compile-time proof macros smoke (runtime-observable)      */
/* ------------------------------------------------------------------ */

static _Alignas(64) uint32_t g_probe_storage[16];

static void layer3_macro_proofs(void)
{
    WEFT_BOUNDS_CHECK_CONST(0, 4);      /* compiles = passes */
    WEFT_PROVE_ALIGNED_CONST(0x1000u, 64);
    if (!WEFT_BOUNDS_CHECK(2, 8)) {
        fprintf(stderr, "FAIL: WEFT_BOUNDS_CHECK(2,8) must hold\n");
        g_failures++;
    }
    if (!WEFT_PROVE_ALIGNED(g_probe_storage, 64)) {
        fprintf(stderr, "FAIL: WEFT_PROVE_ALIGNED(.,64) must hold\n");
        g_failures++;
    }
    if (WEFT_PROVE_ALIGNED((uintptr_t)g_probe_storage + 1u, 64)) {
        fprintf(stderr, "FAIL: +1 must be misaligned\n");
        g_failures++;
    }
    printf("L3 macro proofs: done\n");
}

int main(int argc, char **argv)
{
    unsigned long long cycles = 10000000ULL;
    if (argc > 1 && strcmp(argv[1], "--san") == 0) {
        cycles = 1000000ULL; /* reduced budget under sanitizers */
    }
    /* static stdout buffer: never allocate inside the probe window */
    static char obuf[1 << 16];
    setvbuf(stdout, obuf, _IOFBF, sizeof(obuf));

    printf("weft_verify bounds stress — %s\n",
           (argc > 1) ? argv[1] : "full");
    printf("engine: abi=0x%04x violations=%" PRIu64 "\n",
           (unsigned)weft_verify_abi_version(), weft_verify_violation_count());

    layer1_known_answers();
    layer2_stress(cycles);
    layer3_macro_proofs();

    printf("VERIFY-BOUNDS %s (%d failures)\n",
           (g_failures == 0) ? "PASS" : "FAIL", g_failures);
    fflush(stdout);
    return (g_failures == 0) ? 0 : 1;
}
