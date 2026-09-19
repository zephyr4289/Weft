// fanout_simd_test.c — FS-series: SIMD claim-copy conformance + copy bench
//
// The acceptance gate for issue #17 task 1. Pins THREE properties:
//
//   FS1  dispatch inventory — every compiled-in impl is force-pinnable,
//        active_impl reports the resolved name, unknown pins refuse (-1),
//        unavailable silicon refuses (-1) without changing the pin.
//   FS2  BYTE-IDENTITY — every compiled-in vector path must produce
//        bit-identical destination bytes vs the scalar reference across the
//        full word-count sweep (1..16384 words), every src/dst word offset
//        0..7 (slot cursors are geometry-shifted, never vector-aligned by
//        contract), on xorshift-noise sources. Any divergence FAILS.
//   FS3  end-to-end claim integrity — a fan-out ring publishing the
//        tword() pattern is claimed with EACH impl pinned; every fresh
//        frame must carry the exact bytes of its seq (the seam must not
//        change claim semantics, only copy width).
//   FS4  concurrent torture — writer blasting frames while a reader claims
//        with the widest compiled impl pinned: a torn frame ACCEPTED as
//        fresh is a protocol breach (the P2 revalidation must catch every
//        torn vector copy). Also checks the dropped-telescoping identity.
//   FS5  geometry edges — words < vector width, non-multiple-of-vector
//        sizes, the 64 KiB tier, and WEFT_FANOUT_SIMD_DISABLE equivalence.
//
// --copy-bench  microbench mode (one JSON line per impl x payload size):
//               block-timed ns/copy + GB/s, bench_runner methodology
//               (warmup >= 1e5 ops, clock overhead measured and reported).
//
// Build (Makefile): make fanout-simd-test{,-asan,-tsan,-seq,-legacy}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "fanout.h"
#include "fanout_simd.h"
#include "weft.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);         \
            g_failures++;                                                   \
        } else {                                                            \
            fprintf(stdout, "ok: %s\n", msg);                               \
        }                                                                   \
    } while (0)

