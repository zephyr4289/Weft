// cluster_bench.c — WCR1 protocol-engine benchmark scoreboard (D-31).
//
// One JSON line per cell (repo bench convention). All numbers are
// PROTOCOL-ENGINE overhead measured over the local (direct-store)
// transport — the upper bound on what the engine adds per operation;
// Engineer 2's fabric transport adds NIC/Wire time on top. Batch means
// (time/ops) avoid per-op clock overhead; the `calib` line measures the
// bare timing loop for honest subtraction.
//
// Modes:
//   calib            — empty timing loop (measurement floor)
//   publish          — steady-state two-store publish, slot sweep
//   acquire          — wait-free acquire + watermark consume
//   roundtrip        — publish+acquire+consume pair (local echo of the
//                      cross-node loop: A publishes into B's ring, B
//                      acquires — the fabric adds wire latency only)
//   seqreg           — two-store register read/write microbench
//   consensus-read   — stable consensus block read
//   consensus-write  — consensus block replication write
//   cluster-sync     — 5-node sim: leader publish + follower poll per msg
//   election         — cold-boot election convergence (virtual + wall)

#include "weft_cluster.h"
#include "weft_cluster_internal.h"
#include "sim_cluster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t BENCH_CID[16] = "BENCH-WCR1-CLSTR";

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void emit(const char *mode, uint64_t ops, double ns_mean,
                 uint64_t extra_a, uint64_t extra_b)
{
    printf("{\"mode\":\"%s\",\"ops\":%llu,\"ns_per_op\":%.2f,"
           "\"a\":%llu,\"b\":%llu}\n",
           mode, (unsigned long long)ops, ns_mean,
           (unsigned long long)extra_a, (unsigned long long)extra_b);
    fflush(stdout);
}

static void bench_calib(void)
{
    const uint64_t N = 1000000;
    uint64_t sink = 0;
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < N; i++) sink += i;
    uint64_t t1 = now_ns();
    emit("calib", N, (double)(t1 - t0) / (double)N, sink & 1u, 0);
}

static void bench_publish(void)
{
    static const uint64_t slot_sizes[] = { 128, 192, 512, 2048, 4096 };
    const uint64_t N = 1000000;
    for (size_t si = 0; si < sizeof slot_sizes / sizeof slot_sizes[0]; si++) {
        uint64_t ssz = slot_sizes[si];
        wcr1_cfg_t cfg;
        memset(&cfg, 0, sizeof cfg);
        memcpy(cfg.cluster_id, BENCH_CID, 16);
        cfg.node_id = 2;
        cfg.cluster_size = 2;
        cfg.epoch = 7;
        cfg.capacity_slots = 65536;
        cfg.slot_size = ssz;
        cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
        cfg.peer_count = 1;
        cfg.peer_ids[0] = 1;

        wcr1_ring_view_t v;
        void *region = NULL;
        uint64_t rlen = 0;
        if (wcr1_ring_create(&cfg, 1, &v, &region, &rlen) != WEFT_CLUSTER_OK)
            continue;

        uint32_t payload_len = (uint32_t)(ssz - 64);
        uint8_t *payload = (uint8_t *)malloc(payload_len);
        memset(payload, 0xA5, payload_len);

        /* Warm-up + full cache. */
        uint64_t seq = 0;
        for (int i = 0; i < 10000; i++)
            (void)wcr1_publish(&v, NULL, payload, payload_len, 0, 0, 0, 0,
                               &seq);
        /* Steady-state consumer so backpressure never fires. */
        uint64_t t0 = now_ns();
        uint64_t ops = 0;
        for (uint64_t i = 0; i < N; i++) {
            if (wcr1_publish(&v, NULL, payload, payload_len, 0, 0, 0, 0,
                             &seq) == WEFT_CLUSTER_OK)
                ops++;
            if ((i & 1023u) == 0) {
                wcr1_slot_view_t slot;
                for (int k = 0; k < 1024; k++) {
                    if (wcr1_acquire_next(&v, 4, &slot) == WEFT_CLUSTER_OK)
                        (void)wcr1_consume(&v, slot.seq);
                    else break;
                }
            }
        }
        uint64_t t1 = now_ns();
        emit("publish", ops, (double)(t1 - t0) / (double)ops, ssz,
             payload_len);
        free(payload);
        wcr1_engine_free(region);
    }
}

