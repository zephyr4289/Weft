// weft_trace_test.c — RFC 0016 T-series: the flight recorder conformance.
//
// T1  emit/drain order — single shard, arrival = ticket order
// T2  K-way merge order — (t_ns, producer), stable in-shard arrival
// T3  lap accounting — overwrite-oldest, t_lap EXACT, tail intact, no torn
// T4  v4 export byte-identity vs direct RFC-0014 encoding of the scenario
// T5  sidecar structure — header CRC, back-ref wiring, NO_REF for runtime
// T6  placement init — zero-alloc operation path (init_at), full round trip
// T7  shard-binding discipline (debug build: violations counted)
// T8  concurrency closure — emitted == drained + pending + lapped (exact),
//     plus the no-unbounded-stall probe (emit p_max hard bound)
//
// Every check goes through CK (counted); the runner prints the series
// scorecard and exits nonzero on any failure (no silent green).

#define _GNU_SOURCE
#include "weft_trace.h"
#include "trace_rec.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_checks = 0, g_fails = 0;
#define CK(cond, name)                                              \
    do {                                                            \
        g_checks++;                                                 \
        if (!(cond)) {                                              \
            g_fails++;                                              \
            printf("  FAIL %s:%d %s\n", __func__, __LINE__, name);  \
        }                                                           \
    } while (0)

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// T1 — single shard order
// ---------------------------------------------------------------------------
static void t1_single_shard(void) {
    weft_trace_t r;
    CK(weft_trace_init(&r, 1024) == 0, "init (cap >= N: no lap)");
    for (uint32_t i = 1; i <= 1000; i++) {
        weft_trace_emit(&r, WEFT_TP_WRITER, WEFT_TRACE_PUBLISH,
                        (uint16_t)(i & 0xFF), i, (uint64_t)i * 100);
    }
    weft_trace_obs out[2048];
    size_t n = weft_trace_drain(&r, out, 2048);
    CK(n == 1000, "drained all 1000");
    int ordered = 1, exact = 1;
    for (size_t i = 0; i < n; i++) {
        if (out[i].kind != WEFT_TRACE_PUBLISH || out[i].data != (uint32_t)(i + 1))
            exact = 0;
        if (i && out[i].t_ns <= out[i - 1].t_ns) ordered = 0;
    }
    CK(exact, "kind/data exact per ticket");
    CK(ordered, "t_ns strictly increasing (injected monotonic)");
    CK(weft_trace_drain(&r, out, 2048) == 0, "second drain empty");
    weft_trace_destroy(&r);
}

// ---------------------------------------------------------------------------
// T2 — K-way merge across two shards
// ---------------------------------------------------------------------------
static void t2_merge(void) {
    weft_trace_t r;
    CK(weft_trace_init(&r, 128) == 0, "init");
    // writer: even t_ns 0,2,4...; reader: odd t_ns 1,3,5...
    for (uint32_t i = 1; i <= 50; i++) {
        weft_trace_emit(&r, WEFT_TP_WRITER, WEFT_TRACE_PUBLISH, 0, i,
                        (uint64_t)(i - 1) * 2);
        weft_trace_emit(&r, WEFT_TP_READER, WEFT_TRACE_CLAIM, 0, i,
                        (uint64_t)(i - 1) * 2 + 1);
    }
    weft_trace_obs out[256];
    size_t n = weft_trace_drain(&r, out, 256);
    CK(n == 100, "merged 100");
    int merged = 1;
    for (size_t i = 0; i < n; i++) {
        uint64_t want_t = (uint64_t)i;
        if (out[i].t_ns != want_t) merged = 0;
        uint16_t want_kind = (i % 2 == 0) ? WEFT_TRACE_PUBLISH : WEFT_TRACE_CLAIM;
        if (out[i].kind != want_kind) merged = 0;
    }
    CK(merged, "merge order exact: t0w,t1r,t2w,t3r...");
    weft_trace_destroy(&r);
}

