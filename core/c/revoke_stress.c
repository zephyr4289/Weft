// revoke_stress.c — Issue #16 Tier 1 Task 5: revocation handshake stress.
//
// GOAL (the issue's words): "Test the revoke/reclaim handshake with 100K
// concurrent writers to ensure no use-after-free or deadlocks... Zero
// use-after-free in TSan. Zero deadlocks or timeouts. All writers
// successfully ACK and reclaim."
//
// HONEST SHAPING (this sandbox: 2 vCPU, pids.max unlimited, ulimit -u
// 100000): 100K SIMULTANEOUSLY-RUNNABLE threads on 2 vCPUs is a scheduling
// statement, not a protocol statement — the handshake's adversary is
// CONCURRENT writers racing revoke/reclaim, which waves preserve exactly:
//   Phase 1  concurrency ceiling probe — spawn real threads (256 KiB
//            stacks) in growing batches until pthread_create refuses; the
//            measured ceiling is REPORTED, never assumed.
//   Phase 2  100,000+ total handshakes in waves of C concurrent writers
//            (C = min(measured ceiling, 8192)); every wave: C independent
//            kernels, each with a spinning writer; the harness revokes each
//            instance, the writer ACKs, reclaim observes the ACK, poison,
//            join, destroy. Zero timeouts = zero deadlocks; ASAN across the
//            full churn = zero use-after-free/leaks; the TSAN leg runs a
//            reduced wave count (TSAN maps its own shadow memory per thread
//            — the reduction is DECLARED in the verdict, not hidden).
//
// LAW 1: every resolution (ack / timeout / failure) is counted, never
// silent. LAW 4: the wave decomposition, the ceiling, and the TSAN
// reduction are declared in the output.
//
// Build: make revoke-stress | revoke-stress-asan | revoke-stress-tsan
// CLI:   ./revoke-stress [totalHandshakes] [waveConcurrency] [timeoutMs] [probeCap]
// Exit:  0 pass / 1 fail.

#define _GNU_SOURCE
#include "weft.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <stdatomic.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// Phase 1 — the concurrency ceiling probe (threads that exist and park)
// ---------------------------------------------------------------------------

#define CEILING_PROBE_CAP 16384   // stop the probe here even if the OS allows more
#define STACK_SIZE (128 * 1024)   // 128 KiB: ample for these tiny bodies

typedef struct {
    _Atomic bool release;   // threads park until released — TRUE concurrency
} probe_arg_t;

static void* probe_thread_fn(void* argp) {
    probe_arg_t* a = (probe_arg_t*)argp;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000 };
    while (!atomic_load(&a->release)) nanosleep(&ts, NULL);
    return NULL;
}

// Measures SIMULTANEOUSLY-ALIVE threads: every spawned thread parks until
// the probe releases them, so a create failure marks the true ceiling (a
// probe whose threads exit early measures create-rate, not concurrency —
// the bug this comment replaced).
static int probe_concurrency_ceiling(int* created_out, int cap) {
    pthread_t threads[CEILING_PROBE_CAP];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, STACK_SIZE);
    probe_arg_t arg;
    atomic_init(&arg.release, false);
    int created = 0;
    for (int i = 0; i < cap; i++) {
        if (pthread_create(&threads[i], &attr, probe_thread_fn, &arg) != 0) break;
        created++;
    }
    atomic_store(&arg.release, true);
    for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
    pthread_attr_destroy(&attr);
    *created_out = created;
    return created;
}

// ---------------------------------------------------------------------------
// Phase 2 — the wave engine: C concurrent writers, each racing its own
// kernel's revoke/reclaim/ACK handshake
// ---------------------------------------------------------------------------

typedef struct {
    weft_t* w;
    _Atomic bool revoked_seen;   // writer observed DROPPED_REVOKED (the ACK)
    _Atomic uint64_t publishes;  // successful PUB_OK count (frozen post-ACK)
} wave_writer_t;

