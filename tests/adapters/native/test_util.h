// test_util.h — shared belt for the adapters native batteries: honest
// pass/fail accounting, fd/SHM/RSS leak audits, latency histograms, and
// the allocator interposition guard (plain leg: -Wl,--wrap=malloc,...).

#ifndef WEFT_TESTS__TEST_UTIL_H_
#define WEFT_TESTS__TEST_UTIL_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


extern unsigned long tu_alloc_count; /* mallocs seen by the guard */

int64_t tu_now_ns(void);
unsigned tu_fd_count(void);
unsigned tu_shm_weft_count(const char *prefix);
long tu_rss_kb(void);
void tu_usleep(unsigned long us);

/* Call immediately after fork() in EVERY child. Arms PR_SET_PDEATHSIG so
 * a killed/timeout'd parent takes the child down too — a child spinning
 * on a dead parent's ring otherwise burns a whole core forever and
 * poisons every later run in the container (observed as 440 s of utime
 * from a leaked bench child; D-62 §C.5). Fails closed: if the parent
 * already died before the prctl landed, the child exits immediately. */
void tu_child_setup(void);

typedef struct tu_hist {
    double *v;
    size_t n, cap;
} tu_hist_t;

int tu_hist_init(tu_hist_t *h, size_t cap);
void tu_hist_free(tu_hist_t *h);
void tu_hist_add(tu_hist_t *h, double ns);
double tu_hist_pct(const tu_hist_t *h, double pct); /* 0..100 */

#define TU_PASS(name) \
    do { printf("PASS  %s\n", (name)); fflush(stdout); } while (0)

#define TU_FAIL(name, fmt, ...) \
    do { \
        printf("FAIL  %s: " fmt "\n", (name), ##__VA_ARGS__); \
        fflush(stdout); \
        exit(2); \
    } while (0)

#define TU_CHECK(cond, name) \
    do { \
        if (!(cond)) { printf("FAIL  %s\n", (name)); fflush(stdout); exit(2); } \
    } while (0)

#define TU_ASSERT(cond, name, fmt, ...) \
    do { \
        if (!(cond)) TU_FAIL(name, fmt, ##__VA_ARGS__); \
    } while (0)

#endif  // WEFT_TESTS__TEST_UTIL_H_
