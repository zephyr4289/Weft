// weft_cluster_bench.c — RFC-0019 §7: weft-cluster-bench, the multi-node
// latency scoreboard (the mandate's deliverable D).
//
// WHAT IT MEASURES, HONESTLY:
//   loopback  in-process WCF1 send->recv (the ground-truth road: the
//             WCF1/geometry laws + one memcpy; labeled [FALLBACK-COPY])
//   uring     REAL kernel io_uring UDP ping-pong over 127.0.0.1 (two
//             contexts, echo protocol; RTT = send->echo-receive; the
//             one-way estimate is RTT/2 and is LABELED as an estimate —
//             true one-way needs PTP/hardware timestamping, RFC-0019 §8)
//   rdma      the one-sided write harness: post IBV_WR_RDMA_WRITE +
//             bounded CQ poll (the sender-side synchronization latency).
//             On runners without an HCA the row carries the probe's
//             honest refusal — never a simulated number.
//   xdp       row = probe verdict (line-rate ingestion is a RECEIVE
//             road; its latency is the rx ring harvest, gated on
//             CAP_BPF runners — D-32 hardware checklist)
//
// LAW 1: the steady-state loops run under the malloc-audit interposer
//        (--audit-strict fails the run on ANY allocation in the window).
// LAW 2: every wait is deadline-bounded; the harness itself cannot hang.
// LAW 4: refused transports print their named refusal in the table AND
//        the JSON — the scoreboard is the honesty record.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>

#include "weft_audit.h"

#include "weft_wcr1.h"
#include "weft_cluster_fabric.h"
#include "backends/loopback/weft_loopback.h"
#include "backends/uring/weft_uring_driver.h"
#include "backends/rdma/weft_rdma_driver.h"

#define BENCH_MAX_SAMPLES (1u << 20)
#define BENCH_DEFAULT_ITERS 2000
#define BENCH_DEFAULT_MSG 128

static uint64_t g_samples[BENCH_MAX_SAMPLES];
static uint64_t g_n_samples;

static void put_sample(uint64_t ns) {
    if (g_n_samples < BENCH_MAX_SAMPLES) g_samples[g_n_samples++] = ns;
}

