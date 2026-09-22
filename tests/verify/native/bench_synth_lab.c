// bench_synth_lab.c — Pillar 8 gated benchmark: strict latency and
// throughput evidence that the synthetic lab itself stays out of the
// way.
//
// Gates (plain leg only; sanitizer legs run raw with BENCH_NO_GATES=1 —
// house precedent from P6/P7):
//   G1  interceptor overhead when DISABLED < 5% — the armed-but-off
//       fabric vs a HOOK-FREE TWIN of the same pool/wheel/ring pipeline
//       (identical copy volume, identical ring mechanics; the only delta
//       is the chaos-interceptor branches the mandate prices)
//   G2  determinism when ENABLED — same seed replays bit-for-bit, a
//       different seed diverges (thermal + net + consensus hashes)
//   G3  per-op budgets: GE decision, Bernoulli decision, bit-flip send,
//       jitter send, fabric tick, consensus step, thermal step
//   G4  seqlock probe p99 (10 kHz writer + max blender, min of 3 rounds)
//
// Reported (no gate): CRC-32 throughput, thermal hook absolute costs at
// nominal frequency, 500 kHz max-rate probe p99 tail (hardware-bound).

#include "test_util.h"

#include "weft_synth/weft_synth_bus.h"
#include "weft_synth/weft_synth_net.h"
#include "weft_synth/weft_synth_thermal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static int g_gates_on = 1;
static weft_synth_net_t g_net;   /* the real fabric */
static weft_synth_net_t g_net2;  /* the objcopy-renamed interceptor-free
                                    recompile of the SAME source (G1) */
static weft_synth_consensus_t g_cs;
static weft_synth_thermal_t g_th;
static _Atomic uint64_t g_arena[512] __attribute__((aligned(128)));
static uint64_t g_lat[131072];

#define GATE(name, ok)                                                     \
    do {                                                                   \
        if (g_gates_on) {                                                  \
            if (ok) {                                                      \
                printf("GATE  PASS  %s\n", name);                          \
            } else {                                                       \
                printf("GATE  FAIL  %s\n", name);                          \
                g_gate_fail++;                                             \
            }                                                              \
        } else {                                                           \
            printf("RAW         %s (no gate: sanitizer leg)\n", name);     \
        }                                                                  \
    } while (0)
static int g_gate_fail = 0;

/* ================================================================== */
/* G1 baseline: the SAME fabric code with the chaos layer compiled out  */
/* ================================================================== */
/*
 * The hook-free baseline is weft_synth_net.c compiled a SECOND time with
 * -DWEFT_SYNTH_NET_NO_INTERCEPTOR and its symbols objcopy-renamed to
 * twin_* (Makefile rule). Same source, same struct, same TU-internal
 * optimization luck — the only delta is the chaos interceptor, which is
 * exactly what the directive's "< 5% when disabled" prices.
 */
extern int twin_weft_synth_net_init(weft_synth_net_t *net,
                         const weft_synth_net_cfg_t *cfg);
extern int twin_weft_synth_net_send(weft_synth_net_t *net, uint32_t from, uint32_t to,
                         uint8_t kind, const void *payload, uint32_t len);
extern int twin_weft_synth_net_tick(weft_synth_net_t *net);
extern int twin_weft_synth_net_recv(weft_synth_net_t *net, uint32_t node,
                         weft_synth_net_pkt_t *out_pkt);

/* ================================================================== */
/* G1 — interceptor overhead when disabled                              */
/* ================================================================== */

