// reclaim_test.c — TIER4 §4 bounded-revocation conformance (R-series), C.
//
// Issue #19 (Tier 4: Security & Hardening), task 4 — Revocation Timeout Bounds.
// Proves weft_reclaim ALWAYS returns within its effective bound and that the
// bound is runtime-configurable:
//
//   R1  Default ceiling — a fresh instance's max_reclaim_timeout_ms is 1000.
//   R2  Clamp — a caller bound above the ceiling returns TIMEOUT in ~ceiling
//       ms, NOT ~caller ms (measured, not assumed; tolerance = ceiling + 250 ms
//       poll/jitter slack).
//   R3  ACK — a live writer ACKs within one publish: reclaim returns ACK
//       well inside the bound (legacy rc == 0 contract preserved).
//   R4  Timeout telemetry — every TIMEOUT counts exactly one
//       t_reclaim_timeouts; ACK does not count.
//   R5  Ceiling 0 disables the ceiling — the caller's bound rules (measured).
//   R6  Setter — weft_set_max_reclaim_timeout takes effect on the next call.
//   R7  NULL w — WEFT_RECLAIM_INVALID, no crash.
//
// Modes:
//   ./reclaim-test       — run R1–R7, emit one JSON verdict line
//   exit 0 iff all gates pass

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>

#include "weft.h"

static int fails = 0;
static long checks = 0;

static void expect(int cond, const char* what) {
    checks++;
    if (!cond) {
        fails++;
        fprintf(stderr, "GATE FAIL: %s\n", what);
    }
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

// A writer that publishes FOREVER until stopped — used as a stuck writer when
// the test wants reclaim to time out (the writer never observes `revoked`
// because it is already revoked... it ACKs on the FIRST publish after revoke).
// For a genuinely stuck writer we instead simply NEVER call publish: epoch
// stays frozen at pre_revoke_epoch, so reclaim must time out.
//
// Writer thread: publishes until args->stop, ACKing once after revoke.
typedef struct {
    weft_t* w;
    _Atomic bool stop;
} writer_args_t;

static void* writer_fn(void* p) {
    writer_args_t* a = (writer_args_t*)p;
    uint32_t seq = 0;
    while (!atomic_load(&a->stop)) {
        weft_publish(a->w, ++seq, 0);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 };  // 200 µs
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int main(void) {
    printf("reclaim-test: TIER4 §4 bounded-revocation conformance\n");

    // ---- R1: default ceiling ----
    weft_t w;
    if (weft_init(&w, 64) != 0) { fprintf(stderr, "init failed\n"); return 1; }
    expect(weft_max_reclaim_timeout(&w) == 1000u, "R1 default ceiling is 1000 ms");

    // ---- R3: ACK within the bound (writer alive) ----
    writer_args_t args = { .w = &w, .stop = false };
    pthread_t wt;
    pthread_create(&wt, NULL, writer_fn, &args);
    struct timespec ts5 = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };  // 2 ms
    nanosleep(&ts5, NULL);

    uint32_t e0 = weft_epoch(&w);
    weft_revoke(&w);
    uint64_t t0 = now_ms();
    weft_reclaim_result_t rc = weft_reclaim(&w, e0, 500);
    uint64_t dt = now_ms() - t0;
    expect(rc == WEFT_RECLAIM_ACK, "R3 live writer ACKs");
    expect(dt < 500, "R3 ACK inside the caller bound");
    expect(weft_t_reclaim_timeouts(&w) == 0, "R4 ACK does not count as timeout");
    atomic_store(&args.stop, true);
    pthread_join(wt, NULL);

    // ---- R2: clamp — stuck writer, caller bound above the ceiling ----
    weft_t w2;
    if (weft_init(&w2, 64) != 0) { fprintf(stderr, "init failed\n"); return 1; }
    weft_revoke(&w2);   // writer never publishes -> never ACKs
    t0 = now_ms();
    rc = weft_reclaim(&w2, weft_epoch(&w2), 5000);  // above the 1000 ceiling
    dt = now_ms() - t0;
    expect(rc == WEFT_RECLAIM_TIMEOUT, "R2 stuck writer returns TIMEOUT");
    expect(dt < 1000 + 250, "R2 clamp to the ceiling (1000 ms, not 5000)");
    expect(weft_t_reclaim_timeouts(&w2) == 1, "R4 TIMEOUT counts exactly once");

    // ---- R6: setter takes effect ----
    weft_set_max_reclaim_timeout(&w2, 200);
    expect(weft_max_reclaim_timeout(&w2) == 200u, "R6 setter visible");
    t0 = now_ms();
    rc = weft_reclaim(&w2, weft_epoch(&w2), 5000);
    dt = now_ms() - t0;
    expect(rc == WEFT_RECLAIM_TIMEOUT, "R6 still TIMEOUT");
    expect(dt < 200 + 250, "R6 new ceiling enforced (200 ms, not 1000)");

    // ---- R5: ceiling 0 disables the clamp; the caller bound rules ----
    weft_set_max_reclaim_timeout(&w2, 0);
    expect(weft_max_reclaim_timeout(&w2) == 0u, "R5 ceiling 0 settable");
    t0 = now_ms();
    rc = weft_reclaim(&w2, weft_epoch(&w2), 300);
    dt = now_ms() - t0;
    expect(rc == WEFT_RECLAIM_TIMEOUT, "R5 stuck writer still TIMEOUT");
    expect(dt < 300 + 250, "R5 caller bound rules when ceiling is 0");
    expect(weft_t_reclaim_timeouts(&w2) == 3, "R4 timeout count accumulates");

    // ---- R7: NULL w ----
    expect(weft_reclaim(NULL, 0, 100) == WEFT_RECLAIM_INVALID, "R7 NULL w -> INVALID");

    weft_destroy(&w);
    weft_destroy(&w2);

    printf("{\"test\":\"reclaim\",\"checks\":%ld,\"fails\":%d,\"status\":\"%s\"}\n",
           checks, fails, fails == 0 ? "PASSED" : "FAILED");
    return fails == 0 ? 0 : 1;
}