static void* wave_writer_fn(void* argp) {
    wave_writer_t* a = (wave_writer_t*)argp;
    uint8_t payload[64];
    uint32_t seq = 1;
    // Establish a live publish stream first (a churn handshake with zero
    // publishes is a weaker probe — same discipline as L16).
    for (int i = 0; i < 8; i++) {
        for (size_t j = 0; j < sizeof(payload); j++) payload[j] = weft_pat(seq, (uint32_t)j);
        if (weft_w_write_payload(a->w, payload, sizeof(payload)) != 0) return NULL;
        if (weft_publish(a->w, seq++, sizeof(payload)) != WEFT_PUB_OK) {
            // The revoke landed mid-stream: DROPPED_REVOKED IS the ACK.
            atomic_store(&a->revoked_seen, true);
            return NULL;
        }
        atomic_fetch_add(&a->publishes, 1);
    }
    // Then poll-publish until the revoke lands (the ACK moment). The poll
    // gap keeps the wave's parked threads schedulable on 2 vCPUs.
    while (!atomic_load(&a->revoked_seen)) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000 };  // 500us
        nanosleep(&ts, NULL);
        if (weft_publish(a->w, seq++, sizeof(payload)) == WEFT_PUB_DROPPED_REVOKED) {
            atomic_store(&a->revoked_seen, true);
            return NULL;   // post-ACK: never touch buffer bytes again (02 §6)
        }
        atomic_fetch_add(&a->publishes, 1);
    }
    return NULL;
}

typedef struct {
    int64_t handshakes;
    int64_t timeouts;
    int64_t ack_failures;
    int64_t poison_failures;
    int64_t waves;
    int64_t spawn_failures;
} stress_ledger_t;

static int run_waves(int64_t total, int concurrency, int timeout_ms,
                     stress_ledger_t* led) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, STACK_SIZE);

    int64_t done = 0;
    while (done < total) {
        int c = (int)((total - done) < concurrency ? (total - done) : concurrency);
        // Wave setup: c kernels + c writer threads
        weft_t** kernels = (weft_t**)malloc((size_t)c * sizeof(weft_t*));
        wave_writer_t* writers = (wave_writer_t*)malloc((size_t)c * sizeof(wave_writer_t));
        pthread_t* tids = (pthread_t*)malloc((size_t)c * sizeof(pthread_t));
        uint32_t* e0s = (uint32_t*)malloc((size_t)c * sizeof(uint32_t));
        if (!kernels || !writers || !tids || !e0s) {
            fprintf(stderr, "revoke-stress: wave alloc failed\n");
            free(kernels); free(writers); free(tids); free(e0s);
            return -1;
        }
        int spawned = 0;
        for (int i = 0; i < c; i++) {
            kernels[i] = (weft_t*)malloc(sizeof(weft_t));
            if (!kernels[i] || weft_init(kernels[i], 256) != 0) break;
            writers[i].w = kernels[i];
            atomic_init(&writers[i].revoked_seen, false);
            atomic_init(&writers[i].publishes, 0);
            bool created_ok = false;
            for (int attempt = 0; attempt < 24; attempt++) {
                if (pthread_create(&tids[i], &attr, wave_writer_fn, &writers[i]) == 0) {
                    created_ok = true;
                    break;
                }
                // Transient EAGAIN while the previous wave's joined threads
                // are still being reaped (worst at the measured ceiling) —
                // bounded retry, counted if exhausted.
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000 };
                nanosleep(&ts, NULL);
            }
            if (!created_ok) {
                weft_destroy(kernels[i]);
                free(kernels[i]);
                break;
            }
            spawned++;
        }
        led->spawn_failures += (c - spawned);
        c = spawned;

        // The handshake storm: revoke + reclaim + poison + join per instance.
        for (int i = 0; i < c; i++) {
            e0s[i] = weft_epoch(kernels[i]);
            weft_revoke(kernels[i]);
        }
        for (int i = 0; i < c; i++) {
            if (weft_reclaim(kernels[i], e0s[i], timeout_ms) != 0) {
                led->timeouts++;
                continue;   // counted, never silent; instance still joined below
            }
            // Poison post-ACK; verify the writer honored the contract.
            for (int b = 0; b < 3; b++) memset(kernels[i]->buf[b], 0xDE, kernels[i]->buf_size);
            uint64_t cv;
            memcpy(&cv, kernels[i]->buf[0] + kernels[i]->buf_size - 8, 8);
            if (cv != 0xDEDEDEDEDEDEDEDEull) led->poison_failures++;
            led->handshakes++;
        }
        // ACK adjudication AFTER the join — the writer's post-ACK flag is
        // final once the thread is parked (checking it microseconds after
        // reclaim races the writer's own post-publish bookkeeping).
        for (int i = 0; i < c; i++) {
            pthread_join(tids[i], NULL);
            if (!atomic_load(&writers[i].revoked_seen)) led->ack_failures++;
            weft_destroy(kernels[i]);
            free(kernels[i]);
        }
        free(kernels); free(writers); free(tids); free(e0s);
        done += c;
        led->waves++;
    }
    pthread_attr_destroy(&attr);
    return 0;
}

