// weft_synth_stress.c — standalone Pillar 8 chaos-stress CLI.
//
// Runs the full synthetic-lab stack — thermal throttling, bus saturation,
// network chaos with WCR1-style lease consensus, and the seqlock
// writer/reader probe — concurrently, for a wall-clock duration, then
// prints a summary and exits nonzero if any invariant broke. This is the
// operator-facing harness: every knob is a flag, every number is real.
//
// Usage:
//   weft_synth_stress [--ms N] [--threads N] [--seed S] [--loss none|ge|bern]
//                     [--drop P] [--mode all|thermal|bus|net] [--quiet]
//
// Examples:
//   weft_synth_stress                       # 2 s, defaults, GE ~28% loss
//   weft_synth_stress --ms 10000 --loss bern --drop 0.45   # extreme soak

#include "weft_synth/weft_synth_bus.h"
#include "weft_synth/weft_synth_net.h"
#include "weft_synth/weft_synth_thermal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t ticks;
    uint64_t data_sent, data_ok, data_bad, data_mismatch;
    uint64_t hammer_ops;
    uint64_t reader_reads, reader_retries;
    uint32_t reader_worst;
    uint64_t writer_writes;
} stress_summary_t;

static weft_synth_net_t g_net;
static weft_synth_consensus_t g_cs;
static weft_synth_thermal_t g_th;
static weft_synth_seqlock_t g_sl;
static _Atomic uint64_t g_arena[512] __attribute__((aligned(128)));
static uint64_t g_lat[65536];
static _Atomic int g_stop;
static stress_summary_t g_sum;
static char g_loss[8] = "ge";     /* none | ge | bern */
static double g_drop = 0.30;      /* bernoulli p or ge drop_good */

/* ---- reader probe thread ------------------------------------------------ */

static void *stress_reader(void *arg) {
    (void)arg;
    weft_synth_bus_probe_t p;
    if (weft_synth_bus_probe_init(&p, 31415u, g_lat,
                                  (uint32_t)(sizeof(g_lat) /
                                             sizeof(g_lat[0]))) != 0) {
        return NULL;
    }
    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        (void)weft_synth_bus_probe_read(&p, &g_sl, NULL);
    }
    g_sum.reader_reads = p.reads;
    g_sum.reader_retries = p.retries;
    g_sum.reader_worst = p.max_attempts;
    return NULL;
}

/* ---- the simulation thread ---------------------------------------------- */