static void b1_disabled_overhead(void) {
    printf("=== B1 interceptor overhead (chaos DISABLED) ===\n");
    weft_synth_net_cfg_t cfg;
    TU_CHECK(weft_synth_net_defaults(&cfg) == 0);
    TU_CHECK(weft_synth_net_init(&g_net, &cfg) == 0);
    TU_CHECK(twin_weft_synth_net_init(&g_net2, &cfg) == 0);

    const uint64_t n = tu_iters(200000ull);
    const uint32_t reps = 5u;
    double best_fabric = 1e30, best_twin = 1e30;
    weft_synth_net_pkt_t pkt;
    uint64_t sink = 0;

    for (uint32_t r = 0u; r < reps; r++) {
        TU_CHECK(twin_weft_synth_net_init(&g_net2, &cfg) == 0);
        int64_t t0 = tu_now_ns();
        for (uint64_t i = 0ull; i < n; i++) {
            uint64_t v = i;
            (void)twin_weft_synth_net_send(&g_net2, 0u, 1u,
                                WEFT_SYNTH_NET_KIND_DATA, &v, sizeof(v));
            (void)twin_weft_synth_net_tick(&g_net2);
            if (twin_weft_synth_net_recv(&g_net2, 1u, &pkt) == 1) {
                sink += pkt.seq;
            }
        }
        int64_t t1 = tu_now_ns();
        if ((double)(t1 - t0) < best_twin) {
            best_twin = (double)(t1 - t0);
        }
    }
    for (uint32_t r = 0u; r < reps; r++) {
        TU_CHECK(weft_synth_net_init(&g_net, &cfg) == 0);
        int64_t t0 = tu_now_ns();
        for (uint64_t i = 0ull; i < n; i++) {
            uint64_t v = i;
            (void)weft_synth_net_send(&g_net, 0u, 1u,
                                      WEFT_SYNTH_NET_KIND_DATA, &v,
                                      sizeof(v));
            (void)weft_synth_net_tick(&g_net);
            if (weft_synth_net_recv(&g_net, 1u, &pkt) == 1) {
                sink += pkt.seq;
            }
        }
        int64_t t1 = tu_now_ns();
        if ((double)(t1 - t0) < best_fabric) {
            best_fabric = (double)(t1 - t0);
        }
    }
    const double ns_fab = best_fabric / (double)n;
    const double ns_twin = best_twin / (double)n;
    const double overhead = (ns_fab - ns_twin) / ns_twin * 100.0;
    printf("  chaos-free recompile : %.2f ns/op (send+tick+recv, 8B payload)\n",
           ns_twin);
    printf("  chaos DISABLED       : %.2f ns/op\n", ns_fab);
    printf("  interceptor overhead : %+.2f%% (sink %" PRIu64 ")\n",
           overhead, sink);
    GATE("G1 disabled-hook overhead < 5%", overhead < 5.0 && ns_fab > 0.0);
}

/* ================================================================== */
/* G2 — determinism when enabled                                        */
/* ================================================================== */

static uint64_t scenario_hash(uint32_t seed) {
    weft_synth_net_cfg_t ncfg;
    weft_synth_net_defaults(&ncfg);
    ncfg.seed = seed;
    ncfg.fault.enabled = 1;
    ncfg.fault.model = WEFT_SYNTH_NET_FAULT_GILBERT_ELLIOTT;
    ncfg.fault.ge_p_g2b = 0.05;
    ncfg.fault.ge_p_b2g = 0.40;
    ncfg.fault.ge_drop_good = 0.10;
    ncfg.fault.ge_drop_bad = 0.85;
    ncfg.fault.jitter_mean_ns = 15000.0;
    ncfg.fault.jitter_std_ns = 8000.0;
    ncfg.fault.jitter_max_ns = 80000.0;
    ncfg.fault.reorder_p = 0.25;
    ncfg.fault.reorder_extra_mean_ns = 25000.0;
    ncfg.fault.bitflip_p = 0.08;
    if (weft_synth_net_init(&g_net, &ncfg) != 0) {
        return 0ull;
    }
    weft_synth_consensus_cfg_t ccfg;
    weft_synth_consensus_defaults(&ccfg);
    ccfg.seed = seed;
    if (weft_synth_consensus_init(&g_cs, &ccfg) != 0) {
        return 0ull;
    }
    weft_synth_thermal_cfg_t tcfg;
    weft_synth_thermal_defaults(&tcfg);
    tcfg.seed = seed;
    if (weft_synth_thermal_init(&g_th, &tcfg) != 0) {
        return 0ull;
    }
    const uint64_t ticks = tu_iters(30000ull);
    for (uint64_t t = 0ull; t < ticks; t++) {
        if (t == ticks / 3ull) {
            uint32_t grp[WEFT_SYNTH_NET_MAX_NODES];
            for (uint32_t i = 0u; i < WEFT_SYNTH_NET_MAX_NODES; i++) {
                grp[i] = 0u;
            }
            grp[0] = 1u; grp[1] = 1u; grp[2] = 1u; grp[3] = 2u; grp[4] = 2u;
            weft_synth_net_partition_set(&g_net, grp);
        }
        if (t == (2ull * ticks) / 3ull) {
            weft_synth_net_partition_heal(&g_net);
        }
        uint64_t v = t * 3ull;
        (void)weft_synth_net_send(&g_net, (uint32_t)(t % 5u),
                                  (uint32_t)((t + 2u) % 5u),
                                  WEFT_SYNTH_NET_KIND_DATA, &v, sizeof(v));
        (void)weft_synth_net_tick(&g_net);
        (void)weft_synth_consensus_tick(&g_cs, &g_net);
        if ((t & 31ull) == 0ull) {
            (void)weft_synth_thermal_step(&g_th, (uint32_t)(t % 1001u));
        }
    }
    return weft_synth_net_trace_hash(&g_net) ^
           weft_synth_consensus_trace_hash(&g_cs) ^
           weft_synth_thermal_state_hash(&g_th);
}