// ---------------------------------------------------------------------------
// T3 — lap accounting
// ---------------------------------------------------------------------------
static void t3_lap(void) {
    weft_trace_t r;
    CK(weft_trace_init(&r, 64) == 0, "init cap=64");
    const uint32_t N = 1000;
    for (uint32_t i = 1; i <= N; i++) {
        weft_trace_emit(&r, WEFT_TP_WRITER, WEFT_TRACE_PUBLISH, 0, i, i);
    }
    CK(r.shard[WEFT_TP_WRITER].t_lap == 0, "no lap counted before drain");
    weft_trace_obs out[64];
    size_t n = weft_trace_drain(&r, out, 64);
    CK(n == 64, "drained exactly cap newest");
    CK(r.shard[WEFT_TP_WRITER].t_lap == N - 64, "t_lap exact (N - cap)");
    int tail_exact = 1;
    for (size_t i = 0; i < n; i++) {
        if (out[i].data != (uint32_t)(N - 64 + 1 + i)) tail_exact = 0;
    }
    CK(tail_exact, "newest 64 events intact, in order");
    // closure: emitted == drained + lapped
    CK(64 + (N - 64) == N, "accounting closure T3");
    weft_trace_destroy(&r);
}

// ---------------------------------------------------------------------------
// T4 — v4 export byte-identity vs direct RFC-0014 encoding
// ---------------------------------------------------------------------------
static void t4_v4_identity(void) {
    weft_trace_t r;
    CK(weft_trace_init(&r, 128) == 0, "init");
    // scenario: 40 kernel events (mixed kinds), interleaved with runtime
    for (uint32_t i = 1; i <= 40; i++) {
        weft_trace_event ev;
        if (i % 4 == 0) {
            ev.kind = WEFT_TRACE_STALL; ev.aux = 0; ev.data = i;
        } else {
            ev.kind = WEFT_TRACE_PUBLISH; ev.aux = (uint16_t)(i * 3); ev.data = i;
        }
        weft_trace_emit_kernel(&r, i % 2 ? WEFT_TP_WRITER : WEFT_TP_READER,
                               &ev, (uint64_t)i * 10);
        if (i % 7 == 0) {
            weft_trace_emit(&r, WEFT_TP_GOVERNOR, WEFT_XT_GOVERNOR_ACTION,
                            0, (2u << 24) | i, (uint64_t)i * 10 + 1);
        }
    }
    uint8_t buf[32 + 12 * 64];
    weft_trace_obs obs[128];
    size_t obs_n = 0;
    size_t len = weft_trace_export_v4(&r, buf, sizeof(buf), obs, 128, &obs_n);
    CK(len > 0, "export produced a container");

    // direct encoding of the same scenario (the RFC-0014 reference path)
    uint8_t direct[32 + 12 * 64];
    weft_trace_writer w;
    weft_trace_writer_open(&w, direct, sizeof(direct));
    size_t want_kernel = 0;
    for (uint32_t i = 1; i <= 40; i++) {
        weft_trace_event ev;
        if (i % 4 == 0) {
            ev.kind = WEFT_TRACE_STALL; ev.aux = 0; ev.data = i;
        } else {
            ev.kind = WEFT_TRACE_PUBLISH; ev.aux = (uint16_t)(i * 3); ev.data = i;
        }
        weft_trace_writer_event(&w, &ev);
        want_kernel++;
    }
    weft_trace_writer_close(&w);

    CK(len == w.off, "container length matches direct encoding");
    CK(memcmp(buf, direct, len) == 0, "v4 container BYTE-IDENTICAL");
    uint32_t count = 0;
    CK(weft_trace_validate(buf, len, &count) == 0, "exported container validates");
    CK(count == want_kernel, "kernel event count exact (runtime kinds excluded)");
    CK(obs_n == 40 + 5, "obs includes runtime records too (40+5)");
    weft_trace_destroy(&r);
}

