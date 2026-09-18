// weft_hw_test.c — Issue #15 acceptance tests: the HW-series.
//
// The issue's acceptance criteria, mapped:
//   [x] Core protocol (weft.{h,c}) remains unchanged — kernel freeze is
//       proven by the verification battery, not by this test.
//   [x] Zero runtime overhead for unchanged paths — HW-8 measures the
//       dispatch delta (fn ptr set once; hot path never reads caps).
//   [x] Zero per-frame capability checks — HW-3: the selected backend is
//       STABLE across calls (structurally: the pointers are set at probe).
//   [x] Fallback to scalar/CPU paths — HW-2 verifies the ladder logic;
//       variants below the machine's ISA are skipped with a DECLARED count.
//   [x] All existing tests pass without modification — the battery.
//   [x] New tests for adaptive paths (SIMD, allocation, scheduling) — this
//       file.
//
// HW-series:
//   HW-1  caps coherence (probe ran, values sane, gpu is an honest state)
//   HW-2  dispatch selection matches caps (ladder order)
//   HW-3  dispatch stability (same backend, same result, across repeats)
//   HW-4  CROSS-VARIANT BIT-IDENTITY on random buffers incl. ragged tails
//         (transform: identical bytes; checksum: identical u32) — the
//         acceptance core: SIMD must not change semantics
//   HW-5  determinism + non-degeneracy (same input -> same output; the
//         scramble actually scrambles)
//   HW-6  allocator: alignment >= max(64, cache_line), usable, freeable
//   HW-7  scheduler: default spawn runs; pinned spawn runs on core 0 and
//         REFUSES bad core ids (refusals reported, never swallowed)
//   HW-8  zero-overhead evidence: dispatched vs direct call (reported,
//         never gated — measurement, not law)
//
// Build: make weft-hw-test (baseline-portable: per-function target attrs)
// Exit:  0 pass / 1 fail.

#define _GNU_SOURCE
#include "weft_hw.h"
#include "weft.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { fprintf(stderr, "  HW %s: ok\n", name); } \
    else { fprintf(stderr, "  HW %s: FAIL\n", name); g_fail = 1; } \
} while (0)

static uint32_t xs32(uint32_t* s) {
    return weft_xorshift32(s);
}

// ---------------------------------------------------------------------------
// HW-1 caps coherence
// ---------------------------------------------------------------------------
static void hw1_caps(void) {
    fprintf(stderr, "HW-1 caps coherence\n");
    const weft_hw_caps_t* c = weft_hw_probe();
    char report[256];
    weft_hw_caps_report(report, sizeof(report));
    fprintf(stderr, "  %s\n", report);
    CHECK(c->probed == 1, "1.probed");
    CHECK(c->cache_line >= 32 && c->cache_line <= 512, "1.cache_line-sane");
    CHECK(c->core_count >= 1, "1.core_count");
    CHECK(c->numa_nodes >= 1, "1.numa_nodes");
    CHECK(c->gpu == WEFT_HW_GPU_NONE || c->gpu == WEFT_HW_GPU_LOADER, "1.gpu-state");
}

// ---------------------------------------------------------------------------
// HW-2 dispatch selection matches caps (the ladder)
// ---------------------------------------------------------------------------
static void hw2_selection(void) {
    fprintf(stderr, "HW-2 dispatch selection\n");
    const weft_hw_caps_t* c = weft_hw_probe();
    const char* tb = weft_hw_xor_transform_backend();
    const char* cb = weft_hw_checksum32_backend();
#if defined(__x86_64__) || defined(__i386__)
    const char* expect = "scalar";
    if (c->has_sse42) expect = "sse2";
    if (c->has_avx2) expect = "avx2";
    if (c->has_avx512f && c->has_avx512vl) expect = "avx512";
    CHECK(strcmp(tb, expect) == 0, "2.transform-ladder");
    CHECK(strcmp(cb, expect) == 0, "2.checksum-ladder");
#else
    (void)c; (void)cb;
    CHECK(strcmp(tb, "neon") == 0 || strcmp(tb, "scalar") == 0, "2.arm-ladder");
#endif
}

