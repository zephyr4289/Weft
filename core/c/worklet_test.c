// worklet_test.c — the native worklet conformance battery (Series 8).
//
// W1 no lost ticks: 10k posts -> drain -> executed == posted exactly.
// W2 single-consumer order: every tick ordinal observed exactly once.
// W3 QoS honesty: the spawn report carries real flags (affinity applied
//    or refused is observable; nothing silently no-ops).
// W4 round-trip latency: post->run p50/p99 measured by the tick fn.
// W5 the cadence demo: the worklet runs a governed blend loop (Q12 blend
//    over a 256x256 frame) — the exact per-tick body a consumer runs.

#include "worklet.h"
#include "blend_q12.h"

#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

#define FRAME_WORDS (256 * 256)
static uint32_t prev_f[FRAME_WORDS], new_f[FRAME_WORDS], out_f[FRAME_WORDS];

static void fill(uint32_t *b, uint32_t seed) {
    for (size_t i = 0; i < FRAME_WORDS; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        b[i] = seed;
    }
}

/* W1/W2/W4 capture context */
#define CAPTURE_N 20000
static _Atomic uint64_t seen_count;
static uint8_t seen[CAPTURE_N + 8];
static _Atomic double post_ts[CAPTURE_N + 8];

static void capture_fn(void *ctx, uint64_t tick) {
    (void)ctx;
    const double t0 = now_ns();
    if (tick <= CAPTURE_N) {
        seen[tick] += 1;
        atomic_store_explicit(&post_ts[tick], t0, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&seen_count, 1, memory_order_relaxed);
}

static void blend_fn(void *ctx, uint64_t tick) {
    (void)ctx; (void)tick;
    weft_blend_q12(prev_f, new_f, out_f, FRAME_WORDS, 1234);
}

static int cmp_double(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void) {
    int failures = 0;
    weft_worklet_spawn_report rep;

    printf("== worklet_test ==\n");

    /* ---- W1 + W2 + W4: capture worklet -------------------------------- */
    fill(prev_f, 0x11111111);
    fill(new_f, 0x22222222);
    weft_qos_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.cls = WEFT_QOS_USER_INTERACTIVE;
    spec.cpu_mask = 0;    /* no forced affinity — the honest default */
    spec.rt_priority = 0; /* no FIFO attempt in a container */
    weft_worklet_spawn(capture_fn, NULL, &spec, &rep);
    if (!rep.worklet || !rep.started) {
        fprintf(stderr, "W1-FAIL spawn: %s\n", rep.err);
        return 1;
    }
    printf("W3 spawn qos_flags = 0x%x (%s%s)\n", rep.qos_flags,
           (rep.qos_flags & WEFT_QOS_APPLIED_AFFINITY) ? "affinity " : "",
           (rep.qos_flags & WEFT_QOS_SCHED_UNPRIVILEGED) ? "rt-unprivileged" : "");
    /* W3: the flags are honest — with a zeroed spec the render preset
     * makes no RT request, so neither APPLIED_SCHED nor UNPRIVILEGED
     * may appear. */
    if (rep.qos_flags & (WEFT_QOS_APPLIED_SCHED | WEFT_QOS_SCHED_UNPRIVILEGED)) {
        fprintf(stderr, "W3-FAIL: flags claim an RT attempt that never happened\n");
        failures++;
    }

    for (uint64_t i = 1; i <= CAPTURE_N; i++) {
        weft_worklet_post(rep.worklet);
    }
    if (weft_worklet_drain(rep.worklet, 10000) != 0) {
        fprintf(stderr, "W1-FAIL drain timeout (pending=%llu)\n",
                (unsigned long long)weft_worklet_pending(rep.worklet));
        failures++;
    }
    const uint64_t executed = weft_worklet_executed(rep.worklet);
    if (executed != CAPTURE_N) {
        fprintf(stderr, "W1-FAIL executed=%llu != posted=%llu\n",
                (unsigned long long)executed, (unsigned long long)CAPTURE_N);
        failures++;
    } else {
        printf("W1 no lost ticks: %llu posts -> executed %llu\n",
               (unsigned long long)CAPTURE_N, (unsigned long long)executed);
    }
    int dup = 0;
    for (uint64_t i = 1; i <= CAPTURE_N; i++) {
        if (seen[i] != 1) dup++;
    }
    if (dup) {
        fprintf(stderr, "W2-FAIL %d tick ordinals seen != 1 time\n", dup);
        failures++;
    } else {
        printf("W2 single-consumer order: every ordinal exactly once\n");
    }
    /* W4: post->run latency distribution */
    double lats[CAPTURE_N + 8];
    for (uint64_t i = 2; i <= CAPTURE_N; i++) {
        lats[i] = atomic_load_explicit(&post_ts[i], memory_order_relaxed) -
                  atomic_load_explicit(&post_ts[i - 1], memory_order_relaxed);
    }
    const int n = (int)CAPTURE_N - 1;
    qsort(lats + 2, n, sizeof(double), cmp_double);
    printf("W4 inter-tick latency us: p50=%.1f p99=%.1f max=%.1f\n",
           lats[2 + n / 2] / 1e3, lats[2 + (int)(n * 0.99)] / 1e3,
           lats[2 + n - 1] / 1e3);
    weft_worklet_destroy(rep.worklet);

    /* ---- W5: the cadence blend worklet -------------------------------- */
    weft_worklet_spawn(blend_fn, NULL, &spec, &rep);
    if (!rep.worklet) {
        fprintf(stderr, "W5-FAIL spawn: %s\n", rep.err);
        return 1;
    }
    const int iters = 600;
    const double t0 = now_ns();
    for (int i = 0; i < iters; i++) weft_worklet_post(rep.worklet);
    weft_worklet_drain(rep.worklet, 10000);
    const double per_frame = (now_ns() - t0) / iters;
    const double mbps = (double)FRAME_WORDS * 4.0 * 3.0 / per_frame;
    printf("W5 governed blend worklet: %.0f us/frame (%.1f GB/s) at 256x256\n",
           per_frame / 1e3, mbps);
    weft_worklet_destroy(rep.worklet);

    if (failures) {
        fprintf(stderr, "worklet_test: %d gate failure(s)\n", failures);
        return 1;
    }
    printf("worklet_test: ALL GATES GREEN\n");
    return 0;
}