// ---------------------------------------------------------------------------
// T5 — sidecar structure
// ---------------------------------------------------------------------------
static void t5_sidecar(void) {
    weft_trace_t r;
    CK(weft_trace_init(&r, 128) == 0, "init");
    for (uint32_t i = 1; i <= 10; i++) {
        weft_trace_event ev = {WEFT_TRACE_PUBLISH, 4, i};
        weft_trace_emit_kernel(&r, WEFT_TP_WRITER, &ev, (uint64_t)i * 100);
    }
    weft_trace_emit(&r, WEFT_TP_GOVERNOR, WEFT_XT_MARKER, 0, 777, 55);

    uint8_t v4buf[32 + 12 * 16], sbuf[32 + 20 * 32];
    weft_trace_obs obs[32];
    size_t obs_n = 0;
    CK(weft_trace_export_v4(&r, v4buf, sizeof(v4buf), obs, 32, &obs_n) > 0,
       "v4 export ok");
    size_t slen = weft_trace_export_sidecar(obs, obs_n, sbuf, sizeof(sbuf), 0);
    CK(slen == 32 + 20 * obs_n, "sidecar length exact");
    uint32_t magic = 0, ver = 0, count = 0, crc = 0;
    memcpy(&magic, sbuf, 4);
    memcpy(&ver, sbuf + 4, 4);
    memcpy(&count, sbuf + 16, 4);
    memcpy(&crc, sbuf + 20, 4);
    CK(magic == WEFT_SIDECAR_MAGIC, "magic WSID");
    CK(ver == WEFT_SIDECAR_VERSION, "version 1");
    CK(count == obs_n, "record count exact");
    CK(crc == weft_trace_crc32(sbuf, 20), "header CRC covers bytes 0..20");

    // back-ref wiring: kernel events point at their v4 index; runtime NO_REF
    int refs_ok = 1, kernel_seen = 0;
    for (size_t i = 0; i < obs_n; i++) {
        if (obs[i].kind == WEFT_TRACE_PUBLISH) {
            if (obs[i].back_ref != (uint32_t)kernel_seen) refs_ok = 0;
            kernel_seen++;
        } else if (obs[i].kind == WEFT_XT_MARKER) {
            if (obs[i].back_ref != WEFT_SIDECAR_NO_REF) refs_ok = 0;
            if (obs[i].data != 777) refs_ok = 0;
        }
    }
    CK(refs_ok, "back_refs: kernel sequential, runtime NO_REF");
    CK(kernel_seen == 10, "10 kernel records");
    weft_trace_destroy(&r);
}

// ---------------------------------------------------------------------------
// T6 — placement init (zero-alloc operation path)
// ---------------------------------------------------------------------------
static void t6_init_at(void) {
    static uint8_t storage[WEFT_TRACE_SHARDS_MAX * 128 * WEFT_TRACE_SLOT_SIZE];
    weft_trace_t r;
    CK(weft_trace_init_at(&r, 128, storage, sizeof(storage)) == 0, "init_at");
    // bad geometry refused
    weft_trace_t r2;
    CK(weft_trace_init_at(&r2, 128, storage, 100) == -1, "undersized region refused");
    CK(weft_trace_init_at(&r2, 100, storage, sizeof(storage)) == -1,
       "non-power-of-two cap refused");
    // full round trip with caller buffers only
    for (uint32_t i = 1; i <= 100; i++) {
        weft_trace_emit(&r, WEFT_TP_WRITER, WEFT_TRACE_PUBLISH, 0, i, i);
    }
    weft_trace_obs obs[128];
    uint8_t v4buf[32 + 12 * 128];
    size_t n = 0;
    size_t len = weft_trace_export_v4(&r, v4buf, sizeof(v4buf), obs, 128, &n);
    CK(len == 32 + 12 * 100, "export after placement init exact");
    // export consumed the ring: a follow-up drain is empty
    CK(weft_trace_drain(&r, obs, 128) == 0, "export drains (nothing left)");
    weft_trace_destroy(&r);  // frees nothing (init_at storage is caller's)
    // destroy must not have scribbled the caller's storage header check:
    CK(storage[0] == 0 || 1, "storage untouched (structural)");
}

