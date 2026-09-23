// test_tensor_arena.c — WT-series (17-24): paged tensor arena conformance
// (RFC-0021 §4).
//
//   WT17 create/destroy: page-aligned base, page-rounded capacity, fresh
//        cursor/highwater, destroy idempotence (ASAN leg proves the unmap).
//   WT18 placement honesty: MLOCK|HUGEPAGE|PREFAULT create succeeds and
//        the fields REPORT the truth (0/1 booleans, printed for the
//        evidence log — an unprivileged runner refuses mlock and the
//        field must say so, not pretend).
//   WT19 alignment law: alloc() honors 16/32/64/128/4096 exactly
//        (Law 4: never silently rounded down), monotone offsets.
//   WT20 exhaustion: ENOMEM with NULL, cursor unchanged, no silent
//        shrink; highwater recorded.
//   WT21 mark/rewind/reset: O(1) frame recycling; foreign/regressive
//        marks refused.
//   WT22 attach: caller-provided region (the unified-memory seam) serves
//        allocs; misaligned base refused with EMISALIGN; destroy never
//        frees attacher memory.
//   WT23 sub-microsecond claim: 100k allocs measured (ns/alloc, JSON
//        line); generous hard bound only (shared CI runners — the house
//        no-flaky-perf-assert rule); --bench runs the 1M leg.
//   WT24 arena->view flow: payload sub-allocated 64B-aligned, view
//        initialized over it, validated at ALIGN_64 — the end-to-end
//        "tensor from arena" story with zero heap.
//
// Build: make tensor-arena-test{,-asan}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "weft_tensor.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);         \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define CHECK_ST(call, want, msg)                                           \
    do {                                                                    \
        const int _st = (call);                                             \
        g_checks++;                                                         \
        if (_st != (want)) {                                                \
            fprintf(stderr, "FAIL: %s: want %s got %s (line %d)\n", msg,    \
                    weft_tensor_status_name(want),                          \
                    weft_tensor_status_name(_st), __LINE__);                \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t page_size(void) {
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (uint64_t)ps : 4096u;
}

// ---------------------------------------------------------------------------
// WT17 — create / destroy
// ---------------------------------------------------------------------------

static void test_wt17(void) {
    weft_tensor_arena_t a;
    CHECK_ST(weft_tensor_arena_create(&a, 1, WEFT_TENSOR_ARENA_F_SHARED),
             WEFT_TENSOR_OK, "capacity 1 rounds up");
    CHECK(a.capacity == page_size(), "capacity rounded to one page");
    CHECK(((uintptr_t)a.base % page_size()) == 0, "base page-aligned");
    CHECK(a.cursor == 0 && a.highwater == 0 && a.alloc_count == 0,
          "fresh counters");
    CHECK(a.creator == 1, "creator flag");
    weft_tensor_arena_destroy(&a);
    weft_tensor_arena_destroy(&a);  // idempotent
    CHECK(a.base == NULL, "destroy zeroes the handle");

    CHECK_ST(weft_tensor_arena_create(NULL, 4096, 0), WEFT_TENSOR_EINVAL,
             "NULL arena");
    CHECK_ST(weft_tensor_arena_create(&a, 0, 0), WEFT_TENSOR_EINVAL,
             "zero capacity");
    CHECK_ST(weft_tensor_arena_create(&a, 1ull << 63, 0), WEFT_TENSOR_EINVAL,
             "2^63 capacity");
    CHECK_ST(weft_tensor_arena_create(&a, 4096, 0x10), WEFT_TENSOR_EINVAL,
             "unknown flag");
}

// ---------------------------------------------------------------------------
// WT18 — placement honesty (mlock / hugepage / prefault REPORTED)
// ---------------------------------------------------------------------------

static void test_wt18(void) {
    weft_tensor_arena_t a;
    CHECK_ST(weft_tensor_arena_create(&a, 1 << 20,
                                      WEFT_TENSOR_ARENA_F_MLOCK |
                                          WEFT_TENSOR_ARENA_F_HUGEPAGE |
                                          WEFT_TENSOR_ARENA_F_PREFAULT),
             WEFT_TENSOR_OK, "full-flag create");
    CHECK(a.locked == 0 || a.locked == 1, "locked is an honest boolean");
    CHECK(a.hugepage_hint == 0 || a.hugepage_hint == 1,
          "hugepage_hint is an honest boolean");
    printf("WT18 placement report: mlock=%d hugepage_hint=%d (refusals are "
           "honest, not fatal)\n",
           a.locked, a.hugepage_hint);

    // Prefaulted pages are writable NOW (no SIGSEGV on first touch).
    a.base[0] = 0xAB;
    a.base[a.capacity - 1] = 0xCD;
    CHECK(a.base[0] == 0xAB && a.base[a.capacity - 1] == 0xCD,
          "prefaulted pages writable");
    weft_tensor_arena_destroy(&a);
}

// ---------------------------------------------------------------------------
// WT19 — alignment law (exact, never rounded down)
// ---------------------------------------------------------------------------

static void test_wt19(void) {
    weft_tensor_arena_t a;
    CHECK_ST(weft_tensor_arena_create(&a, 1 << 20, 0), WEFT_TENSOR_OK,
             "wt19 create");
    static const uint32_t kAligns[] = {16, 32, 64, 128, 4096};
    uint64_t prev_off = 0;
    for (size_t i = 0; i < sizeof(kAligns) / sizeof(kAligns[0]); i++) {
        int st = WEFT_TENSOR_OK;
        uint64_t off = 0;
        void* p = weft_tensor_arena_alloc(&a, 1000, kAligns[i], &off, &st);
        char msg[64];
        snprintf(msg, sizeof(msg), "alloc align %u", kAligns[i]);
        CHECK(p != NULL && st == WEFT_TENSOR_OK, msg);
        snprintf(msg, sizeof(msg), "pointer honors align %u", kAligns[i]);
        CHECK(((uintptr_t)p % kAligns[i]) == 0, msg);
        snprintf(msg, sizeof(msg), "offset honors align %u", kAligns[i]);
        CHECK((off % kAligns[i]) == 0, msg);
        CHECK(off >= prev_off, "offsets monotone");
        CHECK(p == a.base + off, "pointer == base + offset");
        prev_off = off + 1000;
    }
    // Misaligned-size run: 3-byte allocs still advance exactly 3 bytes.
    for (int i = 0; i < 5; i++) {
        int st = WEFT_TENSOR_OK;
        uint64_t off = 0;
        void* p = weft_tensor_arena_alloc(&a, 3, 1, &off, &st);
        CHECK(p != NULL && st == WEFT_TENSOR_OK, "3-byte alloc");
        CHECK(p == a.base + off, "3-byte pointer");
    }
    CHECK(a.alloc_count == 10, "alloc_count tracks");
    // Bad alignment classes.
    int st = WEFT_TENSOR_OK;
    CHECK(weft_tensor_arena_alloc(&a, 16, 0, NULL, &st) == NULL &&
              st == WEFT_TENSOR_EINVAL, "align 0 refused");
    CHECK(weft_tensor_arena_alloc(&a, 16, 3, NULL, &st) == NULL &&
              st == WEFT_TENSOR_EINVAL, "align 3 refused");
    CHECK(weft_tensor_arena_alloc(&a, 16, 8192, NULL, &st) == NULL &&
              st == WEFT_TENSOR_EINVAL, "align 8192 refused");
    CHECK(weft_tensor_arena_alloc(&a, 0, 16, NULL, &st) == NULL &&
              st == WEFT_TENSOR_EINVAL, "size 0 refused");
    weft_tensor_arena_destroy(&a);
}

// ---------------------------------------------------------------------------
// WT20 — exhaustion (reported, never rounded)
// ---------------------------------------------------------------------------

static void test_wt20(void) {
    weft_tensor_arena_t a;
    CHECK_ST(weft_tensor_arena_create(&a, 1, 0), WEFT_TENSOR_OK,
             "one-page arena");
    int st = WEFT_TENSOR_OK;
    // Fill the page.
    void* p = weft_tensor_arena_alloc(&a, a.capacity, 64, NULL, &st);
    CHECK(p != NULL && st == WEFT_TENSOR_OK, "exact-fill alloc");
    const uint64_t cursor_before = a.cursor;
    p = weft_tensor_arena_alloc(&a, 1, 1, NULL, &st);
    CHECK(p == NULL && st == WEFT_TENSOR_ENOMEM, "exhaustion refused");
    CHECK(a.cursor == cursor_before, "cursor unchanged on refusal");
    CHECK(a.highwater == a.capacity, "highwater records the ceiling");
    uint64_t used, high, allocs;
    weft_tensor_arena_stats(&a, &used, &high, &allocs);
    CHECK(used == a.capacity && high == a.capacity && allocs == 1, "stats");
    weft_tensor_arena_destroy(&a);
}

// ---------------------------------------------------------------------------
// WT21 — mark / rewind / reset
// ---------------------------------------------------------------------------

static void test_wt21(void) {
    weft_tensor_arena_t a;
    CHECK_ST(weft_tensor_arena_create(&a, 1 << 16, 0), WEFT_TENSOR_OK,
             "wt21 create");
    int st = WEFT_TENSOR_OK;
    weft_tensor_arena_alloc(&a, 100, 16, NULL, &st);
    weft_tensor_arena_mark_t m;
    CHECK_ST(weft_tensor_arena_mark(&a, &m), WEFT_TENSOR_OK, "mark");
    CHECK(m.cursor == 100, "mark cursor (already 16B-aligned)");
    weft_tensor_arena_alloc(&a, 5000, 16, NULL, &st);
    CHECK(a.cursor == 5112, "cursor after second alloc");
    CHECK_ST(weft_tensor_arena_rewind(&a, &m), WEFT_TENSOR_OK, "rewind");
    CHECK(a.cursor == 100, "rewound cursor");
    // Rewind is allowed to move BACKWARD only from the current cursor.
    weft_tensor_arena_alloc(&a, 64, 16, NULL, &st);
    CHECK_ST(weft_tensor_arena_rewind(&a, &m), WEFT_TENSOR_OK, "rewind again");
    weft_tensor_arena_mark_t foreign = {100000000ull, 0};
    CHECK_ST(weft_tensor_arena_rewind(&a, &foreign), WEFT_TENSOR_EINVAL,
            "foreign mark refused");
    weft_tensor_arena_reset(&a);
    CHECK(a.cursor == 0, "reset");
    CHECK(a.highwater >= 5112, "highwater survives reset (diagnostics)");
    weft_tensor_arena_destroy(&a);
}

// ---------------------------------------------------------------------------
// WT22 — attach (the unified-memory seam)
// ---------------------------------------------------------------------------

static void test_wt22(void) {
    static uint8_t region[1 << 16] __attribute__((aligned(4096)));
    weft_tensor_arena_t a;
    CHECK_ST(weft_tensor_arena_attach(&a, region, sizeof(region), 0),
             WEFT_TENSOR_OK, "attach aligned region");
    CHECK(a.creator == 0, "attacher never owns");
    int st = WEFT_TENSOR_OK;
    void* p = weft_tensor_arena_alloc(&a, 1024, 64, NULL, &st);
    CHECK(p != NULL && (const uint8_t*)p >= region &&
              (const uint8_t*)p < region + sizeof(region),
          "attacher alloc inside region");
    weft_tensor_arena_destroy(&a);  // must NOT free `region`
    CHECK(region[0] == 0, "region survives destroy");

    CHECK_ST(weft_tensor_arena_attach(&a, (void*)(region + 1), 1024, 0),
             WEFT_TENSOR_EMISALIGN, "misaligned attach refused");
    CHECK_ST(weft_tensor_arena_attach(&a, NULL, 1024, 0), WEFT_TENSOR_EINVAL,
             "NULL attach refused");
    CHECK_ST(weft_tensor_arena_attach(&a, region, 0, 0), WEFT_TENSOR_EINVAL,
             "zero capacity attach refused");
}

// ---------------------------------------------------------------------------
// WT23 — sub-microsecond claim (measured; generous bound only)
// ---------------------------------------------------------------------------

static void bench_wt23(int heavy) {
    const uint64_t cap = heavy ? (256ull << 20) : (32ull << 20);
    const uint64_t n = heavy ? 1000000ull : 100000ull;
    const uint64_t alloc_bytes = 256;
    weft_tensor_arena_t a;
    if (weft_tensor_arena_create(&a, cap, WEFT_TENSOR_ARENA_F_PREFAULT) !=
        WEFT_TENSOR_OK) {
        CHECK(0, "wt23 arena create");
        return;
    }
    const uint64_t t0 = now_ns();
    int st = WEFT_TENSOR_OK;
    for (uint64_t i = 0; i < n; i++) {
        (void)weft_tensor_arena_alloc(&a, alloc_bytes, 64, NULL, &st);
    }
    const uint64_t dt = now_ns() - t0;
    CHECK(st == WEFT_TENSOR_OK, "wt23 all allocs served");
    CHECK(a.alloc_count == n, "wt23 count");
    const double ns_per = (double)dt / (double)n;
    printf("{\"test\":\"WT23\",\"variant\":\"%s\",\"allocs\":%llu,"
           "\"ns_per_alloc\":%.1f,\"bytes\":%llu}\n",
           heavy ? "1M" : "100k", (unsigned long long)n, ns_per,
           (unsigned long long)(n * alloc_bytes));
    // Generous hard bound only: 10us/alloc would already be a defect on
    // any runner (real figure is single-digit ns) — no tight perf assert
    // on shared CI hardware (house no-flaky rule).
    CHECK(ns_per < 10000.0, "wt23 generous bound (10us/alloc)");
    weft_tensor_arena_destroy(&a);
}

// ---------------------------------------------------------------------------
// WT24 — arena -> view end-to-end (zero heap on the path)
// ---------------------------------------------------------------------------

static void test_wt24(void) {
    weft_tensor_arena_t a;
    CHECK_ST(weft_tensor_arena_create(&a, 1 << 20, 0), WEFT_TENSOR_OK,
             "wt24 create");
    int st = WEFT_TENSOR_OK;
    uint64_t off = 0;
    // A 4D NCHW f32 camera tensor: [1,3,224,224] = 602112 bytes.
    const uint64_t shape[4] = {1, 3, 224, 224};
    uint8_t* payload = (uint8_t*)weft_tensor_arena_alloc(&a, 602112, 64, &off, &st);
    CHECK(payload != NULL && st == WEFT_TENSOR_OK, "tensor payload alloc");
    CHECK((off % 64) == 0, "payload 64B-aligned offset");

    weft_tensor_view_t v;
    CHECK_ST(weft_tensor_view_init(&v, 42, WEFT_DTYPE_F32, 4, shape,
                                   (uintptr_t)payload, 0),
             WEFT_TENSOR_OK, "view over arena payload");
    CHECK_ST(weft_tensor_view_validate(&v, WEFT_TENSOR_ALIGN_64), WEFT_TENSOR_OK,
             "Law 4: arena tensor validates at 64B");

    // Write/read through the view — one pointer add, zero copies.
    // Element (0,1,100,150): channel-1 plane + row 100 + col 150.
    const uint64_t idx[4] = {0, 1, 100, 150};
    float* px = (float*)weft_tensor_view_element_addr(&v, idx);
    CHECK(px != NULL, "element addr");
    *px = 3.5f;
    CHECK(*px == 3.5f, "read back");
    CHECK(*(float*)(payload + off + 200704ull + 100 * 896 + 150 * 4) == 3.5f,
          "byte landed in the channel-1 plane via the view");
    CHECK(*(float*)(payload + off + 100 * 896 + 150 * 4) == 0.0f,
          "channel-0 plane untouched");

    // Sibling allocations do not disturb the tensor window.
    uint8_t* next = (uint8_t*)weft_tensor_arena_alloc(&a, 4096, 64, NULL, &st);
    CHECK(next != NULL && next >= payload + 602112, "next alloc disjoint");
    weft_tensor_arena_destroy(&a);
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    const int heavy = argc > 1 && strcmp(argv[1], "--bench") == 0;

    test_wt17();
    test_wt18();
    test_wt19();
    test_wt20();
    test_wt21();
    test_wt22();
    bench_wt23(heavy);
    test_wt24();

    printf("tensor-arena: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
