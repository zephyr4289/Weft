// test_util.h — Pillar 8 verification test belt (lean, single-process).
//
// Mirrors the P6/P7 belt discipline: allocator interposition behind
// TU_ALLOC_GUARD (linked with -Wl,--wrap=malloc,calloc,realloc on the
// plain legs), a raw monotonic clock, a setup-time histogram, and a
// quiet check counter that exits nonzero on any failure (fail-closed).
// This pillar's batteries are single-process simulators, so the
// multi-process machinery (PDEATHSIG children, /dev/shm audits) is not
// needed and is deliberately not carried over.

#ifndef VERIFY_NATIVE_TEST_UTIL_H
#define VERIFY_NATIVE_TEST_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- allocator interposition (plain legs link with --wrap) --------------- */

extern unsigned long tu_alloc_count;

/* --- environment ---------------------------------------------------------- */

int64_t tu_now_ns(void);
void tu_usleep(unsigned long us);
unsigned tu_cpu_count(void);

/* 1 under ThreadSanitizer instrumentation (relaxed budgets), else 0. */
int tu_tsan(void);

/* 1 under AddressSanitizer instrumentation, else 0 (physical timing
 * measurements — coherency A/Bs — are plain-leg evidence). */
int tu_asan(void);

/* 1 when the online CPUs are SMT siblings of one physical core (the
 * container case): an L1-shared pair cannot exhibit cross-core
 * false-sharing penalties, so the A/B assert is declared N/A. */
int tu_smt_shared(void);

/* 1 when WEFT_QUICK=1 is exported (sanitizer legs of long batteries). */
int tu_quick(void);

/* Iteration scaler: full on plain, divided under QUICK / TSan. */
uint64_t tu_iters(uint64_t base);

/* --- check framework (quiet pass, loud fail, exit code at end) ----------- */

extern unsigned long tu_pass;
extern unsigned long tu_fail;

#define TU_CHECK(cond)                                                    \
    do {                                                                  \
        if (cond) {                                                       \
            tu_pass++;                                                    \
        } else {                                                          \
            tu_fail++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            fflush(stdout);                                               \
        }                                                                 \
    } while (0)

#define TU_CHECKF(cond, ...)                                              \
    do {                                                                  \
        if (cond) {                                                       \
            tu_pass++;                                                    \
        } else {                                                          \
            tu_fail++;                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
            fflush(stdout);                                               \
        }                                                                 \
    } while (0)

/* Print the summary and terminate the process (nonzero on any failure). */
void tu_done(const char *suite_name) __attribute__((noreturn));

/* --- setup-time histogram (malloc'd once, NEVER inside a hot window) ----- */

typedef struct tu_hist {
    double *v;
    size_t n;
    size_t cap;
} tu_hist_t;

int tu_hist_init(tu_hist_t *h, size_t cap);
void tu_hist_free(tu_hist_t *h);
void tu_hist_add(tu_hist_t *h, double ns);
double tu_hist_pct(const tu_hist_t *h, double pct);

#ifdef __cplusplus
}
#endif

#endif  /* VERIFY_NATIVE_TEST_UTIL_H */
