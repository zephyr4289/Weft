// test_util.c — Pillar 8 verification test belt implementation.

#include "test_util.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

unsigned long tu_alloc_count = 0;
unsigned long tu_pass = 0;
unsigned long tu_fail = 0;

#ifdef TU_ALLOC_GUARD
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void *__real_realloc(void *, size_t);

void *__wrap_malloc(size_t n) {
    tu_alloc_count++;
    return __real_malloc(n);
}
void *__wrap_calloc(size_t a, size_t b) {
    tu_alloc_count++;
    return __real_calloc(a, b);
}
void *__wrap_realloc(void *p, size_t n) {
    tu_alloc_count++;
    return __real_realloc(p, n);
}
#endif

int64_t tu_now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

void tu_usleep(unsigned long us) {
    struct timespec ts = {.tv_sec = (time_t)(us / 1000000ul),
                          .tv_nsec = (long)(us % 1000000ul) * 1000};
    (void)nanosleep(&ts, NULL);
}

unsigned tu_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) {
        return 1u;
    }
    return (unsigned)n;
}

int tu_tsan(void) {
#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
    return 1;
#else
    return 0;
#endif
}

int tu_asan(void) {
#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
    return 1;
#else
    return 0;
#endif
}

int tu_smt_shared(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        FILE *f = fopen(
            "/sys/devices/system/cpu/cpu0/topology/"
            "thread_siblings_list",
            "r");
        if (f != NULL) {
            char buf[256];
            buf[0] = '\0';
            if (fgets(buf, sizeof buf, f) != NULL) {
                for (const char *p = buf; *p != '\0'; p++) {
                    if (*p == ',' || *p == '-') {
                        cached = 1;  /* cpu0 shares a physical core */
                        break;
                    }
                }
            }
            (void)fclose(f);
        }
    }
    return cached;
}

int tu_quick(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("WEFT_QUICK");
        cached = (v != NULL && v[0] == '1') ? 1 : 0;
    }
    return cached;
}

uint64_t tu_iters(uint64_t base) {
    uint64_t v = base;
    if (tu_quick()) {
        v /= 10u;
    }
    if (tu_tsan()) {
        v /= 5u;
    }
    return v < 1u ? 1u : v;
}

void tu_done(const char *suite_name) __attribute__((noreturn));
void tu_done(const char *suite_name) {
    printf("----------------------------------------------------------------\n");
    printf("%s: %lu checks, %lu failures\n", suite_name, tu_pass, tu_fail);
    if (tu_fail == 0ul) {
        printf("%s: GREEN\n", suite_name);
        exit(0);
    }
    printf("%s: RED\n", suite_name);
    exit(1);
}

int tu_hist_init(tu_hist_t *h, size_t cap) {
    h->v = calloc(cap, sizeof(double));
    if (h->v == NULL) {
        return -1;
    }
    h->n = 0;
    h->cap = cap;
    return 0;
}

void tu_hist_free(tu_hist_t *h) {
    free(h->v);
    h->v = NULL;
    h->n = h->cap = 0;
}

void tu_hist_add(tu_hist_t *h, double ns) {
    if (h->n < h->cap) {
        h->v[h->n++] = ns;
    }
}

static int tu_cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

double tu_hist_pct(const tu_hist_t *h, double pct) {
    if (h->n == 0) {
        return 0.0;
    }
    double *tmp = malloc(h->n * sizeof(double));
    if (tmp == NULL) {
        return 0.0;
    }
    memcpy(tmp, h->v, h->n * sizeof(double));
    qsort(tmp, h->n, sizeof(double), tu_cmp_double);
    double idx = (pct / 100.0) * (double)(h->n - 1);
    if (idx < 0.0) {
        idx = 0.0;
    }
    if (idx > (double)(h->n - 1)) {
        idx = (double)(h->n - 1);
    }
    size_t lo = (size_t)idx;
    size_t hi = (lo + 1 < h->n) ? (lo + 1) : lo;
    double frac = idx - (double)lo;
    double val = tmp[lo] * (1.0 - frac) + tmp[hi] * frac;
    free(tmp);
    return val;
}
