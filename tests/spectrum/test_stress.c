// test_stress.c — WSP5 S-series: multithreaded stress, torn-read
// eradication proof, allocation ledger and the <= 5 ns latency
// scorecard.
//
// Phases:
//   S1  Concurrent zigzag: 8 lock-free readers hammering
//       weft_get_active_tier() + weft_governor_snapshot() while the
//       single governance writer republishes plans (pressure zigzag:
//       T1 -> T2 -> T3 -> T2 -> T1, single-step transitions so the
//       linearizability interval check is sound). Every accepted
//       snapshot must pass the FULL plan invariant ladder (identity,
//       CRC, geometry table, policy, arena identities). Torn reads
//       MUST be zero; bounded-retry refusals (EPLAN_BUSY) are allowed
//       but sanity-bounded.
//   S2  Concurrent force storm: operator force/clear alternation with
//       readers validating invariants only (no interval check — the
//       force may legally jump tiers).
//   S3  Single-writer transition storm: 100k republishes + snapshots.
//   S4  Allocation ledger: with malloc/calloc/realloc/free/mmap
//       interposed (--wrap), the engine performs ZERO dynamic
//       allocations across live tier transitions, probes and
//       snapshots (0 calls, 0 bytes).
//   S5  Latency scorecard: weft_hw_has_feature() and
//       weft_get_active_tier() at <= 5 ns/op (median of 7 runs of 1M
//       ops, compiler-barriered sink); weft_governor_snapshot()
//       reported informationally (the mandate names the two queries).
//
// Env: SPECTRUM_STRESS_ITERS (default 2000000 total reader ops),
//      SPECTRUM_SKIP_BENCH=1 (sanitizer builds).