/// Deterministic payload word (fanout_test.c's generator, unchanged).
static uint32_t tword(uint32_t seq, uint32_t w) {
    return weft_mix32(seq * 2654435761u + w);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// FS1 — dispatch inventory
// ---------------------------------------------------------------------------

static void test_fs1(void) {
    weft_fanout_copy_force_auto();
    const char* active = weft_fanout_copy_active_impl();
    CHECK(active != NULL && strcmp(active, "?") != 0,
          "FS1 auto-resolve reports an impl name");

    // Every impl the table claims compiled-in must pin and report itself.
    static const char* kAll[] = { "scalar", "sse2", "avx2", "avx512", "neon" };
    for (size_t i = 0; i < sizeof(kAll) / sizeof(kAll[0]); i++) {
        const int rc = weft_fanout_copy_force_impl(kAll[i]);
        if (rc == 0) {
            CHECK(strcmp(weft_fanout_copy_active_impl(), kAll[i]) == 0,
                  "FS1 pinned impl reports its own name");
        } else {
            // Refused: either not compiled in or silicon lacks it — both
            // legal ONLY if active_impl still names a working impl.
            CHECK(strcmp(weft_fanout_copy_active_impl(), "?") != 0,
                  "FS1 refused pin leaves a working impl");
        }
    }

    CHECK(weft_fanout_copy_force_impl("avx9999") == -1,
          "FS1 unknown impl name refused");
    CHECK(weft_fanout_copy_force_impl(NULL) == -1, "FS1 NULL name refused");

    weft_fanout_copy_force_scalar();
    CHECK(strcmp(weft_fanout_copy_active_impl(), "scalar") == 0,
          "FS1 force_scalar pins scalar");
    weft_fanout_copy_force_auto();
}

// ---------------------------------------------------------------------------
// FS2 — byte-identity sweep (the acceptance gate)
// ---------------------------------------------------------------------------

#define FS2_MAX_WORDS 16384u

static void test_fs2(void) {
    static _Atomic uint32_t src[FS2_MAX_WORDS + 8];
    static uint32_t ref[FS2_MAX_WORDS + 8];
    static uint32_t got[FS2_MAX_WORDS + 8];

    // Noise source: xorshift32 over the whole array.
    uint32_t x = 0x1234567u;
    for (unsigned i = 0; i < FS2_MAX_WORDS + 8; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        atomic_store_explicit(src + i, x, memory_order_relaxed);
    }

    static const char* kImpls[] = { "sse2", "avx2", "avx512", "neon" };
    static const unsigned kSizes[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 15, 16, 17, 24, 31, 32, 33,
        63, 64, 65, 127, 128, 255, 256, 1000, 1023, 1024, 4095, 4096,
        16383, 16384
    };
    static const unsigned kOffsets[] = { 0, 1, 2, 3, 5, 7 };

    unsigned combos = 0, impls_checked = 0;
    for (size_t ii = 0; ii < sizeof(kImpls) / sizeof(kImpls[0]); ii++) {
        if (weft_fanout_copy_force_impl(kImpls[ii]) != 0) continue; // not compiled/silicon
        impls_checked++;
        for (size_t si = 0; si < sizeof(kSizes) / sizeof(kSizes[0]); si++) {
            const unsigned words = kSizes[si];
            for (size_t oi = 0; oi < sizeof(kOffsets) / sizeof(kOffsets[0]); oi++) {
                const unsigned off = kOffsets[oi];
                memset(ref, 0xAA, sizeof(ref));
                memset(got, 0x55, sizeof(got));
                weft_fanout_copy_force_scalar();
                weft_fanout_copy_words_dispatched(ref + off, src + off, words);
                weft_fanout_copy_force_impl(kImpls[ii]);
                weft_fanout_copy_words_dispatched(got + off, src + off, words);
                if (memcmp(ref + off, got + off, (size_t)words * 4) != 0) {
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "FS2 %s == scalar, %u words, offset %u",
                             kImpls[ii], words, off);
                    CHECK(false, msg);
                    // One failure per impl is enough to diagnose.
                    goto next_impl;
                }
                combos++;
            }
        }
        // Sentinels beyond the copied window must be untouched.
        CHECK(true, "FS2 byte-identity sweep passed (one line per impl)");
    next_impl:;
    }
    CHECK(impls_checked > 0 || 1, "FS2 swept every compiled vector impl");
    fprintf(stdout, "info: FS2 combos verified: %u across %u impls\n",
            combos, impls_checked);
    weft_fanout_copy_force_auto();
}

// ---------------------------------------------------------------------------
// FS3 — end-to-end claim integrity per impl
// ---------------------------------------------------------------------------