static void bench_acquire(void)
{
    const uint64_t N = 1000000;
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.cluster_id, BENCH_CID, 16);
    cfg.node_id = 2;
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 65536;
    cfg.slot_size = 192;
    cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_ring_view_t v;
    void *region = NULL;
    uint64_t rlen = 0;
    if (wcr1_ring_create(&cfg, 1, &v, &region, &rlen) != WEFT_CLUSTER_OK)
        return;
    uint8_t payload[128];
    memset(payload, 0x5A, sizeof payload);
    uint64_t seq = 0;
    wcr1_slot_view_t slot;
    /* Interleaved batches: the acquire timer sees ONLY the consumer path
       (acquire + watermark consume), never backlog refusals. */
    uint64_t acq_ns = 0, ops = 0;
    const uint64_t BATCH = 1024;
    for (uint64_t b = 0; b < N / BATCH; b++) {
        for (uint64_t i = 0; i < BATCH; i++)
            (void)wcr1_publish(&v, NULL, payload, 128, 0, 0, 0, 0, &seq);
        uint64_t t0 = now_ns();
        for (uint64_t i = 0; i < BATCH; i++) {
            if (wcr1_acquire_next(&v, 4, &slot) == WEFT_CLUSTER_OK) {
                (void)wcr1_consume(&v, slot.seq);
                ops++;
            }
        }
        acq_ns += now_ns() - t0;
    }
    emit("acquire+consume", ops, (double)acq_ns / (double)ops, 128, 0);
    wcr1_engine_free(region);
}

static void bench_roundtrip(void)
{
    const uint64_t N = 500000;
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.cluster_id, BENCH_CID, 16);
    cfg.node_id = 2;
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 65536;
    cfg.slot_size = 192;
    cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_ring_view_t v;
    void *region = NULL;
    uint64_t rlen = 0;
    if (wcr1_ring_create(&cfg, 1, &v, &region, &rlen) != WEFT_CLUSTER_OK)
        return;
    uint8_t payload[128];
    memset(payload, 0xC3, sizeof payload);
    uint64_t seq = 0;
    wcr1_slot_view_t slot;

    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < N; i++) {
        (void)wcr1_publish(&v, NULL, payload, 128, 0, 0, 0, 0, &seq);
        if (wcr1_acquire_next(&v, 4, &slot) == WEFT_CLUSTER_OK)
            (void)wcr1_consume(&v, slot.seq);
    }
    uint64_t t1 = now_ns();
    emit("roundtrip", N, (double)(t1 - t0) / (double)N, 128, 0);
    wcr1_engine_free(region);
}

static void bench_seqreg(void)
{
    /* Two-store register: publish/read cycles on the raw hi/lo pair. */
    uint32_t hi = 0, lo = 0;
    const uint64_t N = 10000000;
    uint64_t acc = 0;
    uint64_t t0 = now_ns();
    for (uint64_t i = 1; i <= N; i++) {
        (void)wcr1_seqreg_publish(NULL, &hi, &lo, i);
        uint64_t s = 0;
        if (wcr1_seqreg_read(&hi, &lo, 16, &s) == WEFT_CLUSTER_OK)
            acc += s;
    }
    uint64_t t1 = now_ns();
    emit("seqreg", N, (double)(t1 - t0) / (double)N, acc & 1u, 0);
}