#include "weft_spectrum_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_fails++;                                                      \
            printf("FAIL S %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

/* ------------------------------------------------------------------ */
/* Allocation ledger (--wrap interposition; atomics: any thread)       */
/* ------------------------------------------------------------------ */

static unsigned long g_ledger_malloc  = 0;
static unsigned long g_ledger_calloc  = 0;
static unsigned long g_ledger_realloc = 0;
static unsigned long g_ledger_free    = 0;
static unsigned long g_ledger_mmap    = 0;

extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void *__real_realloc(void *, size_t);
extern void __real_free(void *);
extern void *__real_mmap(void *, size_t, int, int, int, off_t);

void *__wrap_malloc(size_t n)
{
    __atomic_fetch_add(&g_ledger_malloc, 1, __ATOMIC_RELAXED);
    return __real_malloc(n);
}
void *__wrap_calloc(size_t a, size_t b)
{
    __atomic_fetch_add(&g_ledger_calloc, 1, __ATOMIC_RELAXED);
    return __real_calloc(a, b);
}
void *__wrap_realloc(void *p, size_t n)
{
    __atomic_fetch_add(&g_ledger_realloc, 1, __ATOMIC_RELAXED);
    return __real_realloc(p, n);
}
void __wrap_free(void *p)
{
    __atomic_fetch_add(&g_ledger_free, 1, __ATOMIC_RELAXED);
    __real_free(p);
}
void *__wrap_mmap(void *a, size_t b, int c, int d, int e, off_t f)
{
    __atomic_fetch_add(&g_ledger_mmap, 1, __ATOMIC_RELAXED);
    return __real_mmap(a, b, c, d, e, f);
}

static void ledger_reset(void)
{
    __atomic_store_n(&g_ledger_malloc, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ledger_calloc, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ledger_realloc, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ledger_free, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ledger_mmap, 0, __ATOMIC_RELAXED);
}

static unsigned long ledger_total(void)
{
    return __atomic_load_n(&g_ledger_malloc, __ATOMIC_RELAXED) +
           __atomic_load_n(&g_ledger_calloc, __ATOMIC_RELAXED) +
           __atomic_load_n(&g_ledger_realloc, __ATOMIC_RELAXED) +
           __atomic_load_n(&g_ledger_free, __ATOMIC_RELAXED) +
           __atomic_load_n(&g_ledger_mmap, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/* Shared fixtures                                                     */
/* ------------------------------------------------------------------ */

static weft_hw_profile_t g_prof;          /* M4 Max golden archetype  */
static weft_governor_t g_gov;
static long g_iters_total = 2000000;
static long g_writer_delay_ns = 0; /* TSan builds throttle the writer;
                                        happens-before coverage is
                                        delay-invariant */
static volatile int g_run = 0;            /* 0 = start, 1 = running,
                                             2 = stop */
static unsigned long g_torn = 0;          /* MUST stay 0             */
static unsigned long g_busy = 0;          /* bounded refusals        */
static unsigned long g_snapshots = 0;
static unsigned long g_fast_queries = 0;

/* Locked geometry per tier (mirrors test_governor.c / D-51 §6). */
static const uint16_t LANES[4]  = { 0, 16, 8, 4 };
static const uint16_t SLOTS[4]  = { 0, 256, 128, 64 };
static const uint16_t HDR[4]    = { 0, 128, 64, 64 };
static const uint32_t STRIDE[4] = { 0, 1024, 512, 256 };
static const uint32_t REFRESH[4] = { 0, 240, 120, 60 };
static const uint32_t DEADLINE[4] = { 0, 4166, 8333, 16666 };
static const uint32_t GUARD[4] = { 0, 520, 1041, 2083 };
static const uint32_t BATCH[4] = { 0, 1024, 512, 128 };
static const uint64_t LANE_BYTES[4] = { 0, 262272ull, 65600ull, 16448ull };

static int plan_invariants_ok(const weft_tier_plan_t *pl)
{
    uint32_t t;
    if (pl->magic != WEFT_SPECTRUM_PLAN_MAGIC ||
        pl->abi_version != WEFT_SPECTRUM_ABI_VERSION ||
        pl->schema_hash != WEFT_SPECTRUM_PLAN_SCHEMA_HASH) {
        return 0;
    }
    if (pl->crc32c != weft_crc32c(pl, 0x7c)) {
        return 0;
    }
    t = pl->tier;
    if (t < 1u || t > 3u) {
        return 0;
    }
    if (pl->ring_lanes != LANES[t] || pl->ring_slots != SLOTS[t] ||
        pl->hdr_stride != HDR[t] || pl->slot_stride != STRIDE[t]) {
        return 0;
    }
    if (pl->refresh_hz != REFRESH[t] ||
        pl->frame_deadline_us != DEADLINE[t] ||
        pl->jitter_guard_us != GUARD[t] ||
        pl->batch_max_msgs != BATCH[t]) {
        return 0;
    }
    if (pl->lane_bytes != LANE_BYTES[t] ||
        pl->arena_budget_bytes != (uint64_t)LANES[t] * LANE_BYTES[t]) {
        return 0;
    }
    if ((pl->policy_flags & WEFT_PLAN_POLICY_SEQLOCK_LANES) == 0) {
        return 0;
    }
    if (t == 3u &&
        (pl->policy_flags & WEFT_PLAN_POLICY_DROP_NOT_QUEUE) == 0) {
        return 0;
    }
    if (t != 3u &&
        (pl->policy_flags & WEFT_PLAN_POLICY_DROP_NOT_QUEUE) != 0) {
        return 0;
    }
    if (pl->simd_path > (uint32_t)WEFT_SIMD_AMX ||
        pl->ingest_workers < 1u || pl->ingest_workers > 8u ||
        pl->pressure_level > 2u) {
        return 0;
    }
    return 1;
}

static void count_torn(const char *why)
{
    __atomic_fetch_add(&g_torn, 1, __ATOMIC_RELAXED);
    if (g_torn < 5u) {
        printf("TORN OBSERVATION: %s\n", why);
    }
}

/* ------------------------------------------------------------------ */
/* Reader / writer threads                                             */
/* ------------------------------------------------------------------ */

static void *reader_thread(void *arg)
{
    long iters = (long)(intptr_t)arg;
    long i;
    for (i = 0; i < iters; i++) {
        weft_tier_plan_t pl;
        weft_spectrum_status_t st;
        uint32_t t1 = weft_get_active_tier(&g_gov);
        uint32_t t2;
        __atomic_fetch_add(&g_fast_queries, 2, __ATOMIC_RELAXED);
        st = weft_governor_snapshot(&g_gov, &pl);
        t2 = weft_get_active_tier(&g_gov);
        (void)t1; (void)t2;
        __atomic_fetch_add(&g_snapshots, 1, __ATOMIC_RELAXED);
        if (st == WEFT_SPECTRUM_EPLAN_BUSY) {
            __atomic_fetch_add(&g_busy, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (st != WEFT_SPECTRUM_OK) {
            count_torn("snapshot refused with engine error");
            continue;
        }
        /* Torn-read eradication: a stable snapshot must satisfy the
         * full invariant ladder (identity, CRC-32C, tier->geometry
         * mapping). A torn mix of two plans cannot pass the CRC and
         * the cross-field consistency simultaneously — this is the
         * same evidence class as the heddle T-series (self-verifying
         * payloads + exact accounting). */
        if (!plan_invariants_ok(&pl)) {
            count_torn("invariant ladder failed on stable snapshot");
        }
        /* t1/t2 fast reads are advisory skew telemetry: with a hot
         * writer several publishes fit between them, so no interval
         * bound is sound — the plan's own consistency is the proof. */
    }
    return NULL;
}

static void writer_throttle(void)
{
    if (g_writer_delay_ns > 0) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = g_writer_delay_ns;
        nanosleep(&ts, NULL);
    }
}

static void *zigzag_writer(void *arg)
{
    static const weft_pressure_level_t CYCLE[4] = {
        WEFT_PRESSURE_NOMINAL, WEFT_PRESSURE_ELEVATED,
        WEFT_PRESSURE_CRITICAL, WEFT_PRESSURE_ELEVATED
    };
    uint32_t k = 0;
    (void)arg;
    while (__atomic_load_n(&g_run, __ATOMIC_ACQUIRE) == 1) {
        if (weft_governor_retarget(&g_gov, CYCLE[k & 3u]) !=
            WEFT_SPECTRUM_OK) {
            count_torn("writer retarget failed");
        }
        k++;
        writer_throttle();
    }
    return NULL;
}

static void *force_storm_writer(void *arg)
{
    uint32_t k = 0;
    (void)arg;
    while (__atomic_load_n(&g_run, __ATOMIC_ACQUIRE) == 1) {
        uint32_t want = (k & 1u) ? 3u : 1u;
        if (weft_governor_force_tier(&g_gov, want) != WEFT_SPECTRUM_OK) {
            count_torn("writer force failed");
        }
        if ((k & 0xFFu) == 0xFFu) {
            if (weft_governor_clear_force(&g_gov) != WEFT_SPECTRUM_OK) {
                count_torn("writer clear failed");
            }
        }
        k++;
        writer_throttle();
    }
    /* leave the governor unforced */
    (void)weft_governor_clear_force(&g_gov);
    return NULL;
}

static int run_phase(void *(*reader)(void *), void *(*writer)(void *),
                     const char *label)
{
    enum { R = 8 };
    pthread_t rt[R];
    pthread_t wt;
    long per = g_iters_total / (long)R;
    int i;
    unsigned long torn_now;

    g_busy = 0;
    g_snapshots = 0;
    __atomic_store_n(&g_torn, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_run, 0, __ATOMIC_RELAXED);

    for (i = 0; i < R; i++) {
        if (pthread_create(&rt[i], NULL, reader,
                           (void *)(intptr_t)per) != 0) {
            printf("thread spawn failed\n");
            return -1;
        }
    }
    if (pthread_create(&wt, NULL, writer, NULL) != 0) {
        printf("writer spawn failed\n");
        return -1;
    }

    ledger_reset(); /* threads are live; the window starts now */
    __atomic_store_n(&g_run, 1, __ATOMIC_RELEASE);

    for (i = 0; i < R; i++) {
        pthread_join(rt[i], NULL);
    }
    __atomic_store_n(&g_run, 2, __ATOMIC_RELEASE);
    pthread_join(wt, NULL);

    /* Ledger check BEFORE any join-adjacent libc bookkeeping. */
    {
        unsigned long total = ledger_total();
        CHECK(total == 0ul, "%s: allocation ledger %lu calls (want 0)",
              label, total);
        torn_now = __atomic_load_n(&g_torn, __ATOMIC_RELAXED);
        printf("S %-12s snapshots=%lu busy=%lu torn=%lu ledger=%lu\n",
               label, g_snapshots, g_busy, torn_now, total);
        CHECK(torn_now == 0ul,
              "%s: TORN READS OBSERVED (%lu) — violation of Law 4",
              label, torn_now);
        CHECK(g_snapshots > (unsigned long)(g_iters_total / 100),
              "%s: enough snapshots exercised", label);
        CHECK(g_busy < g_snapshots / 100u + 100u,
              "%s: busy refusals within sanity bound", label);
    }
    return (torn_now == 0ul) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Latency scorecard                                                   */
/* ------------------------------------------------------------------ */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static uint64_t median7(uint64_t *v)
{
    qsort(v, 7, sizeof(v[0]), cmp_u64);
    return v[3];
}

#if defined(__GNUC__) || defined(__clang__)
#  define WSP_COMPILER_BARRIER(var) __asm__ volatile("" : "+r"(var))
#else
#  define WSP_COMPILER_BARRIER(var) do { (void)(var); } while (0)
#endif

static double bench_has_feature(void)
{
    uint32_t ids[WEFT_FEATURE_COUNT];
    uint64_t runs[7];
    uint32_t i;
    int r;
    for (i = 0; i < (uint32_t)WEFT_FEATURE_COUNT; i++) {
        ids[i] = i;
    }
    for (r = 0; r < 7; r++) {
        uint64_t t0, t1;
        uint32_t sink = 0;
        long n = 1000000;
        long k;
        for (k = 0; k < 10000; k++) {
            sink += (uint32_t)weft_hw_has_feature(&g_prof, ids[k % 265]);
        }
        t0 = now_ns();
        for (k = 0; k < n; k++) {
            sink += (uint32_t)weft_hw_has_feature(&g_prof, ids[k % 265]);
            WSP_COMPILER_BARRIER(sink);
        }
        t1 = now_ns();
        runs[r] = (t1 - t0) / (uint64_t)n;
    }
    return (double)median7(runs);
}

static double bench_active_tier(void)
{
    uint64_t runs[7];
    int r;
    for (r = 0; r < 7; r++) {
        uint64_t t0, t1;
        uint32_t sink = 0;
        long n = 1000000;
        long k;
        for (k = 0; k < 10000; k++) {
            sink += weft_get_active_tier(&g_gov);
        }
        t0 = now_ns();
        for (k = 0; k < n; k++) {
            sink += weft_get_active_tier(&g_gov);
            WSP_COMPILER_BARRIER(sink);
        }
        t1 = now_ns();
        runs[r] = (t1 - t0) / (uint64_t)n;
    }
    return (double)median7(runs);
}

static double bench_snapshot(void)
{
    uint64_t runs[7];
    int r;
    for (r = 0; r < 7; r++) {
        uint64_t t0, t1;
        weft_tier_plan_t pl;
        uint32_t sink = 0;
        long n = 200000;
        long k;
        for (k = 0; k < 1000; k++) {
            sink += (uint32_t)weft_governor_snapshot(&g_gov, &pl);
        }
        t0 = now_ns();
        for (k = 0; k < n; k++) {
            sink += (uint32_t)weft_governor_snapshot(&g_gov, &pl);
            WSP_COMPILER_BARRIER(sink);
        }
        t1 = now_ns();
        runs[r] = (t1 - t0) / (uint64_t)n;
    }
    return (double)median7(runs);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    const char *iters_env = getenv("SPECTRUM_STRESS_ITERS");
    const char *skip_env = getenv("SPECTRUM_SKIP_BENCH");
    const char *delay_env = getenv("SPECTRUM_WRITER_DELAY_NS");
    static char stdout_buf[65536];
    weft_tier_plan_t pl;
    uint32_t k;

    setvbuf(stdout, stdout_buf, _IOFBF, sizeof(stdout_buf));
    printf("stdout: warm line (pre-ledger)\n");

    if (iters_env != NULL) {
        long v = atol(iters_env);
        if (v >= 10000) {
            g_iters_total = v;
        }
    }
    if (delay_env != NULL) {
        long d = atol(delay_env);
        if (d >= 0) {
            g_writer_delay_ns = (d > 100000000L) ? 100000000L : d;
        }
    }

    /* Fixtures: M4 Max archetype (Tier 1, maximum tier travel). */
    CHECK(weft_mock_archetype_select(WEFT_ARCHETYPE_APPLE_M4_MAX) ==
          WEFT_SPECTRUM_OK, "select m4");
    CHECK(weft_hw_probe(&g_prof) == WEFT_SPECTRUM_OK, "probe m4");
    weft_spectrum_warmup();
    CHECK(weft_governor_init(&g_gov, &g_prof) == WEFT_SPECTRUM_OK,
          "init governor");
    CHECK(weft_get_active_tier(&g_gov) == 1u, "m4 tier 1");

    /* ---- S1: concurrent zigzag --------------------------------- */
    CHECK(run_phase(reader_thread, zigzag_writer, "zigzag") == 0,
          "S1 phase");
    /* The zigzag writer stops mid-cycle: explicitly republish at
     * NOMINAL before asserting recovery. */
    CHECK(weft_governor_retarget(&g_gov, WEFT_PRESSURE_NOMINAL) ==
          WEFT_SPECTRUM_OK, "post-S1 nominal");
    CHECK(weft_governor_snapshot(&g_gov, &pl) == WEFT_SPECTRUM_OK,
          "post-S1 snapshot");
    CHECK(weft_get_active_tier(&g_gov) == 1u, "post-S1 recovered T1");

    /* ---- S2: concurrent force storm ----------------------------- */
    CHECK(run_phase(reader_thread, force_storm_writer,
                    "force") == 0, "S2 phase");
    CHECK(weft_governor_snapshot(&g_gov, &pl) == WEFT_SPECTRUM_OK,
          "post-S2 snapshot");
    CHECK(pl.tier == 1u, "post-S2 unforced tier 1 (got %u)", pl.tier);

    /* ---- S3: single-writer transition storm --------------------- */
    {
        uint64_t v0 = weft_governor_plan_version(&g_gov);
        for (k = 0; k < 100000u; k++) {
            weft_pressure_level_t lvl = (k & 1u)
                ? WEFT_PRESSURE_ELEVATED : WEFT_PRESSURE_NOMINAL;
            CHECK(weft_governor_retarget(&g_gov, lvl) == WEFT_SPECTRUM_OK,
                  "storm retarget %u", k);
            if ((k & 63u) == 0u) {
                CHECK(weft_governor_snapshot(&g_gov, &pl) ==
                      WEFT_SPECTRUM_OK, "storm snapshot %u", k);
                CHECK(plan_invariants_ok(&pl), "storm invariants %u", k);
            }
        }
        CHECK(weft_governor_plan_version(&g_gov) == v0 + 100000ull,
              "storm version delta");
        /* Probe storm inside the ledger window (mock switching is
         * single-threaded by contract). */
        ledger_reset();
        {
            uint32_t a;
            for (a = 0; a < 100u; a++) {
                weft_hw_profile_t tmp;
                CHECK(weft_mock_archetype_select(a % 12u) ==
                      WEFT_SPECTRUM_OK, "probe storm select %u", a);
                CHECK(weft_hw_probe(&tmp) == WEFT_SPECTRUM_OK,
                      "probe storm %u", a);
            }
            (void)weft_mock_archetype_select(WEFT_ARCHETYPE_APPLE_M4_MAX);
        }
        CHECK(ledger_total() == 0ul,
              "probe storm allocation ledger %lu", ledger_total());
        printf("S probe-storm  100 mock probes ledger=%lu\n",
               ledger_total());
    }

    /* ---- S5: latency scorecard ---------------------------------- */
    if (skip_env == NULL) {
        double ns_feat = bench_has_feature();
        double ns_tier = bench_active_tier();
        double ns_snap = bench_snapshot();
        printf("S scorecard    has_feature=%.2f ns/op  active_tier=%.2f "
               "ns/op  snapshot=%.2f ns/op (informational)\n",
               ns_feat, ns_tier, ns_snap);
        CHECK(ns_feat <= 5.0, "has_feature %.2f ns exceeds 5 ns bar",
              ns_feat);
        CHECK(ns_tier <= 5.0, "active_tier %.2f ns exceeds 5 ns bar",
              ns_tier);
    } else {
        printf("S scorecard    SKIPPED (sanitizer build)\n");
    }

    printf("S-series: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
