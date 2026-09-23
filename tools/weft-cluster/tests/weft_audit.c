// weft_audit.c — the Law-1 interposer implementation (see weft_audit.h).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "weft_audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>

// ---- the bootstrap arena ----------------------------------------------------

#define ARENA_BYTES (64 * 1024)
static char g_arena[ARENA_BYTES] __attribute__((aligned(16)));
static size_t g_arena_off = 0;

static int ptr_in_arena(const void* p) {
    return (const char*)p >= g_arena &&
           (const char*)p < g_arena + ARENA_BYTES;
}

// ---- the real allocators (resolved lazily; arena until then) -----------------

static void* (*real_malloc)(size_t);
static void* (*real_calloc)(size_t, size_t);
static void* (*real_realloc)(void*, size_t);
static void  (*real_free)(void*);

static void resolve_allocators(void) {
    // calloc'd arena? no: the arena is static. dlsym may call calloc
    // internally on first use — which lands in our calloc, which serves
    // from the arena, which is exactly what the arena is for.
    real_malloc = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
    real_calloc = (void* (*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    real_realloc = (void* (*)(void*, size_t))dlsym(RTLD_NEXT, "realloc");
    real_free = (void (*)(void*))dlsym(RTLD_NEXT, "free");
}

static int g_resolved = 0;
static int g_resolving = 0;

// ---- the counters ------------------------------------------------------------

static volatile int g_armed = 0;
static long g_count = 0;

void weft_audit_reset(void) { g_count = 0; }
void weft_audit_arm(int on) { g_armed = on; }
long weft_audit_count(void) { return g_count; }

void weft_audit_report(const char* tag) {
    fprintf(stderr, "[malloc-audit] %s: %ld allocs while armed\n",
            tag ? tag : "?", g_count);
}

// ---- the interposed symbols ---------------------------------------------------

void* malloc(size_t n) {
    if (!g_resolved && !g_resolving) {
        g_resolving = 1;
        resolve_allocators();
        g_resolved = 1;
        g_resolving = 0;
    }
    if (g_armed) g_count++;
    if (!real_malloc) {
        // arena road (during resolution)
        if (g_arena_off + n + 16 > ARENA_BYTES) return NULL;
        void* p = g_arena + g_arena_off;
        g_arena_off += (n + 15) & ~(size_t)15;
        return p;
    }
    return real_malloc(n);
}

void free(void* p) {
    if (!p || ptr_in_arena(p)) return;  // arena blocks never reach libc
    if (g_resolved && real_free) real_free(p);
}

void* calloc(size_t a, size_t b) {
    if (!g_resolved && !g_resolving) {
        g_resolving = 1;
        resolve_allocators();
        g_resolved = 1;
        g_resolving = 0;
    }
    if (g_armed) g_count++;
    if (!real_calloc) {
        const size_t n = a * b;
        if (g_arena_off + n + 16 > ARENA_BYTES) return NULL;
        void* p = g_arena + g_arena_off;
        g_arena_off += (n + 15) & ~(size_t)15;
        memset(p, 0, n);
        return p;
    }
    return real_calloc(a, b);
}

void* realloc(void* p, size_t n) {
    if (!g_resolved && !g_resolving) {
        g_resolving = 1;
        resolve_allocators();
        g_resolved = 1;
        g_resolving = 0;
    }
    if (g_armed) g_count++;
    if (ptr_in_arena(p)) {
        // arena block growth: allocate fresh (arena or real) and copy —
        // the bootstrap never reallocs anything it must preserve quietly
        void* q = malloc(n);
        if (q) memcpy(q, p, n);  // upper bound copy; arena use is tiny
        return q;
    }
    if (!real_realloc) return NULL;
    return real_realloc(p, n);
}