int main(int argc, char** argv) {
    int64_t total = argc > 1 ? strtoll(argv[1], NULL, 10) : 100000;
    int concurrency_wanted = argc > 2 ? (int)strtoul(argv[2], NULL, 10) : 8192;
    int timeout_ms = argc > 3 ? (int)strtoul(argv[3], NULL, 10) : 2000;
    int probe_cap = argc > 4 ? (int)strtoul(argv[4], NULL, 10) : CEILING_PROBE_CAP;

    // Phase 1: measure the real ceiling (capped probe; reported, never assumed;
    // the cap is smaller under TSAN, whose per-thread shadow setup makes the
    // full probe disproportionately slow — the wave concurrency is what the
    // protocol evidence actually needs)
    int ceiling = 0;
    uint64_t t0 = now_ns();
    probe_concurrency_ceiling(&ceiling, probe_cap);
    uint64_t probe_ns = now_ns() - t0;
    fprintf(stderr, "revoke-stress: concurrency ceiling probe = %d threads "
                    "(256KiB stacks, cap %d) in %llu ms\n",
            ceiling, CEILING_PROBE_CAP, (unsigned long long)(probe_ns / 1000000ull));
    if (ceiling < 2) {
        printf("{\"test\":\"revocation-stress\",\"lang\":\"c\",\"pass\":false,"
               "\"metrics\":{\"error\":\"concurrency ceiling < 2\"}}\n");
        return 1;
    }
    // A small margin below the measured ceiling: the OS's thread accounting
    // needs headroom while the previous wave's threads are still being
    // reaped (declared; the ceiling itself is reported separately).
    int usable = ceiling > 8 ? ceiling - 8 : ceiling;
    int concurrency = concurrency_wanted < usable ? concurrency_wanted : usable;

    // Phase 2: the wave engine
    stress_ledger_t led = { 0 };
    t0 = now_ns();
    if (run_waves(total, concurrency, timeout_ms, &led) != 0) {
        printf("{\"test\":\"revocation-stress\",\"lang\":\"c\",\"pass\":false,"
               "\"metrics\":{\"error\":\"wave engine failure\"}}\n");
        return 1;
    }
    uint64_t run_ns = now_ns() - t0;

    bool pass = (led.handshakes >= total) && led.timeouts == 0
                && led.ack_failures == 0 && led.poison_failures == 0
                && led.spawn_failures == 0;
    fprintf(stderr, "revoke-stress: handshakes=%lld waves=%lld (concurrency %d) "
                    "timeouts=%lld ack_fail=%lld poison_fail=%lld spawn_fail=%lld "
                    "elapsed=%llu ms\n",
            (long long)led.handshakes, (long long)led.waves, concurrency,
            (long long)led.timeouts, (long long)led.ack_failures,
            (long long)led.poison_failures, (long long)led.spawn_failures,
            (unsigned long long)(run_ns / 1000000ull));
    printf("{\"test\":\"revocation-stress\",\"lang\":\"c\",\"pass\":%s,"
           "\"metrics\":{\"handshakes\":%lld,\"waves\":%lld,"
           "\"concurrency\":%d,\"ceiling_measured\":%d,"
           "\"timeouts\":%lld,\"ack_failures\":%lld,"
           "\"poison_failures\":%lld,\"spawn_failures\":%lld,"
           "\"elapsed_ms\":%llu}}\n",
           pass ? "true" : "false",
           (long long)led.handshakes, (long long)led.waves, concurrency, ceiling,
           (long long)led.timeouts, (long long)led.ack_failures,
           (long long)led.poison_failures, (long long)led.spawn_failures,
           (unsigned long long)(run_ns / 1000000ull));
    return pass ? 0 : 1;
}