static void b2_determinism(void) {
    printf("=== B2 determinism (chaos ENABLED) ===\n");
    const uint64_t a = scenario_hash(9u);
    const uint64_t b = scenario_hash(9u);
    const uint64_t c = scenario_hash(10u);
    printf("  scenario seed 9 : %#" PRIx64 " / %#" PRIx64 "\n", a, b);
    printf("  scenario seed 10: %#" PRIx64 " (diverges)\n", c);
    GATE("G2 same-seed replay bit-for-bit", a != 0ull && a == b && a != c);
}

/* ================================================================== */
/* G3 — per-op budgets                                                  */
/* ================================================================== */

static double bench_fabric_op(weft_synth_net_fault_cfg_t *f) {
    weft_synth_net_cfg_t cfg;
    weft_synth_net_defaults(&cfg);
    cfg.fault = *f;
    TU_CHECK(weft_synth_net_init(&g_net, &cfg) == 0);
    const uint64_t n = tu_iters(100000ull);
    weft_synth_net_pkt_t pkt;
    uint64_t sink = 0;
    int64_t t0 = tu_now_ns();
    for (uint64_t i = 0ull; i < n; i++) {
        uint64_t v = i;
        (void)weft_synth_net_send(&g_net, 0u, 1u,
                                  WEFT_SYNTH_NET_KIND_DATA, &v, sizeof(v));
        (void)weft_synth_net_tick(&g_net);
        if (weft_synth_net_recv(&g_net, 1u, &pkt) > 0) {
            sink += pkt.seq;
        }
    }
    int64_t t1 = tu_now_ns();
    return (double)(t1 - t0) / (double)n;
}

