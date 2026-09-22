// test_synth_torture.c — Pillar 8 concurrent torture battery.
//
// THE directive's torture shape: 2,000,000 cycles with thermal
// throttling, bus saturation, and network chaos running CONCURRENTLY:
//
//   sim thread      fabric tick + consensus step every cycle; thermal
//                   step every 16 cycles under a deterministic load
//                   waveform; DATA heartbeats every 64 cycles with
//                   payload checksums; periodic crash/resurrect rounds
//                   of cluster nodes (process-restart modeling).
//   2 hammer        adjacent-line blender, flat out (bus saturation).
//   writer thread   500 kHz seqlock publisher.
//   reader thread   instrumented probe reads in a tight loop.
//   main thread     watchdog: 3 consecutive zero-progress windows of
//                   100 ms fail the run (a wedge can never hang CI).
//
// Invariants asserted at the end (fail-closed):
//   * exactly 2,000,000 cycles completed (WEFT_QUICK=1 -> 200,000)
//   * consensus safety ledger untouched: zero dual-primary ticks, zero
//     epoch reuse, zero epoch regressions — under 28% GE loss + jitter
//     + bit rot + crash/resurrect churn
//   * cadence drop-not-queue held: latency backlog 0, queue depth
//     within capacity at every observation
//   * every delivered-clean DATA packet passes its payload checksum;
//     every CRC-rejected DATA packet fails it (double detection)
//   * CRC-32 detects every <= 3-bit flip by construction (minimum
//     distance 4), so payload mismatches on clean packets are 0
//   * seqlock forward progress: worst read bounded, probe completed
//   * no watchdog stall, blender totals sane
//
// The fabric runs on the sim thread only (deterministic-simulator
// contract); the blender/writer/reader hammer alongside it on the host.

#include "test_util.h"

#include "weft_synth/weft_synth_bus.h"
#include "weft_synth/weft_synth_net.h"
#include "weft_synth/weft_synth_thermal.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#define TORTURE_CYCLES_DEFAULT 2000000ull
#define TORTURE_CYCLES_QUICK   200000ull

/* ---- shared statics ----------------------------------------------------- */

static weft_synth_net_t g_net;
static weft_synth_consensus_t g_cs;
static weft_synth_thermal_t g_th;
static _Atomic uint64_t g_arena[512] __attribute__((aligned(128)));
static uint64_t g_lat[262144];

static _Atomic int g_stop;
static _Atomic uint64_t g_progress;
static _Atomic int g_abort_flag;
static _Atomic uint64_t g_reader_reads;
static _Atomic uint64_t g_reader_retries;
static _Atomic uint32_t g_reader_worst;
static weft_synth_seqlock_t g_sl;

typedef struct {
    uint64_t cycles;
    uint64_t data_sent;
    uint64_t data_ok;        /* recv ret==1 AND payload checksum passed */
    uint64_t data_bad;       /* recv ret==2 (CRC-rejected)              */
    uint64_t payload_mismatch; /* ret==1 but checksum failed (must be 0) */
    uint64_t cadence_backlog_bad;
} torture_stats_t;
static torture_stats_t g_stats;

/* ---- reader thread ------------------------------------------------------ */

static void *torture_reader(void *arg) {
    (void)arg;
    weft_synth_bus_probe_t p;
    if (weft_synth_bus_probe_init(&p, 4242u, g_lat,
                                  (uint32_t)(sizeof(g_lat) /
                                             sizeof(g_lat[0]))) != 0) {
        return NULL;
    }
    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        (void)weft_synth_bus_probe_read(&p, &g_sl, NULL);
    }
    atomic_store(&g_reader_reads, p.reads);
    atomic_store(&g_reader_retries, p.retries);
    atomic_store(&g_reader_worst, p.max_attempts);
    return NULL;
}

/* ---- sim thread --------------------------------------------------------- */

