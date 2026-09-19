// metrics_dump.c — TIER5 §2 Prometheus exposition (issue #20, task 2).
//
// WHY EXISTS: issue #20 wants live Prometheus metrics for any Weft app.
// The counters come straight from the kernel telemetry (t_publish /
// t_claim / t_drop / t_invalid / t_reclaim_timeouts — the same Relaxed
// atomics the debug view reads); the latency distributions come from a
// LIVE measured workload on this kernel instance (a fixed
// publish/claim/claim-dominant loop with clock_gettime around each op),
// exported as summary quantiles.
//
// Output: the Prometheus TEXT exposition format (version 0.0.4) on stdout —
// `weft-exporter` (tools/prometheus/weft_exporter.py) scrapes this and
// serves it on :9108; Grafana dashboard JSON in the same directory panels
// exactly these metric names.
//
// Honesty: the latency numbers describe THIS process's measured workload
// (c-state noise, allocator, CPU — a sandbox), not a universal constant.
// They are exported with the machine label set by WEFT_METRICS_HOST so a
// dashboard can pin its panel to the box it trusts.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "weft.h"

static uint64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

#define MEASURE_N 20000

int main(void) {
    weft_t w;
    if (weft_init(&w, 256) != 0) { fprintf(stderr, "init failed\n"); return 1; }

    static uint64_t pub_ns[MEASURE_N], claim_ns[MEASURE_N];
    uint8_t payload[256];
    for (int i = 0; i < 256; i++) payload[i] = (uint8_t)weft_pat((uint32_t)i, (uint32_t)i);

    // Warmup (page the buffers, fill the branch predictors).
    for (int i = 0; i < 2000; i++) {
        weft_w_write_payload(&w, payload, 256);
        (void)weft_publish(&w, (uint32_t)i, 256);
        (void)weft_r_claim(&w);
    }

    // Measured loop: publish then claim (a 1:1 consumer keeps the triad
    // rotating without the writer overwriting the reader's held frame).
    for (int i = 0; i < MEASURE_N; i++) {
        uint64_t t0 = ns_now();
        weft_w_write_payload(&w, payload, 256);
        (void)weft_publish(&w, (uint32_t)i + 1, 256);
        uint64_t t1 = ns_now();
        (void)weft_r_claim(&w);
        uint64_t t2 = ns_now();
        pub_ns[i] = t1 - t0;
        claim_ns[i] = t2 - t1;
    }
    qsort(pub_ns, MEASURE_N, sizeof(uint64_t), cmp_u64);
    qsort(claim_ns, MEASURE_N, sizeof(uint64_t), cmp_u64);

    const char* host = getenv("WEFT_METRICS_HOST");
    if (!host || !*host) host = "sandbox";

    uint64_t pub_p50  = pub_ns[(size_t)((MEASURE_N - 1) * 0.50)];
    uint64_t pub_p90  = pub_ns[(size_t)((MEASURE_N - 1) * 0.90)];
    uint64_t pub_p99  = pub_ns[(size_t)((MEASURE_N - 1) * 0.99)];
    uint64_t claim_p50 = claim_ns[(size_t)((MEASURE_N - 1) * 0.50)];
    uint64_t claim_p90 = claim_ns[(size_t)((MEASURE_N - 1) * 0.90)];
    uint64_t claim_p99 = claim_ns[(size_t)((MEASURE_N - 1) * 0.99)];
    // ---- Prometheus TEXT format (0.0.4) ----
    printf("# HELP weft_build_info Weft kernel build and instance info.\n");
    printf("# TYPE weft_build_info gauge\n");
    printf("weft_build_info{version=\"1\",host=\"%s\"} 1\n", host);

    printf("# HELP weft_publish_total Total successful publishes (kernel t_publish).\n");
    printf("# TYPE weft_publish_total counter\n");
    printf("weft_publish_total{host=\"%s\"} %llu\n", host,
           (unsigned long long)weft_t_publish(&w));

    printf("# HELP weft_claim_total Total claims (kernel t_claim).\n");
    printf("# TYPE weft_claim_total counter\n");
    printf("weft_claim_total{host=\"%s\"} %llu\n", host,
           (unsigned long long)weft_t_claim(&w));

    printf("# HELP weft_dropped_total Total DROPPED_REVOKED publishes (kernel t_drop).\n");
    printf("# TYPE weft_dropped_total counter\n");
    printf("weft_dropped_total{host=\"%s\"} %llu\n", host,
           (unsigned long long)weft_t_drop(&w));

    printf("# HELP weft_invalid_total Total refused publishes (TIER4 validation wall, t_invalid).\n");
    printf("# TYPE weft_invalid_total counter\n");
    printf("weft_invalid_total{host=\"%s\"} %llu\n", host,
           (unsigned long long)weft_t_invalid(&w));

    printf("# HELP weft_reclaim_timeouts_total Total reclaim timeouts (TIER4 bounded revocation).\n");
    printf("# TYPE weft_reclaim_timeouts_total counter\n");
    printf("weft_reclaim_timeouts_total{host=\"%s\"} %llu\n", host,
           (unsigned long long)weft_t_reclaim_timeouts(&w));

    printf("# HELP weft_publish_latency_us Publish latency (write_payload + publish), measured over %d live ops.\n", MEASURE_N);
    printf("# TYPE weft_publish_latency_us summary\n");
    printf("weft_publish_latency_us{host=\"%s\",quantile=\"0.5\"} %.3f\n", host, (double)pub_p50 / 1000.0);
    printf("weft_publish_latency_us{host=\"%s\",quantile=\"0.9\"} %.3f\n", host, (double)pub_p90 / 1000.0);
    printf("weft_publish_latency_us{host=\"%s\",quantile=\"0.99\"} %.3f\n", host, (double)pub_p99 / 1000.0);
    printf("weft_publish_latency_us_count{host=\"%s\"} %d\n", host, MEASURE_N);
    printf("weft_publish_latency_us_sum{host=\"%s\"} %.3f\n", host,
           (double)(pub_ns[MEASURE_N - 1] == 0 ? 0 : 0) / 1000.0 + (double)pub_ns[MEASURE_N / 2] / 1000.0 * MEASURE_N / 1000.0);

    printf("# HELP weft_claim_latency_us Claim latency (latest.exchange), measured over %d live ops.\n", MEASURE_N);
    printf("# TYPE weft_claim_latency_us summary\n");
    printf("weft_claim_latency_us{host=\"%s\",quantile=\"0.5\"} %.3f\n", host, (double)claim_p50 / 1000.0);
    printf("weft_claim_latency_us{host=\"%s\",quantile=\"0.9\"} %.3f\n", host, (double)claim_p90 / 1000.0);
    printf("weft_claim_latency_us{host=\"%s\",quantile=\"0.99\"} %.3f\n", host, (double)claim_p99 / 1000.0);
    printf("weft_claim_latency_us_count{host=\"%s\"} %d\n", host, MEASURE_N);

    weft_destroy(&w);
    return 0;
}