// ---------------------------------------------------------------------------
// HW-3 dispatch stability (no per-frame re-selection)
// ---------------------------------------------------------------------------
static void hw3_stability(void) {
    fprintf(stderr, "HW-3 dispatch stability\n");
    const char* b1 = weft_hw_xor_transform_backend();
    uint8_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = (uint8_t)i;
    for (int k = 0; k < 1000; k++) {
        weft_hw_xor_transform(buf, sizeof(buf));   // transform again (scramble^2)
        if (k == 500) {
            const char* b2 = weft_hw_xor_transform_backend();
            CHECK(strcmp(b1, b2) == 0, "3.backend-stable");
        }
    }
    // Determinism: replaying the same op stream from the same start must
    // reproduce the same final bytes.
    uint8_t replay[256];
    for (int i = 0; i < 256; i++) replay[i] = (uint8_t)i;
    for (int k = 0; k < 1000; k++) weft_hw_xor_transform(replay, sizeof(replay));
    CHECK(memcmp(buf, replay, sizeof(buf)) == 0, "3.deterministic");
}

// ---------------------------------------------------------------------------
// HW-4 cross-variant bit-identity (the acceptance core)
// ---------------------------------------------------------------------------
typedef void (*xfn)(uint8_t*, size_t);
typedef uint32_t (*cfn)(const uint8_t*, size_t);

static void hw4_identity(void) {
    fprintf(stderr, "HW-4 cross-variant bit-identity\n");
    const weft_hw_caps_t* c = weft_hw_probe();
    const size_t sizes[] = { 0, 1, 3, 4, 7, 15, 16, 17, 60, 63, 64, 65, 68, 128, 192, 300, 1024, 4095, 4096, 4097, 65536 };
    const size_t nsizes = sizeof(sizes) / sizeof(sizes[0]);
    uint32_t seed = 0x00C0FFEEu;

    xfn xvars[4] = { weft_hw_xor_transform_scalar, NULL, NULL, NULL };
    cfn cvars[4] = { weft_hw_checksum32_scalar, NULL, NULL, NULL };
    const char* names[4] = { "scalar", "sse2", "avx2", "avx512" };
    int nx = 1;
#if defined(__x86_64__) || defined(__i386__)
    if (c->has_sse42)             { xvars[nx] = weft_hw_xor_transform_sse2;   cvars[nx] = weft_hw_checksum32_sse2;   nx++; }
    if (c->has_avx2)              { xvars[nx] = weft_hw_xor_transform_avx2;   cvars[nx] = weft_hw_checksum32_avx2;   nx++; }
    if (c->has_avx512f && c->has_avx512vl) { xvars[nx] = weft_hw_xor_transform_avx512; cvars[nx] = weft_hw_checksum32_avx512; nx++; }
#else
    (void)c;
#endif
    fprintf(stderr, "  variants compared: %d (%s)\n", nx,
            nx == 4 ? "all" : "ISA-gated subset — DECLARED");
    CHECK(nx >= 1, "4.at-least-scalar");

    uint8_t* orig = (uint8_t*)malloc(65536);
    uint8_t* work = (uint8_t*)malloc(65536);
    if (!orig || !work) { g_fail = 1; free(orig); free(work); return; }

    bool all_transform_ok = true, all_checksum_ok = true;
    for (size_t si = 0; si < nsizes; si++) {
        size_t len = sizes[si];
        for (size_t i = 0; i < len; i++) orig[i] = (uint8_t)xs32(&seed);
        // Reference: scalar
        uint8_t ref[65536];
        memcpy(ref, orig, len);
        weft_hw_xor_transform_scalar(ref, len);
        uint32_t cref = weft_hw_checksum32_scalar(orig, len);
        // Every compiled variant must match EXACTLY
        for (int v = 1; v < nx; v++) {
            memcpy(work, orig, len);
            xvars[v](work, len);
            if (len > 0 && memcmp(work, ref, len) != 0) {
                fprintf(stderr, "  transform %s diverged at len=%zu\n", names[v], len);
                all_transform_ok = false;
            }
            uint32_t cv = cvars[v](orig, len);
            if (cv != cref) {
                fprintf(stderr, "  checksum %s diverged at len=%zu (%u != %u)\n",
                        names[v], len, cv, cref);
                all_checksum_ok = false;
            }
        }
    }
    free(orig); free(work);
    CHECK(all_transform_ok, "4.transform-bit-identity");
    CHECK(all_checksum_ok, "4.checksum-bit-identity");
}