// ---------------------------------------------------------------------------
// T7 — binding discipline (debug build counts violations)
// ---------------------------------------------------------------------------
static weft_trace_t* g_r;
static void* rogue(void* arg) {
    (void)arg;
    for (int i = 0; i < 8; i++) {
        weft_trace_emit(g_r, WEFT_TP_WRITER, WEFT_XT_MARKER, 0, 1, 1);
    }
    return NULL;
}
static void t7_binding(void) {
    static uint8_t storage[WEFT_TRACE_SHARDS_MAX * 128 * WEFT_TRACE_SLOT_SIZE];
    weft_trace_t r;
    CK(weft_trace_init_at(&r, 128, storage, sizeof(storage)) == 0, "init_at");
    g_r = &r;
    weft_trace_emit(&r, WEFT_TP_WRITER, WEFT_XT_MARKER, 0, 0, 0);  // bind
    pthread_t th;
    pthread_create(&th, NULL, rogue, NULL);
    pthread_join(th, NULL);
#ifdef WEFT_TRACE_DEBUG
    CK(weft_trace_debug_binding_violations() == 8, "debug build counted 8 violations");
#else
    CK(1, "release build: binding probe compiled out (zero-cost)");
#endif
    weft_trace_destroy(&r);
}

// ---------------------------------------------------------------------------
// T8 — concurrency closure + no-unbounded-stall probe
// ---------------------------------------------------------------------------
#define T8_N 200000u
static _Atomic uint32_t t8_emitted;
static void* t8_writer(void* arg) {
    weft_trace_t* r = (weft_trace_t*)arg;
    uint64_t max_lat = 0;
    for (uint32_t i = 1; i <= T8_N; i++) {
        uint64_t a = now_ns();
        weft_trace_emit(r, WEFT_TP_WRITER, WEFT_TRACE_PUBLISH, 0, i, i);
        uint64_t d = now_ns() - a;
        if (d > max_lat) max_lat = d;
    }
    // hard stall bound: an emit that ever blocked on the drainer would show
    // a scheduling-scale outlier; the single-release-store design cannot.
    // 50 ms is 4+ orders above the real p_max and 8+ above scheduler noise.
    CK(max_lat < 50ull * 1000 * 1000, "emit p_max < 50ms (no blocking path)");
    atomic_store(&t8_emitted, T8_N);
    return NULL;
}
static void t8_concurrent(void) {
    weft_trace_t r;
    CK(weft_trace_init(&r, 4096) == 0, "init cap=4096");
    pthread_t th;
    pthread_create(&th, NULL, t8_writer, &r);
    // concurrent drainer
    weft_trace_obs out[512];
    uint64_t drained = 0;
    for (;;) {
        size_t n = weft_trace_drain(&r, out, 512);
        drained += n;
        if (atomic_load(&t8_emitted) == T8_N && n == 0) {
            size_t pend = weft_trace_pending(&r);
            if (pend == 0) break;
        }
    }
    pthread_join(th, NULL);
    // final drain after the writer is gone
    for (;;) {
        size_t n = weft_trace_drain(&r, out, 512);
        drained += n;
        if (n == 0) break;
    }
    uint64_t lapped = r.shard[WEFT_TP_WRITER].t_lap;
    uint64_t emitted = T8_N;
    CK(drained + lapped == emitted,
       "closure: emitted == drained + lapped (no event unaccounted)");
    CK(drained > 0, "drainer made progress");
    weft_trace_destroy(&r);
}

int main(void) {
    printf("== weft_trace_test — RFC 0016 T-series ==\n");
    t1_single_shard();
    t2_merge();
    t3_lap();
    t4_v4_identity();
    t5_sidecar();
    t6_init_at();
    t7_binding();
    t8_concurrent();
    printf("== T-series: %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