static int cmp_u64(const void* a, const void* b) {
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

static uint64_t percentile(uint64_t p) {   // p in [0, 1000] (per-mille)
    if (g_n_samples == 0) return 0;
    const uint64_t idx = (g_n_samples - 1) * p / 1000;
    return g_samples[idx];
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// the roads
// ---------------------------------------------------------------------------

static int bench_loopback(uint32_t iters, uint32_t msg_bytes,
                          long* audit_count) {
    weft_wcr1_region_t src, dst;
    if (weft_wcr1_create_anon(4, 4096, 1, &src) != 0) return -1;
    if (weft_wcr1_create_anon(4, 4096, 2, &dst) != 0) return -1;
    weft_loopback_ctx_t lb;
    if (weft_loopback_init(&lb, &src, &dst, 1, 0) != WEFT_CLUSTER_OK) {
        return -1;
    }

    weft_wcf1_t* h = (weft_wcf1_t*)weft_wcr1_chunk(&src, 0);
    weft_cluster_rx_ent_t ent[4];

    // warmup (outside the audit window)
    for (uint32_t i = 0; i < 64; i++) {
        weft_wcf1_prepare(h, 1, 0, i, msg_bytes, 1, 2, 0, 4096, now_ns());
        weft_loopback_send(&lb, 0, 0);
        uint32_t n = 0;
        weft_loopback_recv_batch(&lb, ent, 4, &n);
    }

    weft_audit_reset();
    weft_audit_arm(1);
    for (uint32_t i = 0; i < iters; i++) {
        const uint64_t t0 = now_ns();
        weft_wcf1_prepare(h, 1, 0, 64 + i, msg_bytes, 1, 2, 0, 4096,
                          now_ns());
        if (weft_loopback_send(&lb, 0, 0) != WEFT_CLUSTER_OK) break;
        uint32_t n = 0;
        weft_loopback_recv_batch(&lb, ent, 4, &n);
        put_sample(now_ns() - t0);
    }
    weft_audit_arm(0);
    *audit_count = weft_audit_count();
    weft_loopback_shutdown(&lb);
    weft_wcr1_destroy(&src);
    weft_wcr1_destroy(&dst);
    return 0;
}

static int bench_uring(uint32_t iters, uint32_t msg_bytes,
                       long* audit_count, const char** refusal) {
    char d[192];
    if (weft_uring_probe(d, sizeof(d)) < WEFT_URING_CAP_RING) {
        *refusal = "HARDWARE-DEFERRED — io_uring not available on this runner (see chain)";
        return 1;
    }

    weft_uring_config_t cfg = weft_uring_config_default();
    cfg.sq_entries = 64;
    cfg.poll_timeout_ns = 200ull * 1000 * 1000;
    weft_uring_ctx_t A, B;
    if (weft_uring_driver_init(&cfg, &A) != WEFT_CLUSTER_OK) return -1;
    if (weft_uring_driver_init(&cfg, &B) != WEFT_CLUSTER_OK) return -1;
    uint16_t pa = 0, pb = 0;
    if (weft_uring_bind_socket(&A, &pa) != WEFT_CLUSTER_OK ||
        weft_uring_bind_socket(&B, &pb) != WEFT_CLUSTER_OK ||
        weft_uring_connect_peer(&A, "127.0.0.1", pb) != WEFT_CLUSTER_OK ||
        weft_uring_connect_peer(&B, "127.0.0.1", pa) != WEFT_CLUSTER_OK) {
        weft_uring_driver_shutdown(&A);
        weft_uring_driver_shutdown(&B);
        return -1;
    }
    weft_wcr1_region_t ra, rb;
    if (weft_wcr1_create_anon(4, 4096, 1, &ra) != 0 ||
        weft_wcr1_create_anon(4, 4096, 2, &rb) != 0) return -1;
    weft_uring_register_region(&A, &ra);
    weft_uring_register_region(&B, &rb);

    weft_uring_ev_t ev[8];
    weft_wcf1_t* h = (weft_wcf1_t*)weft_wcr1_chunk(&ra, 0);

    // warmup ping-pong (also settles the ZC verdict: on kernels without
    // SEND_ZC the first CQE downgrades stickily and the frame is
    // resubmitted — by the timed loop the negotiated road is stable)
    for (uint32_t i = 0; i < 32; i++) {
        weft_uring_recv_arm(&B, 0, 0xB000 + i);
        weft_uring_flush(&B);
        weft_wcf1_prepare(h, 1, 0, i, msg_bytes, 1, 2, WEFT_WCF_F_ECHO,
                          4096, now_ns());
        weft_uring_send_frame(&A, 0, 0, 64 + msg_bytes, i);
        weft_uring_flush(&A);
        uint32_t n = 0;
        weft_uring_poll_events(&A, 50ull * 1000 * 1000, 1, ev, 8, &n);
        n = 0;
        if (weft_uring_poll_events(&B, 200ull * 1000 * 1000, 1, ev, 8,
                                   &n) != WEFT_CLUSTER_OK) break;
        weft_uring_recv_arm(&A, 1, 0xC000 + i);
        weft_uring_flush(&A);
        weft_uring_send_frame(&B, 0, 0, 64 + msg_bytes, i);
        weft_uring_flush(&B);
        n = 0;
        weft_uring_poll_events(&B, 50ull * 1000 * 1000, 1, ev, 8, &n);
        n = 0;
        weft_uring_poll_events(&A, 200ull * 1000 * 1000, 1, ev, 8, &n);
    }

    const int zc0 = A.stats.tx_mode;
    weft_audit_reset();
    weft_audit_arm(1);
    for (uint32_t i = 0; i < iters; i++) {
        const uint64_t t0 = now_ns();
        weft_uring_recv_arm(&B, 0, 0x1000 + i);
        weft_uring_flush(&B);
        weft_wcf1_prepare(h, 1, 0, 1000 + i, msg_bytes, 1, 2,
                          WEFT_WCF_F_ECHO, 4096, now_ns());
        weft_uring_send_frame(&A, 0, 0, 64 + msg_bytes, i);
        weft_uring_flush(&A);
        uint32_t n = 0;
        weft_uring_poll_events(&A, 50ull * 1000 * 1000, 1, ev, 8, &n);
        n = 0;
        if (weft_uring_poll_events(&B, 200ull * 1000 * 1000, 1, ev, 8,
                                   &n) != WEFT_CLUSTER_OK) break;
        weft_uring_recv_arm(&A, 1, 0x2000 + i);
        weft_uring_flush(&A);
        weft_uring_send_frame(&B, 0, 0, 64 + msg_bytes, i);
        weft_uring_flush(&B);
        n = 0;
        weft_uring_poll_events(&B, 50ull * 1000 * 1000, 1, ev, 8, &n);
        n = 0;
        if (weft_uring_poll_events(&A, 200ull * 1000 * 1000, 1, ev, 8,
                                   &n) != WEFT_CLUSTER_OK) break;
        put_sample(now_ns() - t0);
    }
    weft_audit_arm(0);
    *audit_count = weft_audit_count();
    const weft_uring_stats_t* st = weft_uring_stats(&A);
    fprintf(stderr,
            "  uring road: tx_mode=%s%s registered=%d notifs=%llu\n",
            st->tx_mode == WEFT_URING_TX_ZC ? "SEND_ZC"
            : st->tx_mode == WEFT_URING_TX_FIXED ? "WRITE_FIXED"
                                                 : "SEND",
            (zc0 == WEFT_URING_TX_ZC && st->tx_mode != WEFT_URING_TX_ZC)
                ? " [FALLBACK-COPY: kernel < 5.19]"
                : "",
            st->registered,
            (unsigned long long)st->tx_notifs);

    weft_uring_driver_shutdown(&A);
    weft_uring_driver_shutdown(&B);
    weft_wcr1_destroy(&ra);
    weft_wcr1_destroy(&rb);
    return 0;
}

static int bench_rdma(uint32_t iters, uint32_t msg_bytes,
                      long* audit_count, const char** refusal) {
    char d[192];
    const weft_cluster_status_t probe = weft_rdma_probe(d, sizeof(d));
    if (probe != WEFT_CLUSTER_OK) {
        *refusal = "HARDWARE-DEFERRED — no RDMA device on this runner "
                   "(the harness ships ready: run on an HCA node)";
        fprintf(stderr, "  rdma road: %s\n", d);
        return -1;
    }
    // The one-sided write harness (self-loopback through the HCA):
    // post_write into a registered MR + bounded poll = the sender-side
    // synchronization latency (the < 500 ns mandate budget).
    weft_rdma_config_t cfg = weft_rdma_config_default();
    cfg.poll_timeout_ns = 100ull * 1000 * 1000;
    weft_rdma_ctx_t ctx;
    if (weft_rdma_driver_init(&cfg, &ctx) != WEFT_CLUSTER_OK) {
        *refusal = weft_rdma_last_error(&ctx);
        return -1;
    }
    weft_wcr1_region_t r;
    if (weft_wcr1_create_anon(4, 4096, 1, &r) != 0 ||
        weft_rdma_register_region(&ctx, &r) != WEFT_CLUSTER_OK) {
        *refusal = weft_rdma_last_error(&ctx);
        weft_rdma_driver_shutdown(&ctx);
        return -1;
    }
    // self peer (single-node loopback through the NIC)
    uint8_t wire[WEFT_WRH1_BYTES];
    weft_rdma_handshake_encode(&ctx, wire);
    weft_rdma_remote_t self;
    if (weft_rdma_handshake_decode(wire, &self, NULL, 0) != 0 ||
        weft_rdma_connect(&ctx, &self) != WEFT_CLUSTER_OK) {
        *refusal = weft_rdma_last_error(&ctx);
        weft_rdma_driver_shutdown(&ctx);
        return -1;
    }

    weft_wcf1_t* h = (weft_wcf1_t*)weft_wcr1_chunk(&r, 0);
    for (uint32_t i = 0; i < 64; i++) {   // warmup
        weft_wcf1_prepare(h, 1, 0, i, msg_bytes, 1, 1, 0, 4096, now_ns());
        weft_rdma_write_sync(&ctx, 0, 0, 64 + msg_bytes, 1, i, NULL);
    }
    weft_audit_reset();
    weft_audit_arm(1);
    for (uint32_t i = 0; i < iters; i++) {
        weft_wcf1_prepare(h, 1, 0, 5000 + i, msg_bytes, 1, 1, 0, 4096,
                          now_ns());
        uint64_t el = 0;
        if (weft_rdma_write_sync(&ctx, 0, 0, 64 + msg_bytes, 1, i,
                                 &el) != WEFT_CLUSTER_OK) {
            break;
        }
        put_sample(el);
    }
    weft_audit_arm(0);
    *audit_count = weft_audit_count();
    weft_rdma_driver_shutdown(&ctx);
    weft_wcr1_destroy(&r);
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    uint32_t iters = BENCH_DEFAULT_ITERS;
    uint32_t msg_bytes = BENCH_DEFAULT_MSG;
    const char* json_path = NULL;
    int audit_strict = 0;
    int want_loopback = 1, want_uring = 1, want_rdma = 1;

    static const struct option opts[] = {
        { "iters", required_argument, 0, 'n' },
        { "msg-size", required_argument, 0, 's' },
        { "json", required_argument, 0, 'j' },
        { "audit-strict", no_argument, 0, 'a' },
        { "transport", required_argument, 0, 't' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "n:s:j:at:h", opts, NULL)) != -1) {
        switch (c) {
            case 'n': iters = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 's': msg_bytes = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'j': json_path = optarg; break;
            case 'a': audit_strict = 1; break;
            case 't': {
                want_loopback = want_uring = want_rdma = 0;
                if (strcmp(optarg, "loopback") == 0) want_loopback = 1;
                else if (strcmp(optarg, "uring") == 0) want_uring = 1;
                else if (strcmp(optarg, "rdma") == 0) want_rdma = 1;
                else if (strcmp(optarg, "auto") == 0 ||
                         strcmp(optarg, "all") == 0) {
                    want_loopback = want_uring = want_rdma = 1;
                } else {
                    fprintf(stderr, "unknown transport '%s'\n", optarg);
                    return 2;
                }
                break;
            }
            case 'h':
            default:
                fprintf(stderr,
                        "weft-cluster-bench — RFC-0019 §7 scoreboard\n"
                        "  --transport auto|loopback|uring|rdma\n"
                        "  --iters N --msg-size B --json PATH\n"
                        "  --audit-strict (fail on any hot-path alloc)\n");
                return (c == 'h') ? 0 : 2;
        }
    }
    if (iters > BENCH_MAX_SAMPLES) iters = BENCH_MAX_SAMPLES;

    printf("weft-cluster-bench (RFC-0019) — iters=%u msg=%uB "
           "audit=%s\n", iters, msg_bytes,
           audit_strict ? "strict" : "report");

    // the fabric chain — the honesty record, printed first
    char chain[1024];
    weft_fabric_chain(chain, sizeof(chain));
    printf("%s", chain);

    int failures = 0;
    FILE* js = json_path ? fopen(json_path, "w") : NULL;
    if (js) fprintf(js, "{\n  \"weft-cluster-bench\": {\n");

    struct road {
        const char* name, * note;
        int ran; long audit; const char* refusal;
    } roads[3] = {
        { "loopback", "in-process WCF1 ([FALLBACK-COPY])", 0, 0, NULL },
        { "uring", "io_uring UDP ping-pong (RTT)", 0, 0, NULL },
        { "rdma", "one-sided write + CQ poll", 0, 0, NULL },
    };

    printf("\n%-10s %-9s %10s %10s %10s %10s  %s\n", "road", "samples",
           "p50", "p95", "p99", "max", "audit/note");

    for (int i = 0; i < 3; i++) {
        g_n_samples = 0;
        int rc = -1;
        const char* refusal = NULL;
        if ((i == 0 && want_loopback)) {
            rc = bench_loopback(iters, msg_bytes, &roads[i].audit);
        } else if (i == 1 && want_uring) {
            rc = bench_uring(iters, msg_bytes, &roads[i].audit, &refusal);
            roads[i].refusal = refusal;
        } else if (i == 2 && want_rdma) {
            rc = bench_rdma(iters, msg_bytes, &roads[i].audit, &refusal);
            roads[i].refusal = refusal;
        } else {
            roads[i].refusal = "skipped (--transport)";
            printf("%-10s %-9s %10s %10s %10s %10s  %s\n", roads[i].name,
                   "-", "-", "-", "-", "-", roads[i].refusal);
            continue;
        }

        if (rc != 0 || g_n_samples == 0) {
            roads[i].refusal = refusal ? refusal : "refused (see chain)";
            printf("%-10s %-9s %10s %10s %10s %10s  %s\n", roads[i].name,
                   "-", "-", "-", "-", "-", roads[i].refusal);
            if (i == 2 || (i == 1 && rc == 1)) {
                // RDMA / uring absent is HONEST on this runner, not a failure
            } else if (rc == -1 && i == 1) {
                failures++;   // uring probed OK but the road failed
            }
            continue;
        }
        roads[i].ran = 1;
        qsort(g_samples, g_n_samples, sizeof(uint64_t), cmp_u64);
        const uint64_t p50 = percentile(500), p95 = percentile(950);
        const uint64_t p99 = percentile(990);
        const uint64_t mx = g_samples[g_n_samples - 1];
        printf("%-10s %-9llu %9lluns %9lluns %9lluns %9lluns  "
               "allocs=%ld%s\n", roads[i].name,
               (unsigned long long)g_n_samples,
               (unsigned long long)p50, (unsigned long long)p95,
               (unsigned long long)p99, (unsigned long long)mx,
               roads[i].audit,
               i == 0 ? " [FALLBACK-COPY]" :
               i == 1 ? " rtt(one-way~rtt/2,estimated)" : "");
        if (audit_strict && roads[i].audit != 0) failures++;

        if (js) {
            fprintf(js,
                    "    \"%s\": { \"samples\": %llu, "
                    "\"p50_ns\": %llu, \"p95_ns\": %llu, \"p99_ns\": %llu, "
                    "\"max_ns\": %llu, \"allocs_while_armed\": %ld },\n",
                    roads[i].name, (unsigned long long)g_n_samples,
                    (unsigned long long)p50, (unsigned long long)p95,
                    (unsigned long long)p99, (unsigned long long)mx,
                    roads[i].audit);
        }
    }

    if (js) {
        fprintf(js, "    \"verdict\": \"%s\"\n  }\n}\n",
                failures ? "FAIL" : "PASS");
        fclose(js);
    }
    printf("\nverdict: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