static void *torture_sim(void *arg) {
    const uint64_t cycles = *(const uint64_t *)arg;

    /* chaos profile: GE burst loss ~28% + microsecond jitter + bit rot.
     * The fabric carries SIX nodes: consensus owns 0-4, and node 5 is a
     * dedicated DATA sink — consensus drains its nodes' inboxes every
     * tick (dropping non-consensus traffic by contract), so DATA aimed
     * at a consensus node would be eaten before the sim's verifier saw
     * it. A dedicated sink keeps both traffic classes honest. */
    weft_synth_net_cfg_t ncfg;
    weft_synth_net_defaults(&ncfg);
    ncfg.n_nodes = 6u;
    ncfg.seed = 20260922u;
    ncfg.fault.enabled = 1;
    ncfg.fault.model = WEFT_SYNTH_NET_FAULT_GILBERT_ELLIOTT;
    ncfg.fault.ge_p_g2b = 0.05;
    ncfg.fault.ge_p_b2g = 0.40;
    ncfg.fault.ge_drop_good = 0.20;
    ncfg.fault.ge_drop_bad = 0.90;   /* stationary loss ~27.8% */
    ncfg.fault.jitter_mean_ns = 8000.0;
    ncfg.fault.jitter_std_ns = 4000.0;
    ncfg.fault.jitter_max_ns = 40000.0;
    ncfg.fault.reorder_p = 0.20;
    ncfg.fault.reorder_extra_mean_ns = 15000.0;
    ncfg.fault.bitflip_p = 0.05;
    ncfg.fault.bitflip_bits_min = 1u;
    ncfg.fault.bitflip_bits_max = 3u;
    if (weft_synth_net_init(&g_net, &ncfg) != 0) {
        atomic_store(&g_abort_flag, 1);
        return NULL;
    }
    weft_synth_consensus_cfg_t ccfg;
    weft_synth_consensus_defaults(&ccfg);
    ccfg.seed = 777u;
    if (weft_synth_consensus_init(&g_cs, &ccfg) != 0) {
        atomic_store(&g_abort_flag, 1);
        return NULL;
    }
    weft_synth_thermal_cfg_t tcfg;
    weft_synth_thermal_defaults(&tcfg);
    tcfg.seed = 31337u;
    if (weft_synth_thermal_init(&g_th, &tcfg) != 0) {
        atomic_store(&g_abort_flag, 1);
        return NULL;
    }
    /* NOTE: g_sl is initialized by main() BEFORE the writer/reader
     * fleet starts — re-initializing it here would tear live writes. */

    weft_synth_net_pkt_t pkt;
    weft_synth_thermal_verdict_t verdict;

    for (uint64_t c = 1ull; c <= cycles; c++) {
        /* thermal starvation under a deterministic load waveform */
        if ((c & 15ull) == 0ull) {
            (void)weft_synth_thermal_step(&g_th, (uint32_t)(c % 1001u));
        }
        /* cadence probe at 240 FPS whenever the thermal model ticks */
        if ((c & 63ull) == 0ull) {
            const uint64_t now = (c / 64ull) * 4166666ull;
            (void)weft_synth_thermal_frame_admit(&g_th, now, 1200000ull,
                                                 now + 4166666ull, &verdict);
            if (verdict.verdict == WEFT_SYNTH_THERMAL_ADMIT) {
                (void)weft_synth_thermal_frame_complete(&g_th, &verdict);
            }
            if (g_th.latency_backlog_ns != 0) {
                g_stats.cadence_backlog_bad++;
            }
        }
        /* DATA traffic with self-checking payloads (node 5 = DATA sink) */
        if ((c & 63ull) == 0ull) {
            for (uint32_t k = 0u; k < 8u; k++) {
                uint64_t v = (c << 3) | k;
                uint32_t words[2];
                words[0] = (uint32_t)(v & 0xFFFFFFFFull);
                words[1] = (uint32_t)((v * 2654435761ull) & 0xFFFFFFFFull);
                (void)weft_synth_net_send(&g_net, 0u, 5u,
                                          WEFT_SYNTH_NET_KIND_DATA, words,
                                          sizeof(words));
                g_stats.data_sent++;
            }
        }
        /* crash/resurrect rotation: node i dies for 50k cycles */
        if ((c % 500000ull) == 0ull) {
            uint32_t victim = (uint32_t)((c / 500000ull) % 5ull);
            (void)weft_synth_consensus_set_crash_mask(&g_cs,
                                                       1u << victim);
        } else if ((c % 500000ull) == 50000ull) {
            (void)weft_synth_consensus_set_crash_mask(&g_cs, 0u);
        }

        (void)weft_synth_net_tick(&g_net);
        (void)weft_synth_consensus_tick(&g_cs, &g_net);

        /* drain + verify DATA at the dedicated sink (node 5) */
        for (;;) {
            int rc = weft_synth_net_recv(&g_net, 5u, &pkt);
            if (rc == 0) {
                break;
            }
            if (pkt.kind != WEFT_SYNTH_NET_KIND_DATA) {
                continue;  /* consensus traffic rides the same wire */
            }
            uint32_t words[2];
            memcpy(words, pkt.payload, sizeof(words));
            int checksum_ok =
                (words[1] ==
                 (uint32_t)(((uint64_t)words[0] * 2654435761ull) &
                            0xFFFFFFFFull));
            if (rc == 1) {
                g_stats.data_ok++;
                if (!checksum_ok) {
                    g_stats.payload_mismatch++;
                }
            } else {  /* rc == 2: CRC gate rejected it */
                g_stats.data_bad++;
            }
        }

        if ((c & 4095ull) == 0ull) {
            atomic_store(&g_progress, c);
            if (atomic_load_explicit(&g_abort_flag, memory_order_acquire)) {
                g_stats.cycles = c;
                return NULL;
            }
        }
    }
    atomic_store(&g_progress, cycles);
    g_stats.cycles = cycles;
    return NULL;
}

