// test_util.c — implementation of the shared test belt.

#include "test_util.h"

#include <dirent.h>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

unsigned long tu_alloc_count = 0;

void tu_child_setup(void) {
#ifdef PR_SET_PDEATHSIG
    if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0) {
        _exit(99);
    }
#endif
    /* the signal only fires on FUTURE parent death; a parent that died
     * between fork() and prctl() is caught by the getppid() check */
    if (getppid() == 1) {
        _exit(99);
    }
}

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
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

void tu_usleep(unsigned long us) {
    struct timespec ts = {.tv_sec = (time_t)(us / 1000000ul),
                          .tv_nsec = (long)(us % 1000000ul) * 1000};
    (void)nanosleep(&ts, NULL);
}

unsigned tu_fd_count(void) {
    DIR *d = opendir("/proc/self/fd");
    if (d == NULL) return 0;
    unsigned n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9') n++;
    }
    /* the opendir() itself holds one of those fds */
    (void)closedir(d);
    return n > 0 ? n - 1 : 0;
}

unsigned tu_shm_weft_count(const char *prefix) {
    DIR *d = opendir("/dev/shm");
    if (d == NULL) return 0;
    unsigned n = 0;
    size_t plen = strlen(prefix);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, plen) == 0) n++;
    }
    (void)closedir(d);
    return n;
}

long tu_rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (f == NULL) return -1;
    unsigned long total = 0, resident = 0;
    if (fscanf(f, "%lu %lu", &total, &resident) != 2) resident = 0;
    (void)fclose(f);
    return (long)(resident * (unsigned long)sysconf(_SC_PAGESIZE) / 1024ul);
}

static int tu_cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int tu_hist_init(tu_hist_t *h, size_t cap) {
    h->v = calloc(cap, sizeof(double));
    if (h->v == NULL) return -1;
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
    if (h->n < h->cap) h->v[h->n++] = ns;
}

double tu_hist_pct(const tu_hist_t *h, double pct) {
    if (h->n == 0) return 0.0;
    double *tmp = malloc(h->n * sizeof(double));
    if (tmp == NULL) return 0.0;
    memcpy(tmp, h->v, h->n * sizeof(double));
    qsort(tmp, h->n, sizeof(double), tu_cmp_double);
    double idx = (pct / 100.0) * (double)(h->n - 1);
    size_t lo = (size_t)idx;
    size_t hi = lo + 1 < h->n ? lo + 1 : lo;
    double frac = idx - (double)lo;
    double val = tmp[lo] * (1.0 - frac) + tmp[hi] * frac;
    free(tmp);
    return val;
}