static void bench_consensus(void)
{
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.cluster_id, BENCH_CID, 16);
    cfg.node_id = 2;
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 64;
    cfg.slot_size = 192;
    cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_ring_view_t v;
    void *region = NULL;
    uint64_t rlen = 0;
    if (wcr1_ring_create(&cfg, 1, &v, &region, &rlen) != WEFT_CLUSTER_OK)
        return;

    const uint64_t N = 2000000;
    uint64_t hb = 0, acc = 0;
    wcr1_consensus_view_t cv;
    uint64_t token = wcr1_fencing_token(9, 2);

    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < N; i++) {
        (void)wcr1_consensus_block_write(&v, NULL, 9, 2, 12345, token, 0,
                                         0xF, &hb);
        if (wcr1_consensus_read(&v, 4, &cv) == WEFT_CLUSTER_OK)
            acc += cv.term;
    }
    uint64_t t1 = now_ns();
    emit("consensus-write+read", N, (double)(t1 - t0) / (double)N,
         acc & 1u, 0);

    uint64_t t2 = now_ns();
    for (uint64_t i = 0; i < N; i++)
        if (wcr1_consensus_read(&v, 4, &cv) == WEFT_CLUSTER_OK)
            acc += cv.term;
    uint64_t t3 = now_ns();
    emit("consensus-read", N, (double)(t3 - t2) / (double)N, acc & 1u, 0);
    wcr1_engine_free(region);
}

static void bench_cluster_sync(void)
{
    /* The headline cell: 5-node sim, leader publish + follower poll per
       message (the protocol engine's share of cross-server sync). */
    sim_opts_t o = sim_opts_default();
    o.capacity_slots = 4096;
    sim_cluster_t *s = sim_create(5, &o);
    if (!s) return;
    for (int round = 0; round < 40; round++) {
        sim_advance(s, 1250000);
        sim_tick(s);
    }
    if (sim_leader_node(s) == 0) {
        sim_destroy(s);
        return;
    }

    const uint64_t N = 100000;
    uint8_t payload[128];
    memset(payload, 0x77, sizeof payload);
    uint64_t seq = 0, ops = 0;

    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < N; i++) {
        if (wcr1_publish_as_leader(sim_ctx(s, sim_leader_node(s)), 2,
                                   payload, 128, 0, &seq) ==
            WEFT_CLUSTER_OK)
            ops++;
        if ((i & 63u) == 0) {
            (void)wcr1_node_tick(sim_ctx(s, 2));
            (void)wcr1_node_poll(sim_ctx(s, 2), 64);
        }
    }
    /* Drain. */
    for (int k = 0; k < 4000; k++)
        (void)wcr1_node_poll(sim_ctx(s, 2), 128);
    uint64_t t1 = now_ns();
    emit("cluster-sync", ops, (double)(t1 - t0) / (double)ops, 5, 128);
    sim_destroy(s);
}

static void bench_election(void)
{
    for (uint32_t n = 3; n <= 7; n += 2) {
        sim_cluster_t *s = sim_create(n, NULL);
        if (!s) continue;
        uint64_t v_boot = sim_vclock(s);
        uint64_t t0 = now_ns();
        uint64_t converged_at = 0;
        for (int round = 0; round < 400; round++) {
            sim_advance(s, 1250000);
            sim_tick(s);
            if (sim_count_leaders(s) == 1) {
                uint64_t t0tok = wcr1_ctx_token(
                    sim_ctx(s, sim_leader_node(s)));
                bool all = true;
                for (uint32_t k = 1; k <= n; k++)
                    if (wcr1_ctx_token(sim_ctx(s, k)) != t0tok) all = false;
                if (all) {
                    converged_at = sim_vclock(s) - v_boot;
                    break;
                }
            }
        }
        uint64_t t1 = now_ns();
        emit("election", n, (double)(t1 - t0), converged_at, n);
        sim_destroy(s);
    }
}

int main(int argc, char **argv)
{
    const char *only = (argc > 1) ? argv[1] : NULL;
    if (!only || strcmp(only, "calib") == 0)      bench_calib();
    if (!only || strcmp(only, "publish") == 0)    bench_publish();
    if (!only || strcmp(only, "acquire") == 0)    bench_acquire();
    if (!only || strcmp(only, "roundtrip") == 0)  bench_roundtrip();
    if (!only || strcmp(only, "seqreg") == 0)     bench_seqreg();
    if (!only || strcmp(only, "consensus") == 0)  bench_consensus();
    if (!only || strcmp(only, "cluster-sync") == 0) bench_cluster_sync();
    if (!only || strcmp(only, "election") == 0)   bench_election();
    return 0;
}
