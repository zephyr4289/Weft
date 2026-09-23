// weft_accel_common.c — RFC-0017 §2.2 implementation.
//
// Layer discipline: tools layer, no core/c edits; everything here is
// allocation-free on the hot paths (Law 1) and every probe/hook state is
// reported honestly (Law 4).

#include "weft/weft_accel_common.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

const char* weft_accel_path_name(weft_accel_path_t p) {
    switch (p) {
    case WEFT_ACCEL_ZERO_COPY:     return "ZERO-COPY";
    case WEFT_ACCEL_FALLBACK_COPY: return "FALLBACK-COPY";
    default:                       return "UNAVAILABLE";
    }
}

// ---------------------------------------------------------------------------
// Clock + stats
// ---------------------------------------------------------------------------

uint64_t weft_accel_ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void weft_accel_stats_reset(weft_accel_stats_t* s) {
    if (!s) return;
    s->n = 0;
    s->overflow = 0;
}

void weft_accel_stats_add(weft_accel_stats_t* s, uint64_t ns) {
    if (!s) return;
    if (s->n < WEFT_ACCEL_STAT_MAX) {
        s->samples[s->n++] = ns;
    } else {
        s->overflow++;  // counted, never silent — Law 4
    }
}

uint32_t weft_accel_stats_count(const weft_accel_stats_t* s) {
    return s ? s->n : 0u;
}

static int u64_cmp(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

uint64_t weft_accel_stats_pct(const weft_accel_stats_t* s,
                              weft_accel_stats_t* scratch, double pct) {
    if (!s || !scratch || s->n == 0) return 0;
    uint32_t n = s->n;
    memcpy(scratch->samples, s->samples, (size_t)n * sizeof(uint64_t));
    qsort(scratch->samples, n, sizeof(uint64_t), u64_cmp);
    if (pct <= 0.0)   return scratch->samples[0];
    if (pct >= 100.0) return scratch->samples[n - 1];
    double pos = (pct / 100.0) * (double)(n - 1);
    uint32_t lo = (uint32_t)pos;
    uint32_t hi = (lo + 1 < n) ? lo + 1 : lo;
    double frac = pos - (double)lo;
    double v = (double)scratch->samples[lo] * (1.0 - frac) +
               (double)scratch->samples[hi] * frac;
    return (uint64_t)(v + 0.5);
}

// ---------------------------------------------------------------------------
// Frozen-kernel identity (FNV-1a 64 — deterministic, dependency-free)
// ---------------------------------------------------------------------------

uint64_t weft_accel_frozen_id(const void* kernel_bytes, size_t n) {
    const uint8_t* p = (const uint8_t*)kernel_bytes;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Law 1 audit — malloc interposition (compiled only into audit binaries)
// ---------------------------------------------------------------------------
// The pooled-run gates (ONNX/ggml) must PROVE Law 1: zero heap allocations
// across a hot-path call. This module implements the classic dlsym(RTLD_NEXT)
// interposer; it is compiled ONLY when the binary is built with
// -DWEFT_ACCEL_ALLOC_AUDIT=1 (the test/bench targets that need the gate) —
// a library-wide silent interposition would be exactly the kind of surprise
// this project refuses. glibc-only; on other libcs the gate self-skips and
// SAYS so (weft_accel_alloc_audit_state), never pretends (Law 4).

#ifdef WEFT_ACCEL_ALLOC_AUDIT

#include <dlfcn.h>

static uint64_t g_audit_allocs = 0;
static int      g_audit_installed = 0;
static int      g_audit_bootstrapping = 0;

static void* (*real_malloc)(size_t);
static void  (*real_free)(void*);
static void* (*real_calloc)(size_t, size_t);
static void* (*real_realloc)(void*, size_t);

// dlsym's own bootstrap: the first calloc happens before the real symbols
// resolve; serve it from a tiny static arena (the classic recursive-calloc
// guard, 64 KiB is far past dlsym's needs).
static uint8_t  boot_buf[65536] __attribute__((aligned(16)));
static size_t   boot_used = 0;

static void* boot_alloc(size_t n) {
    n = (n + 15u) & ~(size_t)15u;
    if (boot_used + n > sizeof(boot_buf)) return NULL;
    void* p = &boot_buf[boot_used];
    boot_used += n;
    return p;
}

static void resolve_real(void) {
    if (real_malloc) return;
    g_audit_bootstrapping = 1;
    real_malloc  = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
    real_free    = (void (*)(void*))dlsym(RTLD_NEXT, "free");
    real_calloc  = (void* (*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    real_realloc = (void* (*)(void*, size_t))dlsym(RTLD_NEXT, "realloc");
    g_audit_bootstrapping = 0;
}

int weft_accel_alloc_audit_install(void) {
    resolve_real();
    if (!real_malloc) return -1;
    g_audit_allocs = 0;
    g_audit_installed = 1;
    return 0;
}

void weft_accel_alloc_audit_remove(void) { g_audit_installed = 0; }
uint64_t weft_accel_alloc_count(void)    { return g_audit_allocs; }
const char* weft_accel_alloc_audit_state(void) {
    return real_malloc ? "malloc(3) interposed (gate active)"
                       : "dlsym(RTLD_NEXT) failed (gate INACTIVE)";
}

void* malloc(size_t n) {
    if (!real_malloc) {
        if (g_audit_bootstrapping) return boot_alloc(n);
        resolve_real();
        if (!real_malloc) return boot_alloc(n);
    }
    void* p = real_malloc(n);
    if (p && g_audit_installed) g_audit_allocs++;
    return p;
}

void free(void* p) {
    if (p >= (void*)boot_buf && p < (void*)(boot_buf + sizeof(boot_buf))) {
        return;  // bootstrap arena — nothing to free
    }
    if (!real_free) { resolve_real(); }
    if (real_free) real_free(p);
}

void* calloc(size_t a, size_t b) {
    if (!real_calloc) {
        if (g_audit_bootstrapping) {
            void* bp = boot_alloc(a * b);
            if (bp) memset(bp, 0, a * b);
            return bp;
        }
        resolve_real();
        if (!real_calloc) {
            void* bp = boot_alloc(a * b);
            if (bp) memset(bp, 0, a * b);
            return bp;
        }
    }
    void* p = real_calloc(a, b);
    if (p && g_audit_installed) g_audit_allocs++;
    return p;
}

void* realloc(void* old, size_t n) {
    if (!real_realloc) { resolve_real(); }
    if (!real_realloc) return boot_alloc(n);
    void* p = real_realloc(old, n);
    if (p && g_audit_installed) g_audit_allocs++;
    return p;
}

#else  // !WEFT_ACCEL_ALLOC_AUDIT — compiled-out, honest about it

static uint64_t g_audit_allocs_stub = 0;

int  weft_accel_alloc_audit_install(void) { return -1; }
void weft_accel_alloc_audit_remove(void)  {}
uint64_t weft_accel_alloc_count(void)     { return g_audit_allocs_stub; }
const char* weft_accel_alloc_audit_state(void) {
    return "not compiled in (rebuild with -DWEFT_ACCEL_ALLOC_AUDIT=1; "
           "the Law-1 gate self-skips and says so)";
}

#endif // WEFT_ACCEL_ALLOC_AUDIT