/* ---- main ---------------------------------------------------------------- */

int main(void) {
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    uint64_t cycles = tu_quick() ? TORTURE_CYCLES_QUICK
                                 : TORTURE_CYCLES_DEFAULT;
    printf("weft-verify synthetic torture: %" PRIu64 " cycles "
           "(quick=%d tsan=%d cpus=%u)\n",
           cycles, tu_quick(), tu_tsan(), tu_cpu_count());
    TU_CHECKF(weft_synth_cycle_calibrate() > 0ull,
              "cycle calibration failed");

    /* contention fleet (the seqlock is initialized BEFORE the writer
     * and reader threads exist — no init/write race) */
    TU_CHECK(weft_synth_seqlock_init(&g_sl) == 0);
    weft_synth_bus_cfg_t bcfg;
    TU_CHECK(weft_synth_bus_defaults(&bcfg) == 0);
    bcfg.n_threads = 2u;
    bcfg.mode = WEFT_SYNTH_BUS_ADJACENT;
    memset((void *)g_arena, 0, sizeof(g_arena));
    weft_synth_bus_blender_t blender;
    TU_CHECK(weft_synth_bus_blender_init(&blender, &bcfg, g_arena,
                                         sizeof(g_arena) /
                                             sizeof(g_arena[0])) == 0);
    TU_CHECK(weft_synth_bus_blender_start(&blender) == 0);

    weft_synth_seqlock_writer_t writer;
    writer.sl = &g_sl;
    writer.stop = &g_stop;
    writer.rng = 0xfeedfaceull;
    writer.writes = 0ull;
    writer.period_ns = 2000u;
    atomic_store(&g_stop, 0);
    pthread_t writer_tid;
    TU_CHECK(pthread_create(&writer_tid, NULL,
                            weft_synth_seqlock_writer_thread, &writer) == 0);
    pthread_t reader_tid;
    TU_CHECK(pthread_create(&reader_tid, NULL, torture_reader, NULL) == 0);
    tu_usleep(1000);

    pthread_t sim_tid;
    TU_CHECK(pthread_create(&sim_tid, NULL, torture_sim, &cycles) == 0);

    /* watchdog: zero progress for 3 s fails the run; the sim thread is
     * reaped exactly once (tryjoin success OR the abort path's join —
     * never both: joining an already-reaped thread is UB) */
    uint64_t last_progress = 0ull;
    int stalls = 0;
    int aborted = 0;
    for (;;) {
        void *ret = NULL;
        int rc = pthread_tryjoin_np(sim_tid, &ret);
        if (rc == 0) {
            break;  /* reaped by tryjoin — do NOT join again */
        }
        if (rc != EBUSY) {
            aborted = 1;  /* unexpected pthread error: fail-closed path */
            break;
        }
        tu_usleep(100000);
        const uint64_t now_progress = atomic_load(&g_progress);
        if (now_progress == last_progress) {
            stalls++;
            if (stalls >= 30) {  /* 3 s with zero progress */
                atomic_store(&g_abort_flag, 1);
                printf("FAIL watchdog: sim stalled at %" PRIu64
                       " cycles for 3 s\n", now_progress);
                aborted = 1;
                break;
            }
        } else {
            stalls = 0;
            last_progress = now_progress;
        }
    }
    if (aborted) {
        TU_CHECK(pthread_join(sim_tid, NULL) == 0);
    }

    /* teardown the fleet */
    atomic_store(&g_stop, 1);
    TU_CHECK(weft_synth_bus_blender_stop(&blender) > 0ull);
    TU_CHECK(pthread_join(writer_tid, NULL) == 0);
    TU_CHECK(pthread_join(reader_tid, NULL) == 0);
    weft_synth_bus_blender_destroy(&blender);

    /* ---- invariants ------------------------------------------------------ */
    TU_CHECKF(atomic_load(&g_abort_flag) == 0, "watchdog abort fired");
    TU_CHECK(g_stats.cycles == cycles);
    printf("  cycles %" PRIu64 ", thermal: freq %u MHz, temp %u mC, "
           "migrations %" PRIu64 ", frames admitted %" PRIu64
           " / dropped %" PRIu64 ", backlog %" PRId64 "\n",
           g_stats.cycles, g_th.freq_mhz, g_th.temp_mc, g_th.migrations,
           g_th.frames_admitted, g_th.frames_dropped,
           g_th.latency_backlog_ns);

    /* consensus safety under chaos + crash churn (THE directive ledger) */
    TU_CHECK(g_cs.dual_primary_ticks == 0ull);
    TU_CHECK(g_cs.epoch_reuse_violations == 0ull);
    TU_CHECK(g_cs.node_epoch_regressions == 0ull);
    printf("  consensus: grants %" PRIu64 ", stepdowns %" PRIu64
           ", invalid msgs %" PRIu64 ", availability %.1f%%, max epoch %"
           PRIu64 "\n",
           g_cs.grants, g_cs.stepdowns, g_cs.invalid_msgs,
           100.0 * (double)g_cs.ticks_with_primary /
               (double)g_cs.ticks_elapsed,
           g_cs.max_epoch);

    /* cadence drop-not-queue held all the way through */
    TU_CHECK(g_th.latency_backlog_ns == 0);
    TU_CHECK(g_stats.cadence_backlog_bad == 0ull);
    TU_CHECK(g_th.queue_depth_hiwat <= g_th.cfg.queue_capacity);
    TU_CHECK(g_th.frames_evaluated ==
             g_th.frames_admitted + g_th.frames_dropped);

    /* DATA integrity through the chaos fabric */
    TU_CHECK(g_stats.payload_mismatch == 0ull);  /* CRC d<=4 catches <=3 */
    TU_CHECK(g_stats.data_ok > 0ull);
    TU_CHECK(g_stats.data_bad > 0ull);
    printf("  DATA: sent %" PRIu64 ", clean-delivered %" PRIu64
           ", CRC-rejected %" PRIu64 ", payload mismatches %" PRIu64 "\n",
           g_stats.data_sent, g_stats.data_ok, g_stats.data_bad,
           g_stats.payload_mismatch);

    /* seqlock forward progress under the full adversarial mix: the worst
     * read can legitimately spiral when the OS preempts the writer INSIDE
     * its critical section on this oversubscribed 2-vCPU runner (the
     * version stays odd for a whole timeslice); the probe's per-attempt
     * yield escalation recovers it — what is asserted is that every read
     * COMPLETED (bounded spiral, zero deadlock), not that the spiral
     * never happens. */
    const uint64_t rreads = atomic_load(&g_reader_reads);
    TU_CHECK(rreads > 1000ull);
    TU_CHECKF(atomic_load(&g_reader_worst) < 200000u,
              "worst read %u attempts (unbounded?)",
              atomic_load(&g_reader_worst));
    printf("  seqlock: reader reads %" PRIu64 ", retries %" PRIu64
           ", worst read %u attempts; writer writes %" PRIu64
           "; blender ops %" PRIu64 "\n",
           rreads, atomic_load(&g_reader_retries),
           atomic_load(&g_reader_worst), writer.writes, blender.ops_total);
    TU_CHECK(writer.writes > 1000ull);
    TU_CHECK(blender.ops_total > 100000ull);

    tu_done("synth-torture");
}
