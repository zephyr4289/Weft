// fanout_batch_test.c — FB-series: batch publish conformance + bench
// (issue #17 task 3 acceptance)
//
//   FB1  batch roundtrip — n in {1,2,4,8,16,64}, payload sweep: freshest
//        claim carries the LAST frame's bytes exactly; seq continuity with
//        the surrounding single-frame publishes; publishes telemetry == n.
//   FB2  mixed-mode compatibility — single publishes before AND after
//        batches interleave seamlessly (the batch is just a fast writer).
//   FB3  dropped accounting — a reader behind a 100-frame batch (M=4)
//        sees dropped == 99, seq == batch tail, sum telescopes exactly.
//   FB4  concurrent torture — reader claiming while a writer batch-
//        publishes n > M bursts (windowed stamping): ZERO torn frames
//        accepted; every fresh frame byte-exact; telescoping exact.
//   FB5  malformed-frame refusal is atomic — a bad len anywhere in the
//        batch publishes NOTHING (latestSeq/publishes unchanged).
//   FB6  commit-only API — n manual begins + weft_publish_batch_commit
//        publishes the same frames the batch API would.
//   --bench  throughput A/B: 100-frame batches vs 100 single publishes,
//        idle ring and reader-polling legs (the contention amortization),
//        one JSON line per cell.
//
// Build: make fanout-batch-test{,-asan,-tsan,-seq}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "fanout.h"
#include "fanout_batch.h"
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