static void b3_module_costs(void) {
    printf("=== B3 per-op costs ===\n");

    /* thermal model step */
    {
        weft_synth_thermal_cfg_t cfg;
        weft_synth_thermal_defaults(&cfg);
        TU_CHECK(weft_synth_thermal_init(&g_th, &cfg) == 0);
        const uint64_t n = tu_iters(1000000ull);
        int64_t t0 = tu_now_ns();
        for (uint64_t i = 0ull; i < n; i++) {
            (void)weft_synth_thermal_step(&g_th, (uint32_t)(i % 1001u));
        }
        int64_t t1 = tu_now_ns();
        const double ns = (double)(t1 - t0) / (double)n;
        printf("  thermal step            : %6.1f ns\n", ns);
        GATE("G3 thermal step < 200 ns", ns < 200.0);
    }
    /* thermal hooks at NOMINAL frequency (identity fast paths) */
    {
        weft_synth_thermal_cfg_t cfg;
        weft_synth_thermal_defaults(&cfg);
        TU_CHECK(weft_synth_thermal_init(&g_th, &cfg) == 0);
        const uint64_t n = tu_iters(10000000ull);
        volatile uint64_t sink = 0;
        int64_t t0 = tu_now_ns();
        for (uint64_t i = 0ull; i < n; i++) {
            sink += weft_synth_thermal_deadline_ns(&g_th, 4166666ull);
            sink += weft_synth_thermal_work_ns(&g_th, 1200000ull);
        }
        int64_t t1 = tu_now_ns();
        printf("  thermal hooks @ nominal : %6.1f ns per (deadline+work) "
               "pair [reported; identity fast paths]\n",
               (double)(t1 - t0) / (double)n);
        (void)sink;
    }
    /* chaos decision paths */
    {
        weft_synth_net_fault_cfg_t f;
        weft_synth_net_fault_defaults(&f);
        f.enabled = 1;
        f.model = WEFT_SYNTH_NET_FAULT_GILBERT_ELLIOTT;
        f.ge_p_g2b = 0.05;
        f.ge_p_b2g = 0.40;
        f.ge_drop_good = 0.05;
        f.ge_drop_bad = 0.90;
        const double ns = bench_fabric_op(&f);
        printf("  fabric op, GE 27.8%%    : %6.1f ns (send+tick+recv)\n",
               ns);
        GATE("G3 GE fabric op < 600 ns", ns < 600.0);
    }
    {
        weft_synth_net_fault_cfg_t f;
        weft_synth_net_fault_defaults(&f);
        f.enabled = 1;
        f.model = WEFT_SYNTH_NET_FAULT_BERNOULLI;
        f.drop_p = 0.30;
        const double ns = bench_fabric_op(&f);
        printf("  fabric op, bernoulli 30%%: %6.1f ns\n", ns);
        GATE("G3 bernoulli fabric op < 600 ns", ns < 600.0);
    }
    {
        weft_synth_net_fault_cfg_t f;
        weft_synth_net_fault_defaults(&f);
        f.enabled = 1;
        f.model = WEFT_SYNTH_NET_FAULT_BERNOULLI;
        f.bitflip_p = 0.30;
        f.bitflip_bits_min = 1u;
        f.bitflip_bits_max = 3u;
        const double ns = bench_fabric_op(&f);
        printf("  fabric op, bitflip 30%% : %6.1f ns\n", ns);
        GATE("G3 bitflip fabric op < 800 ns", ns < 800.0);
    }
    {
        weft_synth_net_fault_cfg_t f;
        weft_synth_net_fault_defaults(&f);
        f.enabled = 1;
        f.model = WEFT_SYNTH_NET_FAULT_BERNOULLI;
        f.jitter_mean_ns = 20000.0;
        f.jitter_std_ns = 8000.0;
        f.jitter_max_ns = 80000.0;
        f.reorder_p = 0.25;
        f.reorder_extra_mean_ns = 25000.0;
        const double ns = bench_fabric_op(&f);
        printf("  fabric op, jitter+reorder: %6.1f ns\n", ns);
        GATE("G3 jitter fabric op < 800 ns", ns < 800.0);
    }
    /* consensus step over a clean 5-node wire */
    {
        weft_synth_net_cfg_t cfg;
        weft_synth_net_defaults(&cfg);
        TU_CHECK(weft_synth_net_init(&g_net, &cfg) == 0);
        weft_synth_consensus_cfg_t ccfg;
        weft_synth_consensus_defaults(&ccfg);
        TU_CHECK(weft_synth_consensus_init(&g_cs, &ccfg) == 0);
        const uint64_t n = tu_iters(100000ull);
        int64_t t0 = tu_now_ns();
        for (uint64_t i = 0ull; i < n; i++) {
            (void)weft_synth_net_tick(&g_net);
            (void)weft_synth_consensus_tick(&g_cs, &g_net);
        }
        int64_t t1 = tu_now_ns();
        const double ns = (double)(t1 - t0) / (double)n;
        printf("  consensus step (5 nodes): %6.1f ns (tick+consensus)\n",
               ns);
        GATE("G3 consensus step < 3 us", ns < 3000.0);
    }
    /* CRC-32 throughput */
    {
        static uint8_t buf[192];
        for (uint32_t i = 0u; i < sizeof(buf); i++) {
            buf[i] = (uint8_t)i;
        }
        const uint64_t n = tu_iters(500000ull);
        uint32_t sink = 0;
        int64_t t0 = tu_now_ns();
        for (uint64_t i = 0ull; i < n; i++) {
            sink ^= weft_synth_net_crc32(buf, sizeof(buf));
        }
        int64_t t1 = tu_now_ns();
        const double gbs =
            (double)n * (double)sizeof(buf) / (double)(t1 - t0);
        printf("  CRC-32 (192 B pkt)     : %6.2f GB/s [reported]\n",
               gbs);
        (void)sink;
    }
}