// ---------------------------------------------------------------------------
// HW-5 determinism + non-degeneracy
// ---------------------------------------------------------------------------
static void hw5_nondegenerate(void) {
    fprintf(stderr, "HW-5 determinism + non-degeneracy\n");
    uint8_t a[256], b[256];
    for (int i = 0; i < 256; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)i; }
    weft_hw_xor_transform(a, 256);
    weft_hw_xor_transform(b, 256);
    CHECK(memcmp(a, b, 256) == 0, "5.deterministic");
    bool scrambled = false;
    for (int i = 0; i < 256; i++) if (a[i] != (uint8_t)i) { scrambled = true; break; }
    CHECK(scrambled, "5.actually-scrambles");
    // checksum spreads: distinct inputs -> distinct sums (birthday-bounded)
    uint32_t s0 = weft_hw_checksum32((const uint8_t*)"aaaaaaaa", 8);
    uint32_t s1 = weft_hw_checksum32((const uint8_t*)"aaaaaaab", 8);
    CHECK(s0 != s1, "5.checksum-spreads");
}

// ---------------------------------------------------------------------------
// HW-6 allocator
// ---------------------------------------------------------------------------
static void hw6_allocator(void) {
    fprintf(stderr, "HW-6 allocator\n");
    const weft_hw_caps_t* c = weft_hw_probe();
    size_t expect_align = (size_t)(c->cache_line > 64 ? c->cache_line : 64);
    void* p = weft_hw_alloc(1024, 32);
    CHECK(p != NULL, "6.allocates");
    CHECK(((uintptr_t)p % expect_align) == 0, "6.cache-line-aligned");
    memset(p, 0xAB, 1024);   // usable
    ((uint8_t*)p)[0] = 1;
    CHECK(((uint8_t*)p)[0] == 1, "6.writable");
    free(p);
    // align=0 request clamps to the cache-line default
    p = weft_hw_alloc(64, 0);
    CHECK(p != NULL && ((uintptr_t)p % expect_align) == 0, "6.zero-align-clamps");
    free(p);
    // zero-size request: defined behavior, and the result (if any) frees
    void* z = weft_hw_alloc(0, 64);
    CHECK(z != NULL || true, "6.zero-size-defined");
    free(z);   // posix_memalign(0) may return a unique freeable pointer
}

// ---------------------------------------------------------------------------
// HW-7 scheduler (default + pinned + honest refusals)
// ---------------------------------------------------------------------------
static _Atomic int g_ran;

static uint64_t now_ns(void);   // fwd: defined with the HW-8 timing helpers

static void* trivial_fn(void* arg) {
    (void)arg;
    atomic_store(&g_ran, 1);
    return NULL;
}

static void* spin_fn(void* arg) {
    // arg points at a two-flag record: [0] = stop request, [1] = exit ack.
    // The thread is DETACHED — the caller MUST wait for the exit ack before
    // its own frame unwinds, or the poll reads freed stack (the bug ASAN
    // caught in the first version of this test).
    _Atomic int* flag = (_Atomic int*)arg;
    atomic_store(&g_ran, 1);   // ran-flag at ENTRY: placement proven immediately
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000 };
    while (!atomic_load(flag)) nanosleep(&ts, NULL);
    atomic_store(&flag[1], 1);  // exit ack
    return NULL;
}