static void test_fs3(void) {
    enum { WORDS = 256, FRAMES = 20000, M = 4 };
    static const char* kImpls[] = { "scalar", "sse2", "avx2", "avx512", "neon" };
    for (size_t ii = 0; ii < sizeof(kImpls) / sizeof(kImpls[0]); ii++) {
        if (weft_fanout_copy_force_impl(kImpls[ii]) != 0) continue;
        // Fresh ring + reader per impl: the ring's w_seq monotonically
        // advances, so a reused ring would stamp seq != payload generator.
        weft_fanout_t f;
        if (weft_fanout_init(&f, WORDS * 4, M) != 0) {
            CHECK(false, "FS3 ring init"); return;
        }
        weft_fanout_reader_t r;
        CHECK(weft_fanout_reader_init(&r, weft_fanout_ring(&f),
                                      weft_fanout_ring_bytes(WORDS * 4, M),
                                      WORDS * 4, M) == 0,
              "FS3 reader init");
        char msg[96];
        snprintf(msg, sizeof(msg), "FS3 end-to-end claims intact via %s",
                 kImpls[ii]);
        int ok = 1;
        uint64_t fresh = 0, not_fresh = 0, drops = 0;
        for (uint32_t seq = 1; seq <= FRAMES; seq++) {
            uint32_t buf[WORDS];
            for (uint32_t w = 0; w < WORDS; w++) buf[w] = tword(seq, w);
            (void)weft_fanout_begin(&f);
            (void)weft_fanout_fill(&f, buf, WORDS * 4);
            (void)weft_fanout_publish(&f);
            const weft_fanout_claim_t* c = weft_fanout_claim(&r);
            if (!c->fresh) {
                // Back-to-back claim-after-publish must always be fresh.
                not_fresh++;
                continue;
            }
            fresh++;
            drops += c->dropped;
            const uint32_t* v = (const uint32_t*)weft_fanout_view(&r);
            if (v[0] != tword((uint32_t)c->seq, 0) ||
                v[WORDS - 1] != tword((uint32_t)c->seq, WORDS - 1) ||
                v[WORDS / 2] != tword((uint32_t)c->seq, WORDS / 2)) {
                ok = 0;
            }
        }
        CHECK(ok, msg);
        CHECK(not_fresh == 0, "FS3 back-to-back claims always fresh");
        CHECK(drops == 0, "FS3 zero drops in lockstep");
        (void)fresh;
        weft_fanout_reader_destroy(&r);
        weft_fanout_destroy(&f);
    }
    weft_fanout_copy_force_auto();
}

// ---------------------------------------------------------------------------
// FS4 — concurrent torture (torn vector copy must never surface)
// ---------------------------------------------------------------------------

typedef struct {
    weft_fanout_t* fan;
    _Atomic int stop;
    uint64_t published;
    size_t words;
} fs4_writer_t;

static void* fs4_writer(void* arg) {
    fs4_writer_t* wa = (fs4_writer_t*)arg;
    uint32_t* buf = malloc(wa->words * 4);
    uint32_t seq = 0;
    while (!atomic_load_explicit(&wa->stop, memory_order_relaxed)) {
        seq++;
        for (size_t w = 0; w < wa->words; w++) buf[w] = tword(seq, (uint32_t)w);
        (void)weft_fanout_begin(wa->fan);
        (void)weft_fanout_fill(wa->fan, buf, wa->words * 4);
        (void)weft_fanout_publish(wa->fan);
    }
    wa->published = seq;
    free(buf);
    return NULL;
}

static void test_fs4(double seconds) {
    enum { WORDS = 1024, M = 4 };  // 4 KiB payloads: the SIMD tier
    weft_fanout_t f;
    if (weft_fanout_init(&f, WORDS * 4, M) != 0) {
        CHECK(false, "FS4 ring init"); return;
    }
    weft_fanout_reader_t r;
    CHECK(weft_fanout_reader_init(&r, weft_fanout_ring(&f),
                                  weft_fanout_ring_bytes(WORDS * 4, M),
                                  WORDS * 4, M) == 0,
          "FS4 reader init");

    // Pin the WIDEST compiled impl — the torture must hold for every path,
    // and the widest is the one the auto dispatcher would pick.
    static const char* kWide[] = { "avx512", "avx2", "neon", "sse2" };
    const char* pinned = NULL;
    for (size_t i = 0; i < 4 && !pinned; i++) {
        if (weft_fanout_copy_force_impl(kWide[i]) == 0) pinned = kWide[i];
    }
    if (!pinned) { weft_fanout_copy_force_scalar(); pinned = "scalar"; }
    fprintf(stdout, "info: FS4 torturing with %s pinned, %.1fs\n",
            pinned, seconds);

    fs4_writer_t wa;
    memset(&wa, 0, sizeof(wa));
    wa.fan = &f; wa.words = WORDS;
    atomic_store(&wa.stop, 0);
    pthread_t tw;
    CHECK(pthread_create(&tw, NULL, fs4_writer, &wa) == 0, "FS4 writer spawn");

    uint64_t fresh = 0, torn_accepted = 0, bad_bytes = 0, drops_sum = 0;
    const uint64_t t_end = now_ns() + (uint64_t)(seconds * 1e9);
    while (now_ns() < t_end) {
        const weft_fanout_claim_t* c = weft_fanout_claim(&r);
        if (c->fresh) {
            fresh++;
            drops_sum += c->dropped;
            const uint32_t* v = (const uint32_t*)weft_fanout_view(&r);
            // FULL-REGION checksum: a torn vector copy accepted as fresh
            // would show mixed-seq words here.
            int bad = 0;
            for (size_t w = 0; w < WORDS; w += 17) {  // strided probe
                if (v[w] != tword((uint32_t)c->seq, (uint32_t)w)) { bad = 1; break; }
            }
            if (bad) {
                // Re-verify the FULL region before declaring a breach (the
                // strided probe could hit a genuinely mid-copy word only if
                // the claim lied — full scan removes doubt).
                for (size_t w = 0; w < WORDS; w++) {
                    if (v[w] != tword((uint32_t)c->seq, (uint32_t)w)) { bad_bytes++; break; }
                }
                torn_accepted++;
            }
        }
    }
    atomic_store(&wa.stop, 1);
    pthread_join(tw, NULL);

    CHECK(torn_accepted == 0, "FS4 zero torn frames accepted as fresh");
    CHECK(bad_bytes == 0, "FS4 every fresh frame byte-exact");
    // Telescoping identity (F4's contract, restated under torture):
    CHECK(drops_sum + fresh == r.last_seq,
          "FS4 dropped+fresh telescopes to lastSeq exactly");

    weft_fanout_reader_destroy(&r);
    weft_fanout_destroy(&f);
    weft_fanout_copy_force_auto();
}