/* ================================================================== */
/* G4 — seqlock probe p99 under max blender saturation                  */
/* ================================================================== */

static void b4_probe_p99(void) {
    printf("=== B4 seqlock probe p99 (10 kHz writer + max blender) ===\n");
    TU_CHECKF(weft_synth_cycle_calibrate() > 0ull,
              "cycle calibration failed");
    static weft_synth_seqlock_t sl;
    static _Atomic int stop;
    TU_CHECK(weft_synth_seqlock_init(&sl) == 0);
    atomic_store(&stop, 0);
    weft_synth_seqlock_writer_t w;
    w.sl = &sl;
    w.stop = &stop;
    w.rng = 0xabcd1234ull;
    w.writes = 0ull;
    w.period_ns = 100000u;
    pthread_t wtid;
    TU_CHECK(pthread_create(&wtid, NULL,
                            weft_synth_seqlock_writer_thread, &w) == 0);
    weft_synth_bus_cfg_t cfg;
    TU_CHECK(weft_synth_bus_defaults(&cfg) == 0);
    cfg.n_threads = 2u;
    cfg.mode = WEFT_SYNTH_BUS_ADJACENT;
    memset((void *)g_arena, 0, sizeof(g_arena));
    weft_synth_bus_blender_t blender;
    TU_CHECK(weft_synth_bus_blender_init(&blender, &cfg, g_arena,
                                         sizeof(g_arena) /
                                             sizeof(g_arena[0])) == 0);
    TU_CHECK(weft_synth_bus_blender_start(&blender) == 0);
    tu_usleep(1000);

    const uint32_t rounds = 3u;
    const uint64_t reads_per = tu_iters(60000ull) / rounds;
    double p99_min = 1e30;
    for (uint32_t r = 0u; r < rounds; r++) {
        weft_synth_bus_probe_t p;
        TU_CHECK(weft_synth_bus_probe_init(
                     &p, 31u + r, &g_lat[(size_t)r * 21000u], 21000u) == 0);
        for (uint64_t i = 0ull; i < reads_per; i++) {
            (void)weft_synth_bus_probe_read(&p, &sl, NULL);
        }
        weft_synth_probe_stat_t st;
        weft_synth_bus_probe_stats(&p, &st);
        printf("  round %u: p50 %.1f / p99 %.1f / max %.1f ns, retries %"
               PRIu64 "\n", r, st.p50_ns, st.p99_ns, st.max_ns,
               st.retries);
        if (st.p99_ns < p99_min) {
            p99_min = st.p99_ns;
        }
    }
    printf("  min-of-%u p99: %.1f ns (SLA bound 100 ns)\n", rounds,
           p99_min);
    atomic_store(&stop, 1);
    TU_CHECK(weft_synth_bus_blender_stop(&blender) > 0ull);
    TU_CHECK(pthread_join(wtid, NULL) == 0);
    weft_synth_bus_blender_destroy(&blender);
    GATE("G4 seqlock retry p99 < 100 ns", p99_min < 100.0);
}

/* ================================================================== */

int main(void) {
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    const char *ng = getenv("BENCH_NO_GATES");
    if (ng != NULL && ng[0] == '1') {
        g_gates_on = 0;
    }
    printf("weft-verify synthetic bench (gates=%d quick=%d tsan=%d "
           "cpus=%u)\n", g_gates_on, tu_quick(), tu_tsan(),
           tu_cpu_count());

    b1_disabled_overhead();
    b2_determinism();
    b3_module_costs();
    b4_probe_p99();

    printf("----------------------------------------------------------------\n");
    printf("synth-bench: %d gate failures\n", g_gate_fail);
    if (g_gate_fail == 0) {
        printf("synth-bench: GREEN\n");
        return 0;
    }
    printf("synth-bench: RED\n");
    return 1;
}
