/*
 * test_alloc_intercept.h - Weft Pillar 6 Law 1 probe.
 *
 * Interposes malloc/calloc/realloc/free inside the test binary via
 * dlsym(RTLD_NEXT). Every allocation EVENT (not byte count) during a
 * decode window is counted; the harness resets counters around each
 * window and asserts ZERO events, proving the parser/transcoder paths
 * never touch the heap.
 *
 * Compile the including TU WITHOUT sanitizers and link -ldl.
 * A small bump arena serves pre-init allocations (dlsym bootstrap) so
 * interposition never re-enters itself.
 *
 * Only include from the test TU that defines main().
 */
#ifndef WEFT_TEST_ALLOC_INTERCEPT_H
#define WEFT_TEST_ALLOC_INTERCEPT_H

#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

static long g_probe_alloc_events = 0;
static long g_probe_free_events = 0;

static void *(*real_malloc_fn)(size_t);
static void *(*real_calloc_fn)(size_t, size_t);
static void *(*real_realloc_fn)(void *, size_t);
static void (*real_free_fn)(void *);

static char probe_arena[1 << 16];
static size_t probe_arena_off = 0;
static int probe_in_init = 0;

static char probe_stdout_buf[1 << 16];

static void *probe_arena_alloc(size_t n)
{
    void *p;
    n = (n + 15u) & ~(size_t)15u;
    if (probe_arena_off + n > sizeof probe_arena) { return NULL; }
    p = probe_arena + probe_arena_off;
    probe_arena_off += n;
    return p;
}

static int probe_ptr_is_arena(const void *p)
{
    return (const char *)p >= probe_arena &&
           (const char *)p < probe_arena + sizeof probe_arena;
}

static void probe_init(void)
{
    /* dlsym returns void*; convert via memcpy to stay ISO C clean
     * (object-pointer -> function-pointer casts are a pedantic
     * violation, the byte-copy is the portable idiom). */
    void *sym;
    probe_in_init = 1;
    sym = dlsym(RTLD_NEXT, "malloc");
    memcpy(&real_malloc_fn, &sym, sizeof sym);
    sym = dlsym(RTLD_NEXT, "calloc");
    memcpy(&real_calloc_fn, &sym, sizeof sym);
    sym = dlsym(RTLD_NEXT, "realloc");
    memcpy(&real_realloc_fn, &sym, sizeof sym);
    sym = dlsym(RTLD_NEXT, "free");
    memcpy(&real_free_fn, &sym, sizeof sym);
    probe_in_init = 0;
}

__attribute__((constructor))
static void probe_install(void)
{
    /* keep stdio off the heap so printf can't pollute the counters */
    setvbuf(stdout, probe_stdout_buf, _IOLBF, sizeof probe_stdout_buf);
    setvbuf(stderr, NULL, _IONBF, 0);
    probe_init();
}

void *malloc(size_t n)
{
    if (!real_malloc_fn) {
        if (probe_in_init) { return probe_arena_alloc(n); }
        probe_init();
    }
    ++g_probe_alloc_events;
    return real_malloc_fn(n);
}

void *calloc(size_t a, size_t b)
{
    if (!real_calloc_fn) {
        if (probe_in_init) {
            void *p = probe_arena_alloc(a * b);
            if (p) { memset(p, 0, a * b); }
            return p;
        }
        probe_init();
    }
    ++g_probe_alloc_events;
    return real_calloc_fn(a, b);
}

void *realloc(void *p, size_t n)
{
    if (!real_realloc_fn) { probe_init(); }
    if (probe_ptr_is_arena(p)) {
        void *q = malloc(n);
        if (q && p) {
            memcpy(q, p, n); /* arena blocks are small; copy is safe */
        }
        return q;
    }
    ++g_probe_alloc_events;
    return real_realloc_fn(p, n);
}

void free(void *p)
{
    if (probe_ptr_is_arena(p)) { return; }
    if (!real_free_fn) { probe_init(); }
    ++g_probe_free_events;
    real_free_fn(p);
}

static void probe_reset(void)
{
    g_probe_alloc_events = 0;
    g_probe_free_events = 0;
}

#endif /* WEFT_TEST_ALLOC_INTERCEPT_H */