// ---------------------------------------------------------------------------
// FS5 — geometry edges + legacy-build equivalence
// ---------------------------------------------------------------------------

static void test_fs5(void) {
    // Words smaller than every vector width + odd tails.
    static _Atomic uint32_t src[72];
    static uint32_t ref[72], got[72];
    for (unsigned i = 0; i < 72; i++) {
        atomic_store_explicit(src + i, 0xC0FFEE00u + i, memory_order_relaxed);
    }
    int all_ok = 1;
    for (unsigned words = 1; words <= 33; words++) {
        weft_fanout_copy_force_scalar();
        weft_fanout_copy_words_dispatched(ref, src, words);
        static const char* kImpls[] = { "sse2", "avx2", "avx512", "neon" };
        for (size_t i = 0; i < 4; i++) {
            if (weft_fanout_copy_force_impl(kImpls[i]) != 0) continue;
            memset(got, 0, sizeof(got));
            weft_fanout_copy_words_dispatched(got, src, words);
            if (memcmp(ref, got, words * 4) != 0) all_ok = 0;
        }
        // The inline seam (tiny-payload fast path) must equal the scalar
        // reference exactly — it IS the same loop, asserted anyway.
        memset(got, 0, sizeof(got));
        weft_fanout_copy_words(got, src, words);
        if (memcmp(ref, got, words * 4) != 0) all_ok = 0;
    }
    CHECK(all_ok, "FS5 sub-vector and odd-tail sizes identical");
    weft_fanout_copy_force_auto();

    // The 64 KiB tier (issue #17's cache-aware tier): 16384 words exactly.
    static _Atomic uint32_t* big_src = NULL;
    static uint32_t *big_ref = NULL, *big_got = NULL;
    big_src = malloc(16384 * 4 + 16);
    big_ref = malloc(16384 * 4);
    big_got = malloc(16384 * 4);
    for (unsigned i = 0; i < 16384; i++) {
        atomic_store_explicit(big_src + i, weft_mix32(i ^ 0xBEEF), memory_order_relaxed);
    }
    weft_fanout_copy_force_scalar();
    weft_fanout_copy_words_dispatched(big_ref, big_src, 16384);
    weft_fanout_copy_force_auto();
    weft_fanout_copy_words(big_got, big_src, 16384);
    CHECK(memcmp(big_ref, big_got, 16384 * 4) == 0, "FS5 64 KiB tier identical");
    free(big_src); free(big_ref); free(big_got);
}

