// weft_synth_thermal_curve.c — Pillar 8 thermal throttler response-curve
// generator (the D-82 audit evidence "thermal response curves").
//
// Drives the synthetic thermal engine through a chosen load profile and
// prints a CSV trace on stdout: model tick, temperature, DVFS frequency,
// active core, event bitmask, migration count, and the running cadence
// drop-not-queue ledger (frames admitted / dropped so far, queue depth).
//
// Usage:
//   weft_synth_thermal_curve [--ticks N] [--profile full|burst|wave|idle]
//                            [--seed S] [--jitter-pm P]
//
// Profiles:
//   full  : sustained 100% load — throttle to the floor, hold, recover
//   burst : 100%/10% alternating every 300 ticks — burst thermal patterns
//   wave  : deterministic sawtooth 0..100% — worst-case DVFS chatter
//   idle  : idle machine (ambient + jitter only)

#include "weft_synth/weft_synth_thermal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t load_for(const char *profile, uint32_t t) {
    if (strcmp(profile, "idle") == 0) {
        return 0u;
    }
    if (strcmp(profile, "burst") == 0) {
        return ((t / 300u) % 2u == 0u) ? 1000u : 100u;
    }
    if (strcmp(profile, "wave") == 0) {
        return (t * 7u) % 1001u;
    }
    return 1000u;  /* full */
}

int main(int argc, char **argv) {
    uint32_t ticks = 1200u;
    uint32_t seed = 1u;
    uint32_t jitter_pm = 20u;
    const char *profile = "full";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            ticks = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--jitter-pm") == 0 && i + 1 < argc) {
            jitter_pm = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            profile = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [--ticks N] [--profile full|burst|wave|idle]"
                   " [--seed S] [--jitter-pm P]\n",
                   argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown flag: %s\n", argv[i]);
            return 2;
        }
    }
    if (ticks == 0u || ticks > 1000000u) {
        ticks = 1200u;
    }

    weft_synth_thermal_cfg_t cfg;
    if (weft_synth_thermal_defaults(&cfg) != 0) {
        return 1;
    }
    cfg.seed = seed;
    cfg.clock_jitter_pm = jitter_pm;
    weft_synth_thermal_t th;
    if (weft_synth_thermal_init(&th, &cfg) != 0) {
        return 1;
    }

    printf("# weft-synth thermal response curve\n");
    printf("# profile=%s seed=%u jitter_pm=%u tick_ns=%llu\n",
           profile, seed, jitter_pm,
           (unsigned long long)cfg.tick_ns);
    printf("tick_ms,temp_c,freq_mhz,active_core,events,migrations,"
           "frames_admitted,frames_dropped,queue_depth,backlog_ns\n");

    weft_synth_thermal_verdict_t v;
    const uint64_t frame_period = 4166666ull;   /* 240 FPS */
    const uint64_t base_work = 1200000ull;      /* 1.2 ms at full clock */
    for (uint32_t t = 1u; t <= ticks; t++) {
        const uint32_t ev = weft_synth_thermal_step(&th,
                                                    load_for(profile, t));
        /* a 240 FPS cadence probe every 4 model ticks (4 ms) */
        if ((t & 3u) == 0u) {
            const uint64_t now = (uint64_t)(t / 4u) * frame_period;
            (void)weft_synth_thermal_frame_admit(&th, now, base_work,
                                                 now + frame_period, &v);
            if (v.verdict == WEFT_SYNTH_THERMAL_ADMIT) {
                (void)weft_synth_thermal_frame_complete(&th, &v);
            }
        }
        printf("%u.%03u,%.1f,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64
               ",%u,%" PRId64 "\n",
               t / 1000u, t % 1000u,
               (double)th.temp_mc / 1000.0,
               weft_synth_thermal_freq_mhz(&th),
               th.active_core, ev, th.migrations,
               th.frames_admitted, th.frames_dropped, th.queue_depth,
               th.latency_backlog_ns);
    }
    fprintf(stderr, "curve complete: %u ticks, final %u MHz @ %.1f C, "
            "admitted %" PRIu64 " / dropped %" PRIu64 ", backlog %"
            PRId64 "\n",
            ticks, th.freq_mhz, (double)th.temp_mc / 1000.0,
            th.frames_admitted, th.frames_dropped,
            th.latency_backlog_ns);
    return (th.latency_backlog_ns == 0 &&
            th.queue_depth_hiwat <= th.cfg.queue_capacity)
               ? 0
               : 1;
}