static uint32_t tword(uint32_t seq, uint32_t w) {
    return weft_mix32(seq * 2654435761u + w);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void fill_words(uint32_t* buf, uint32_t seq, size_t words) {
    for (size_t w = 0; w < words; w++) buf[w] = tword(seq, (uint32_t)w);
}

// ---------------------------------------------------------------------------
// FB1 — batch roundtrip
// ---------------------------------------------------------------------------

static void test_fb1(void) {
    enum { WORDS = 256, M = 8 };
    static const size_t kN[] = { 1, 2, 4, 8, 16, 64 };
    int all_ok = 1, tel_ok = 1, seq_ok = 1;
    weft_fanout_t f;
    weft_fanout_reader_t r;
    if (weft_fanout_init(&f, WORDS * 4, M) != 0) { CHECK(false, "FB1 init"); return; }
    weft_fanout_reader_init(&r, weft_fanout_ring(&f),
                            weft_fanout_ring_bytes(WORDS * 4, M), WORDS * 4, M);

    uint64_t expect_seq = 0;
    uint32_t* buf = malloc(WORDS * 4);
    for (size_t ni = 0; ni < sizeof(kN) / sizeof(kN[0]); ni++) {
        const size_t n = kN[ni];
        weft_batch_frame_t frames[64];
        for (size_t i = 0; i < n; i++) {
            fill_words(buf, (uint32_t)(expect_seq + i + 1), WORDS);
            // each frame needs its own buffer copy (fill reads src live)
            frames[i].src = malloc(WORDS * 4);
            memcpy((void*)frames[i].src, buf, WORDS * 4);
            frames[i].len = WORDS * 4;
        }
        const uint64_t got = weft_publish_batch(&f, frames, n);
        const uint64_t want = expect_seq + n;
        if (got != want) seq_ok = 0;
        expect_seq = want;
        const weft_fanout_claim_t* c = weft_fanout_claim(&r);
        if (!c->fresh || c->seq != want || c->dropped != n - 1) all_ok = 0;
        else {
            const uint32_t* v = (const uint32_t*)weft_fanout_view(&r);
            for (size_t w = 0; w < WORDS; w += 7)
                if (v[w] != tword((uint32_t)want, (uint32_t)w)) { all_ok = 0; break; }
        }
        for (size_t i = 0; i < n; i++) free((void*)frames[i].src);
    }
    weft_fanout_debug_t dbg;
    weft_fanout_debug_stats(&f, &dbg);
    if (dbg.publishes != expect_seq) tel_ok = 0;  // one add per frame
    CHECK(seq_ok, "FB1 batch returns the tail seq, w_seq continuity exact");
    CHECK(all_ok, "FB1 freshest claim carries the LAST frame's bytes; dropped == n-1");
    CHECK(tel_ok, "FB1 publishes telemetry counts every frame (n per batch)");

    // Payload-size sweep: len < payload_bytes and len == 0 (begin-only).
    {
        weft_batch_frame_t fr[2];
        uint32_t small[16];
        fill_words(small, (uint32_t)(expect_seq + 1), 16);
        fr[0].src = small; fr[0].len = 64;
        fr[1].src = NULL;  fr[1].len = 0;      // begin-only frame
        const uint64_t got = weft_publish_batch(&f, fr, 2);
        CHECK(got == expect_seq + 2, "FB1 short + begin-only frames accepted");
        const weft_fanout_claim_t* c = weft_fanout_claim(&r);
        CHECK(c->fresh && c->seq == expect_seq + 2,
              "FB1 begin-only tail frame claims (empty payload valid)");
        expect_seq += 2;
    }
    free(buf);
    weft_fanout_reader_destroy(&r);
    weft_fanout_destroy(&f);
}

// ---------------------------------------------------------------------------
// FB2 — mixed-mode compatibility
// ---------------------------------------------------------------------------

static void test_fb2(void) {
    enum { WORDS = 64, M = 4 };
    weft_fanout_t f;
    weft_fanout_reader_t r;
    if (weft_fanout_init(&f, WORDS * 4, M) != 0) { CHECK(false, "FB2 init"); return; }
    weft_fanout_reader_init(&r, weft_fanout_ring(&f),
                            weft_fanout_ring_bytes(WORDS * 4, M), WORDS * 4, M);
    uint32_t buf[WORDS];
    uint64_t seq = 0;
    int ok = 1;
    // single, batch(3), single, batch(2), single — the interleaving story
    for (int round = 0; round < 5; round++) {
        if (round % 2 == 0) {
            seq++;
            fill_words(buf, (uint32_t)seq, WORDS);
            (void)weft_fanout_begin(&f);
            (void)weft_fanout_fill(&f, buf, WORDS * 4);
            (void)weft_fanout_publish(&f);
        } else {
            const size_t n = (round == 1) ? 3 : 2;
            weft_batch_frame_t fr[3];
            for (size_t i = 0; i < n; i++) {
                fill_words(buf, (uint32_t)(seq + 1 + i), WORDS);
                uint32_t* b = malloc(WORDS * 4);
                memcpy(b, buf, WORDS * 4);
                fr[i].src = b; fr[i].len = WORDS * 4;
            }
            const uint64_t got = weft_publish_batch(&f, fr, n);
            if (got != seq + n) ok = 0;
            for (size_t i = 0; i < n; i++) free((void*)fr[i].src);
            seq += n;
        }
        const weft_fanout_claim_t* c = weft_fanout_claim(&r);
        if (!c->fresh || c->seq != seq) ok = 0;
        else {
            const uint32_t* v = (const uint32_t*)weft_fanout_view(&r);
            if (v[0] != tword((uint32_t)seq, 0) || v[WORDS - 1] != tword((uint32_t)seq, WORDS - 1))
                ok = 0;
        }
    }
    CHECK(ok, "FB2 single publishes interleave with batches seamlessly");
    weft_fanout_reader_destroy(&r);
    weft_fanout_destroy(&f);
}

// ---------------------------------------------------------------------------
// FB3 — dropped accounting under a big batch (n > M)
// ---------------------------------------------------------------------------

static void test_fb3(void) {
    enum { WORDS = 64, M = 4, N = 100 };
    weft_fanout_t f;
    weft_fanout_reader_t r;
    if (weft_fanout_init(&f, WORDS * 4, M) != 0) { CHECK(false, "FB3 init"); return; }
    weft_fanout_reader_init(&r, weft_fanout_ring(&f),
                            weft_fanout_ring_bytes(WORDS * 4, M), WORDS * 4, M);
    uint32_t buf[WORDS];
    weft_batch_frame_t fr[N];
    for (size_t i = 0; i < N; i++) {
        uint32_t* b = malloc(WORDS * 4);
        fill_words(b, (uint32_t)(i + 1), WORDS);
        fr[i].src = b; fr[i].len = WORDS * 4;
    }
    const uint64_t got = weft_publish_batch(&f, fr, N);
    CHECK(got == N, "FB3 100-frame batch returns 100");
    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    CHECK(c->fresh && c->seq == N && c->dropped == N - 1,
          "FB3 dropped == 99 behind a 100-frame batch (M=4)");
    const uint32_t* v = (const uint32_t*)weft_fanout_view(&r);
    CHECK(v[0] == tword(N, 0) && v[WORDS - 1] == tword(N, WORDS - 1),
          "FB3 tail frame bytes exact after windowed stamping");
    for (size_t i = 0; i < N; i++) free((void*)fr[i].src);
    weft_fanout_reader_destroy(&r);
    weft_fanout_destroy(&f);
}

// ---------------------------------------------------------------------------
// FB4 — concurrent torture (reader during n > M bursts)
// ---------------------------------------------------------------------------

typedef struct {
    weft_fanout_t* fan;
    _Atomic int stop;
    uint64_t batches, frames;
    size_t words, n;
} fb4_writer_t;

static void* fb4_writer(void* arg) {
    fb4_writer_t* wa = (fb4_writer_t*)arg;
    const size_t n = wa->n, words = wa->words;
    uint32_t* block = malloc(n * words * 4);
    weft_batch_frame_t* fr = malloc(n * sizeof(weft_batch_frame_t));
    uint64_t next = 0;
    while (!atomic_load_explicit(&wa->stop, memory_order_relaxed)) {
        for (size_t i = 0; i < n; i++) {
            const uint64_t s = next + i + 1;
            for (size_t w = 0; w < words; w++)
                block[i * words + w] = tword((uint32_t)s, (uint32_t)w);
            fr[i].src = block + i * words;
            fr[i].len = words * 4;
        }
        next = weft_publish_batch(wa->fan, fr, n);
        wa->batches++;
        wa->frames += n;
    }
    free(block); free(fr);
    return NULL;
}

static void test_fb4(double seconds) {
    enum { WORDS = 256, M = 4 };
    const size_t N = 32;  // batch deeper than the ring: windowed stamping
    weft_fanout_t f;
    weft_fanout_reader_t r;
    if (weft_fanout_init(&f, WORDS * 4, M) != 0) { CHECK(false, "FB4 init"); return; }
    weft_fanout_reader_init(&r, weft_fanout_ring(&f),
                            weft_fanout_ring_bytes(WORDS * 4, M), WORDS * 4, M);

    fb4_writer_t wa;
    memset(&wa, 0, sizeof(wa));
    wa.fan = &f; wa.words = WORDS; wa.n = N;
    atomic_store(&wa.stop, 0);
    pthread_t tw;
    CHECK(pthread_create(&tw, NULL, fb4_writer, &wa) == 0, "FB4 writer spawn");

    uint64_t fresh = 0, torn_accepted = 0, drops_sum = 0;
    const uint64_t t_end = now_ns() + (uint64_t)(seconds * 1e9);
    while (now_ns() < t_end) {
        const weft_fanout_claim_t* c = weft_fanout_claim(&r);
        if (c->fresh) {
            fresh++;
            drops_sum += c->dropped;
            const uint32_t* v = (const uint32_t*)weft_fanout_view(&r);
            for (size_t w = 0; w < WORDS; w += 13) {
                if (v[w] != tword((uint32_t)c->seq, (uint32_t)w)) {
                    torn_accepted++;
                    break;
                }
            }
        }
    }
    atomic_store(&wa.stop, 1);
    pthread_join(tw, NULL);
    CHECK(torn_accepted == 0, "FB4 ZERO torn frames accepted under batch bursts");
    CHECK(drops_sum + fresh == r.last_seq,
          "FB4 dropped+fresh telescopes to lastSeq exactly under batching");
    fprintf(stdout, "info: FB4 %llu batches (%llu frames) vs reader: fresh=%llu\n",
            (unsigned long long)wa.batches, (unsigned long long)wa.frames,
            (unsigned long long)fresh);
    weft_fanout_reader_destroy(&r);
    weft_fanout_destroy(&f);
}

// ---------------------------------------------------------------------------
// FB5 — atomic refusal
// ---------------------------------------------------------------------------

static void test_fb5(void) {
    enum { WORDS = 64, M = 4 };
    weft_fanout_t f;
    if (weft_fanout_init(&f, WORDS * 4, M) != 0) { CHECK(false, "FB5 init"); return; }
    uint32_t buf[WORDS];
    fill_words(buf, 1, WORDS);
    // A good frame then a malformed one (len % 4 != 0):
    weft_batch_frame_t fr[3] = {
        { buf, WORDS * 4 }, { buf, 6 }, { buf, WORDS * 4 },
    };
    weft_fanout_debug_t before;
    weft_fanout_debug_stats(&f, &before);
    const uint64_t got = weft_publish_batch(&f, fr, 3);
    weft_fanout_debug_t after;
    weft_fanout_debug_stats(&f, &after);
    CHECK(got == 0, "FB5 malformed batch refused (returns 0)");
    CHECK(before.latest_seq == after.latest_seq &&
          before.publishes == after.publishes,
          "FB5 refusal is ATOMIC: latestSeq and publishes unchanged");
    CHECK(f.w_seq == before.latest_seq,
          "FB5 refusal leaves w_seq untouched (no half-begun state)");
    // NULL-src with len is also refused:
    weft_batch_frame_t bad[1] = { { NULL, 16 } };
    CHECK(weft_publish_batch(&f, bad, 1) == 0, "FB5 NULL src with len refused");
    CHECK(weft_publish_batch(&f, NULL, 4) == 0, "FB5 NULL frames refused");
    CHECK(weft_publish_batch(&f, fr, 0) == 0, "FB5 n == 0 is a detectable no-op");
    weft_fanout_destroy(&f);
}

// ---------------------------------------------------------------------------
// FB6 — commit-only API
// ---------------------------------------------------------------------------

static void test_fb6(void) {
    enum { WORDS = 64, M = 8 };
    weft_fanout_t f;
    weft_fanout_reader_t r;
    if (weft_fanout_init(&f, WORDS * 4, M) != 0) { CHECK(false, "FB6 init"); return; }
    weft_fanout_reader_init(&r, weft_fanout_ring(&f),
                            weft_fanout_ring_bytes(WORDS * 4, M), WORDS * 4, M);
    uint32_t buf[WORDS];
    const size_t n = 5;
    for (size_t i = 0; i < n; i++) {
        fill_words(buf, (uint32_t)(i + 1), WORDS);
        (void)weft_fanout_begin(&f);
        (void)weft_fanout_fill(&f, buf, WORDS * 4);
    }
    const uint64_t got = weft_publish_batch_commit(&f, n);
    CHECK(got == n, "FB6 commit publishes the 5 begun frames");
    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    CHECK(c->fresh && c->seq == n && c->dropped == n - 1,
          "FB6 reader view identical to the batch API");
    const uint32_t* v = (const uint32_t*)weft_fanout_view(&r);
    CHECK(v[0] == tword(n, 0), "FB6 tail frame bytes exact");
    CHECK(weft_publish_batch_commit(&f, 99) == 0, "FB6 commit > w_seq refused");
    weft_fanout_reader_destroy(&r);
    weft_fanout_destroy(&f);
}

// ---------------------------------------------------------------------------
// --bench — throughput A/B (issue acceptance: 2x for 100-frame batches)
// ---------------------------------------------------------------------------

typedef struct {
    weft_fanout_t* fan;
    _Atomic int stop;
    _Atomic uint64_t polls;
    // claiming-leg state (the display-thread pattern: claim + consume)
    weft_fanout_reader_t* reader;
    _Atomic uint64_t claims;
} fb_poller_t;

static void* fb_poller(void* arg) {
    fb_poller_t* p = (fb_poller_t*)arg;
    uint64_t n = 0, c = 0;
    while (!atomic_load_explicit(&p->stop, memory_order_relaxed)) {
        // The display-thread poll: advisory latestSeq read at cadence.
        volatile uint64_t L = atomic_load_explicit(p->fan->ctrl, memory_order_relaxed);
        (void)L;
        n++;
        for (int i = 0; i < 50; i++) { /* cadence spacing */ }
    }
    atomic_store(&p->polls, n);
    atomic_store(&p->claims, c);
    return NULL;
}

/// The claiming leg: a display-style consumer running claims + payload
/// consumption in a loop — the realistic contention pattern (its claims
/// acquire-load latestSeq, copy slots, and evict the writer's cache state;
/// the raw-poll leg above only observes the ctrl line).
static volatile uint64_t g_sink_acc;  // claimer consumption sink (DCE guard)

static void* fb_claimer(void* arg) {
    fb_poller_t* p = (fb_poller_t*)arg;
    uint64_t c = 0;
    uint64_t acc = 0;
    while (!atomic_load_explicit(&p->stop, memory_order_relaxed)) {
        const weft_fanout_claim_t* cl = weft_fanout_claim(p->reader);
        if (cl->fresh) {
            const uint32_t* v = (const uint32_t*)weft_fanout_view(p->reader);
            acc += v[0] + v[(p->reader->payload_bytes / 4) - 1];
            c++;
        }
    }
    g_sink_acc = acc;
    atomic_store(&p->claims, c);
    return NULL;
}

static void run_bench(void) {
    enum { WORDS = 256, M = 8, BATCH = 100 };
    static const char* kLegs[] = { "idle", "reader-polling", "reader-claiming" };
    for (int leg = 0; leg < 3; leg++) {
        weft_fanout_t f;
        if (weft_fanout_init(&f, WORDS * 4, M) != 0) exit(2);
        fb_poller_t poller;
        memset(&poller, 0, sizeof(poller));
        poller.fan = &f;
        weft_fanout_reader_t claim_reader;
        if (leg == 2) {
            weft_fanout_reader_init(&claim_reader, weft_fanout_ring(&f),
                                    weft_fanout_ring_bytes(WORDS * 4, M),
                                    WORDS * 4, M);
            poller.reader = &claim_reader;
        }
        pthread_t tp = 0;
        if (leg >= 1) {
            atomic_store(&poller.stop, 0);
            pthread_create(&tp, NULL, leg == 2 ? fb_claimer : fb_poller, &poller);
        }
        uint32_t* block = malloc(BATCH * WORDS * 4);
        weft_batch_frame_t* fr = malloc(BATCH * sizeof(weft_batch_frame_t));
        for (size_t i = 0; i < BATCH; i++) {
            fr[i].src = block + i * WORDS;
            fr[i].len = WORDS * 4;
        }
        const uint64_t target_ns = 1000000000ull;  // 1 s per cell
        uint64_t frames_single = 0, frames_batch = 0;
        double single_ns = 0, batch_ns = 0;
        uint64_t seq = 0;

        // warmup both
        for (int i = 0; i < 20000; i++) {
            fill_words(block + (i % BATCH) * WORDS, (uint32_t)(seq + 1), WORDS);
            (void)weft_fanout_begin(&f);
            (void)weft_fanout_fill(&f, block + (i % BATCH) * WORDS, WORDS * 4);
            seq = weft_fanout_publish(&f);
        }
        for (int i = 0; i < 200; i++) {
            for (size_t j = 0; j < BATCH; j++)
                fill_words(block + j * WORDS, (uint32_t)(seq + j + 1), WORDS);
            seq = weft_publish_batch(&f, fr, BATCH);
        }

        // A: single-frame publishes
        {
            uint64_t t0 = now_ns(), t1 = t0;
            while ((t1 = now_ns()) - t0 < target_ns) {
                for (int i = 0; i < 200; i++) {
                    fill_words(block, (uint32_t)(seq + 1), WORDS);
                    (void)weft_fanout_begin(&f);
                    (void)weft_fanout_fill(&f, block, WORDS * 4);
                    seq = weft_fanout_publish(&f);
                    frames_single++;
                }
            }
            single_ns = (double)(t1 - t0);
        }
        // B: batches
        {
            uint64_t t0 = now_ns(), t1 = t0;
            while ((t1 = now_ns()) - t0 < target_ns) {
                for (int i = 0; i < 2; i++) {
                    for (size_t j = 0; j < BATCH; j++)
                        fill_words(block + j * WORDS, (uint32_t)(seq + j + 1), WORDS);
                    seq = weft_publish_batch(&f, fr, BATCH);
                    frames_batch += BATCH;
                }
            }
            batch_ns = (double)(t1 - t0);
        }
        if (leg >= 1) {
            atomic_store(&poller.stop, 1);
            pthread_join(tp, NULL);
        }
        if (leg == 2) weft_fanout_reader_destroy(&claim_reader);
        const double fps_single = frames_single / (single_ns / 1e9);
        const double fps_batch = frames_batch / (batch_ns / 1e9);
        printf("{\"bench\":\"fanout-batch\",\"lang\":\"c\",\"leg\":\"%s\","
               "\"batch_n\":%d,\"payload_bytes\":%u,\"slots\":%u,"
               "\"single_frames_per_s\":%.0f,\"batch_frames_per_s\":%.0f,"
               "\"speedup\":%.3f,\"copy_impl\":\"%s\","
               "\"polls\":%llu}\n",
               kLegs[leg], BATCH, WORDS * 4, M,
               fps_single, fps_batch, fps_batch / fps_single,
               weft_fanout_copy_active_impl(),
               leg >= 1 ? (unsigned long long)(atomic_load(&poller.polls) + atomic_load(&poller.claims)) : 0ull);
        free(block); free(fr);
        weft_fanout_destroy(&f);
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    double torture_s = 1.0;
    int bench = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bench") == 0) bench = 1;
        else if (strncmp(argv[i], "torture_s=", 10) == 0)
            torture_s = atof(argv[i] + 10);
    }
    if (bench) { run_bench(); return g_failures ? 1 : 0; }
    test_fb1();
    test_fb2();
    test_fb3();
    test_fb4(torture_s);
    test_fb5();
    test_fb6();
    if (g_failures == 0) {
        fprintf(stdout, "FB-SERIES PASS\n");
        return 0;
    }
    fprintf(stderr, "FB-SERIES FAIL (%d)\n", g_failures);
    return 1;
}