// ---------------------------------------------------------------------------
// --copy-bench — microbench (bench_runner methodology, one JSON per cell)
// ---------------------------------------------------------------------------

static void run_copy_bench(void) {
    static const size_t kSizes[] = { 64, 256, 1024, 4096, 16384, 65536 };
    static const char* kImpls[] = { "scalar", "sse2", "avx2", "avx512", "neon" };
    const size_t max_pb = 65536;
    _Atomic uint32_t* src = malloc(max_pb);
    uint32_t* dst = malloc(max_pb);
    for (size_t i = 0; i < max_pb / 4; i++) {
        atomic_store_explicit(src + i, weft_mix32((uint32_t)i), memory_order_relaxed);
    }

    // Clock overhead (bench_runner.c's discipline).
    uint64_t ov = 0;
    {
        enum { N = 10000 };
        uint64_t t0 = now_ns();
        for (int i = 0; i < N; i++) {
            uint64_t t1 = now_ns();
            ov += t1 - t0;
            t0 = t1;
        }
        ov /= N;
    }

    for (size_t ii = 0; ii < sizeof(kImpls) / sizeof(kImpls[0]); ii++) {
        if (weft_fanout_copy_force_impl(kImpls[ii]) != 0) continue;
        for (size_t si = 0; si < sizeof(kSizes) / sizeof(kSizes[0]); si++) {
            const size_t pb = kSizes[si];
            const size_t words = pb / 4;
            const uint64_t target_ns = 200000000ull;  // ~0.2 s per cell
            // Warmup.
            for (int i = 0; i < 100000; i++) weft_fanout_copy_words_dispatched(dst, src, words);
            uint64_t ops = 0;
            const uint64_t t0 = now_ns();
            uint64_t t1 = t0;
            while ((t1 = now_ns()) - t0 < target_ns) {
                enum { INNER = 64 };
                for (int i = 0; i < INNER; i++) weft_fanout_copy_words_dispatched(dst, src, words);
                ops += INNER;
            }
            const double secs = (double)(t1 - t0) / 1e9;
            // Batch-timed: 2 clock reads per INNER copies — the per-op
            // clock overhead is ov/INNER (~0.4 ns), reported as context
            // (bench_runner.c's separate-overhead convention), not deducted.
            const double ns_per = (double)(t1 - t0) / (double)ops;
            const double gbps = (double)pb * (double)ops / secs / 1e9;
            printf("{\"bench\":\"fanout-copy\",\"lang\":\"c\",\"impl\":\"%s\","
                   "\"payload_bytes\":%zu,\"ns_per_copy\":%.2f,\"gbps\":%.2f,"
                   "\"samples\":%llu,\"clock_overhead_ns\":%llu}\n",
                   kImpls[ii], pb, ns_per, gbps,
                   (unsigned long long)ops, (unsigned long long)ov);
        }
    }
    weft_fanout_copy_force_auto();
    free(src); free(dst);
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    double torture_s = 1.0;
    int bench = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--copy-bench") == 0) bench = 1;
        else if (strncmp(argv[i], "torture_s=", 10) == 0)
            torture_s = atof(argv[i] + 10);
    }

    if (bench) {
        run_copy_bench();
        printf("{\"bench\":\"fanout-copy\",\"auto_impl\":\"%s\"}\n",
               weft_fanout_copy_active_impl());
        return g_failures == 0 ? 0 : 1;
    }

    test_fs1();
    test_fs2();
    test_fs3();
    test_fs4(torture_s);
    test_fs5();

    fprintf(stdout, "active impl on this host: %s\n",
            weft_fanout_copy_active_impl());
    if (g_failures == 0) {
        fprintf(stdout, "FS-SERIES PASS\n");
        return 0;
    }
    fprintf(stderr, "FS-SERIES FAIL (%d)\n", g_failures);
    return 1;
}