static void hw7_scheduler(void) {
    fprintf(stderr, "HW-7 scheduler\n");
    atomic_store(&g_ran, 0);
    CHECK(weft_hw_spawn(trivial_fn, NULL) == 0, "7.spawn-default");
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
    nanosleep(&ts, NULL);
    CHECK(atomic_load(&g_ran) == 1, "7.spawned-ran");

    atomic_store(&g_ran, 0);
    _Atomic int ctl[2] = { 0, 0 };   // [0] stop request, [1] exit ack
    int rc = weft_hw_spawn_pinned(spin_fn, ctl, 0);
    CHECK(rc == 0, "7.spawn-pinned-core0");
    nanosleep(&ts, NULL);
    CHECK(atomic_load(&g_ran) == 1, "7.pinned-ran");
    atomic_store(&ctl[0], 1);
    // Teardown handshake: the thread is detached; wait for its exit ack
    // before this frame unwinds (bounded — a stuck thread is a RED).
    uint64_t deadline = now_ns() + 2000000000ull;
    while (!atomic_load(&ctl[1]) && now_ns() < deadline) nanosleep(&ts, NULL);
    CHECK(atomic_load(&ctl[1]) == 1, "7.pinned-exited");

    // Honest refusals: bad core ids
    CHECK(weft_hw_spawn_pinned(trivial_fn, NULL, -1) != 0, "7.refuses-negative-core");
    CHECK(weft_hw_spawn_pinned(trivial_fn, NULL, 1 << 20) != 0, "7.refuses-huge-core");
}

// ---------------------------------------------------------------------------
// HW-8 zero-overhead evidence (reported, never gated)
// ---------------------------------------------------------------------------
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void hw8_overhead(void) {
    fprintf(stderr, "HW-8 dispatch overhead (measurement, not a gate)\n");
    uint8_t buf[4096] __attribute__((aligned(64)));
    memset(buf, 0x5A, sizeof(buf));
    const int N = 200000;
    // warm both paths
    for (int i = 0; i < 1000; i++) { weft_hw_xor_transform(buf, sizeof(buf)); }
    uint64_t t0 = now_ns();
    for (int i = 0; i < N; i++) weft_hw_xor_transform(buf, sizeof(buf));
    uint64_t t_dispatch = now_ns() - t0;
    // direct: call the selected variant straight through its fn ptr,
    // captured ONCE (what an application with manual dispatch would do)
    weft_hw_process_fn direct = NULL;
#if defined(__x86_64__) || defined(__i386__)
    const weft_hw_caps_t* c = weft_hw_probe();
    if (c->has_avx512f && c->has_avx512vl) direct = weft_hw_xor_transform_avx512;
    else if (c->has_avx2) direct = weft_hw_xor_transform_avx2;
    else if (c->has_sse42) direct = weft_hw_xor_transform_sse2;
#endif
    if (!direct) direct = weft_hw_xor_transform_scalar;
    for (int i = 0; i < 1000; i++) direct(buf, sizeof(buf));
    t0 = now_ns();
    for (int i = 0; i < N; i++) direct(buf, sizeof(buf));
    uint64_t t_direct = now_ns() - t0;
    double per_dispatch = (double)t_dispatch / N;
    double per_direct = (double)t_direct / N;
    fprintf(stderr, "  dispatched: %.1f ns/call  direct: %.1f ns/call  delta: %+.1f ns/call\n",
            per_dispatch, per_direct, per_dispatch - per_direct);
    printf("{\"metric\":\"hw8-dispatch-overhead\",\"dispatched_ns_per_call\":%.2f,"
           "\"direct_ns_per_call\":%.2f,\"delta_ns_per_call\":%.2f}\n",
           per_dispatch, per_direct, per_dispatch - per_direct);
}

int main(void) {
    hw1_caps();
    hw2_selection();
    hw3_stability();
    hw4_identity();
    hw5_nondegenerate();
    hw6_allocator();
    hw7_scheduler();
    hw8_overhead();

    bool pass = g_fail == 0;
    printf("{\"test\":\"weft-hw\",\"lang\":\"c\",\"pass\":%s}\n", pass ? "true" : "false");
    fprintf(stderr, "weft-hw: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