static void *stress_sim(void *arg) {
    const int64_t deadline_ns = *(const int64_t *)arg;

    weft_synth_net_cfg_t ncfg;
    weft_synth_net_defaults(&ncfg);
    ncfg.n_nodes = 6u;
    ncfg.seed = 4242u;
    ncfg.fault.enabled = (strcmp(g_loss, "none") != 0);
    if (strcmp(g_loss, "bern") == 0) {
        ncfg.fault.model = WEFT_SYNTH_NET_FAULT_BERNOULLI;
        ncfg.fault.drop_p = g_drop;
    } else {  /* "ge": stationary loss = .889*drop + .111*.90 */
        ncfg.fault.model = WEFT_SYNTH_NET_FAULT_GILBERT_ELLIOTT;
        ncfg.fault.ge_p_g2b = 0.05;
        ncfg.fault.ge_p_b2g = 0.40;
        ncfg.fault.ge_drop_good = g_drop;
        ncfg.fault.ge_drop_bad = 0.90;
    }
    ncfg.fault.jitter_mean_ns = 8000.0;
    ncfg.fault.jitter_std_ns = 4000.0;
    ncfg.fault.jitter_max_ns = 40000.0;
    ncfg.fault.reorder_p = 0.20;
    ncfg.fault.reorder_extra_mean_ns = 15000.0;
    ncfg.fault.bitflip_p = 0.05;
    ncfg.fault.bitflip_bits_min = 1u;
    ncfg.fault.bitflip_bits_max = 3u;
    if (weft_synth_net_init(&g_net, &ncfg) != 0) {
        return NULL;
    }
    weft_synth_consensus_cfg_t ccfg;
    weft_synth_consensus_defaults(&ccfg);
    ccfg.seed = 99u;
    if (weft_synth_consensus_init(&g_cs, &ccfg) != 0) {
        return NULL;
    }
    weft_synth_thermal_cfg_t tcfg;
    weft_synth_thermal_defaults(&tcfg);
    tcfg.seed = 5u;
    if (weft_synth_thermal_init(&g_th, &tcfg) != 0) {
        return NULL;
    }

    weft_synth_net_pkt_t pkt;
    weft_synth_thermal_verdict_t verdict;
    uint64_t c = 0ull;
    while (weft_synth_now_ns() < deadline_ns &&
           !atomic_load_explicit(&g_stop, memory_order_acquire)) {
        if ((c & 15ull) == 0ull) {
            (void)weft_synth_thermal_step(&g_th, (uint32_t)(c % 1001u));
        }
        if ((c & 63ull) == 0ull) {
            const uint64_t now = (c / 64ull) * 4166666ull;
            (void)weft_synth_thermal_frame_admit(&g_th, now, 1200000ull,
                                                 now + 4166666ull, &verdict);
            if (verdict.verdict == WEFT_SYNTH_THERMAL_ADMIT) {
                (void)weft_synth_thermal_frame_complete(&g_th, &verdict);
            }
            for (uint32_t k = 0u; k < 8u; k++) {
                uint64_t v = (c << 3) | k;
                uint32_t words[2];
                words[0] = (uint32_t)(v & 0xFFFFFFFFull);
                words[1] =
                    (uint32_t)((v * 2654435761ull) & 0xFFFFFFFFull);
                (void)weft_synth_net_send(&g_net, 0u, 5u,
                                          WEFT_SYNTH_NET_KIND_DATA, words,
                                          sizeof(words));
                g_sum.data_sent++;
            }
        }
        (void)weft_synth_net_tick(&g_net);
        (void)weft_synth_consensus_tick(&g_cs, &g_net);
        for (;;) {
            int rc = weft_synth_net_recv(&g_net, 5u, &pkt);
            if (rc == 0) {
                break;
            }
            if (pkt.kind != WEFT_SYNTH_NET_KIND_DATA) {
                continue;
            }
            uint32_t words[2];
            memcpy(words, pkt.payload, sizeof(words));
            int ok = (words[1] ==
                      (uint32_t)(((uint64_t)words[0] * 2654435761ull) &
                                 0xFFFFFFFFull));
            if (rc == 1) {
                g_sum.data_ok++;
                if (!ok) {
                    g_sum.data_mismatch++;
                }
            } else {
                g_sum.data_bad++;
            }
        }
        c++;
    }
    g_sum.ticks = c;
    return NULL;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv) {
    int64_t ms = 2000;
    uint32_t threads = 2u;
    uint32_t seed = 1u;
    const char *loss = "ge";
    double drop = 0.30;
    int quiet = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ms") == 0 && i + 1 < argc) {
            ms = strtoll(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--loss") == 0 && i + 1 < argc) {
            loss = argv[++i];
        } else if (strcmp(argv[i], "--drop") == 0 && i + 1 < argc) {
            drop = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [--ms N] [--threads N] [--seed S] "
                   "[--loss none|ge|bern] [--drop P] [--quiet]\n",
                   argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown flag: %s\n", argv[i]);
            return 2;
        }
    }
    if (threads == 0u || threads > WEFT_SYNTH_BUS_MAX_THREADS) {
        threads = 2u;
    }
    /* the sim seeds are fixed for replayability; --seed is reserved */
    (void)seed;
    snprintf(g_loss, sizeof(g_loss), "%s", loss);
    g_drop = (drop > 0.0 && drop < 1.0) ? drop : 0.30;

    if (!quiet) {
        printf("weft-synth stress: %lld ms, %u hammer threads, "
               "loss=%s (%.0f%%) + jitter + bit rot\n",
               (long long)ms, threads, g_loss, g_drop * 100.0);
    }
    (void)weft_synth_cycle_calibrate();

    /* fleet */
    if (weft_synth_seqlock_init(&g_sl) != 0) {
        return 1;
    }
    weft_synth_bus_cfg_t bcfg;
    (void)weft_synth_bus_defaults(&bcfg);
    bcfg.n_threads = threads;
    bcfg.mode = WEFT_SYNTH_BUS_ADJACENT;
    memset((void *)g_arena, 0, sizeof(g_arena));
    weft_synth_bus_blender_t blender;
    if (weft_synth_bus_blender_init(&blender, &bcfg, g_arena,
                                    sizeof(g_arena) /
                                        sizeof(g_arena[0])) != 0) {
        return 1;
    }
    if (weft_synth_bus_blender_start(&blender) != 0) {
        return 1;
    }
    weft_synth_seqlock_writer_t writer;
    writer.sl = &g_sl;
    writer.stop = &g_stop;
    writer.rng = 0x77ull;
    writer.writes = 0ull;
    writer.period_ns = 2000u;
    atomic_store(&g_stop, 0);
    pthread_t wtid;
    if (pthread_create(&wtid, NULL, weft_synth_seqlock_writer_thread,
                       &writer) != 0) {
        return 1;
    }
    pthread_t rtid;
    if (pthread_create(&rtid, NULL, stress_reader, NULL) != 0) {
        return 1;
    }

    int64_t deadline = weft_synth_now_ns() + ms * 1000000ll;
    pthread_t stid;
    if (pthread_create(&stid, NULL, stress_sim, &deadline) != 0) {
        return 1;
    }
    pthread_join(stid, NULL);

    atomic_store(&g_stop, 1);
    g_sum.hammer_ops = weft_synth_bus_blender_stop(&blender);
    g_sum.writer_writes = writer.writes;
    pthread_join(wtid, NULL);
    pthread_join(rtid, NULL);
    weft_synth_bus_blender_destroy(&blender);

    /* summary + invariants */
    int bad = 0;
    if (g_cs.dual_primary_ticks != 0ull ||
        g_cs.epoch_reuse_violations != 0ull ||
        g_cs.node_epoch_regressions != 0ull) {
        bad = 1;
    }
    if (g_th.latency_backlog_ns != 0 ||
        g_th.queue_depth_hiwat > g_th.cfg.queue_capacity) {
        bad = 1;
    }
    if (g_sum.data_mismatch != 0ull) {
        bad = 1;
    }

    printf("summary:\n");
    printf("  virtual ticks      : %" PRIu64 "\n", g_sum.ticks);
    printf("  consensus          : grants %" PRIu64 ", availability "
           "%.1f%%, dual-primary %" PRIu64 ", epoch-reuse %" PRIu64
           ", regressions %" PRIu64 "\n",
           g_cs.grants,
           g_cs.ticks_elapsed > 0ull
               ? 100.0 * (double)g_cs.ticks_with_primary /
                     (double)g_cs.ticks_elapsed
               : 0.0,
           g_cs.dual_primary_ticks, g_cs.epoch_reuse_violations,
           g_cs.node_epoch_regressions);
    printf("  thermal            : %u MHz, backlog %" PRId64
           ", frames admitted %" PRIu64 " / dropped %" PRIu64 "\n",
           g_th.freq_mhz, g_th.latency_backlog_ns, g_th.frames_admitted,
           g_th.frames_dropped);
    printf("  DATA               : sent %" PRIu64 ", clean %" PRIu64
           ", CRC-rejected %" PRIu64 ", payload mismatches %" PRIu64 "\n",
           g_sum.data_sent, g_sum.data_ok, g_sum.data_bad,
           g_sum.data_mismatch);
    printf("  seqlock            : reads %" PRIu64 ", retries %" PRIu64
           ", worst read %u attempts\n",
           g_sum.reader_reads, g_sum.reader_retries, g_sum.reader_worst);
    printf("  fleet              : writer writes %" PRIu64
           ", hammer ops %" PRIu64 "\n",
           g_sum.writer_writes, g_sum.hammer_ops);
    printf("verdict: %s\n", bad ? "INVARIANT VIOLATION" : "ALL INVARIANTS HELD");
    return bad ? 1 : 0;
}
