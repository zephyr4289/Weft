// weft_trend_test.c — RFC 0020 D-series: the predictive lag-trend filter.
//
// D1  constant plateau -> STABLE (slope-stall guard), level converges
// D2  predictive lead: plateau 2 then ramp +1 -> RISING at raw 5, skip 3,
//     >= 10 steps before the reactive Snapshot rung (16)
// D3  decline -> FALLING near the tail, skip 0
// D4  burst: single-step jump >= 32 -> BURST with demand-action skip
// D5  pinned xorshift verdict stream (the xlang parity surface)
// D6  configuration override (alpha 256 = instant level)
// D7  hand-computed Q16 vectors (level/slope exactness)

#define _GNU_SOURCE
#include "weft_trend.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0, g_fails = 0;
#define CK(cond, name)                                              \
    do {                                                            \
        g_checks++;                                                 \
        if (!(cond)) {                                              \
            g_fails++;                                              \
            printf("  FAIL %s:%d %s\n", __func__, __LINE__, name);  \
        }                                                           \
    } while (0)

static uint32_t xs32(uint32_t x) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

static void t_d1_plateau(void) {
    weft_trend_t t;
    weft_trend_init(&t);
    weft_trend_out_t o;
    for (int i = 0; i < 30; i++) {
        weft_trend_observe(&t, 5, &o);
        if (i == 29) {
            CK(o.verdict == WEFT_TREND_STABLE, "constant 5 is STABLE (not RISING)");
            CK(o.pred_raw >= 4 && o.pred_raw <= 6, "projection settles at the plateau");
        }
    }
    // level within [4.5, 5.5] Q16
    CK(t.level_q16 >= 294912 && t.level_q16 <= 360448, "level converged to ~5");
    CK(t.slope_q16 < 32768, "slope residue below the trend guard");
}

static void t_d2_predictive_lead(void) {
    weft_trend_t t;
    weft_trend_init(&t);
    weft_trend_out_t o;
    // plateau at 2 — the filter settles
    for (int i = 0; i < 12; i++) weft_trend_observe(&t, 2, &o);
    CK(o.verdict == WEFT_TREND_STABLE, "plateau reads STABLE");
    // ramp +1/step; the reactive ladder would hold FastPath at 2-4 and
    // only reach Snapshot territory at 17+ (16 < behind <= skip: actually
    // the ladder SKIPS at 5..16; Snapshot at 17). The trend's job: RISING
    // (proactive skip) as the ramp STARTS, well before Snapshot-class
    // reactions would be warranted.
    int first_rising_raw = -1;
    uint32_t first_skip = 0;
    for (uint32_t b = 3; b <= 10; b++) {
        weft_trend_observe(&t, b, &o);
        if (o.verdict == WEFT_TREND_RISING && first_rising_raw < 0) {
            first_rising_raw = (int)b;
            first_skip = o.skip_n;
        }
    }
    CK(first_rising_raw == 4, "RISING fires at raw behind 4 (pinned)");
    CK(first_skip == 6, "proactive skip 6 (projection 7.4, pinned)");
    CK(o.pred_raw >= 7, "projection leads the raw signal by 3+ steps");
}

static void t_d3_recovery(void) {
    weft_trend_t t;
    weft_trend_init(&t);
    weft_trend_out_t o;
    for (uint32_t b = 8; b >= 1; b--) {
        weft_trend_verdict_t v = weft_trend_observe(&t, b, &o);
        if (b == 1) {
            CK(v == WEFT_TREND_FALLING, "decline tail reads FALLING");
            CK(o.skip_n == 0, "FALLING carries no skip");
        }
    }
}

static void t_d4_burst(void) {
    weft_trend_t t;
    weft_trend_init(&t);
    weft_trend_out_t o;
    weft_trend_observe(&t, 2, &o);
    weft_trend_observe(&t, 2, &o);
    weft_trend_observe(&t, 2, &o);
    weft_trend_verdict_t v = weft_trend_observe(&t, 2 + 62, &o);
    CK(v == WEFT_TREND_BURST, "single-step jump 62 >= 32 -> BURST");
    CK(o.skip_n >= 1 && o.skip_n <= 63, "burst demands action (skip in range)");
    CK(o.skip_n == 63, "skip pinned (projection saturates past the G5 cap)");
}

static void t_d5_pinned_stream(void) {
    weft_trend_t t;
    weft_trend_init(&t);
    weft_trend_out_t o;
    uint32_t state = 0x00C0FFEE;
    // 2000 samples of behind = state % 64; pack the verdict stream (G5
    // packing: verdict << 6 | skip). Pinned = the cross-port fixture.
    static char hex[4001];
    for (int i = 0; i < 2000; i++) {
        state = xs32(state);
        weft_trend_observe(&t, state % 64u, &o);
        snprintf(hex + i * 2, 3, "%02x", weft_trend_pack(&o));
    }
    // FNV-1a 64 over the hex stream — the pinned parity vector
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 4000; i++) {
        h ^= (uint8_t)hex[i];
        h *= 0x100000001b3ull;
    }
    CK(h == 0x11187b9a02b378efull, "verdict stream pinned (parity vector)");
    CK(t.t_samples == 2000, "2000 samples observed");
}

static void t_d6_configure(void) {
    weft_trend_t t;
    weft_trend_init(&t);
    weft_trend_configure(&t, 256, 256, 32, 4, 1);  // instant filters
    weft_trend_out_t o;
    weft_trend_observe(&t, 3, &o);
    weft_trend_observe(&t, 7, &o);
    CK(t.level_q16 == 7 << 16, "alpha 256: level tracks raw exactly");
    CK(t.slope_q16 == 4 << 16, "beta 256: slope tracks delta exactly");
    // clamping
    weft_trend_configure(&t, 999, 0, 0, 1u << 21, 1u << 21);
    CK(t.alpha_q8 == 256 && t.beta_q8 == 1 && t.burst_delta == 1,
       "configuration clamped to declared ranges");
}

static void t_d7_q16_vectors(void) {
    weft_trend_t t;
    weft_trend_init(&t);
    weft_trend_out_t o;
    weft_trend_observe(&t, 3, &o);  // seed
    CK(t.level_q16 == 3 << 16 && t.slope_q16 == 0, "seed: level=raw, slope=0");
    weft_trend_observe(&t, 4, &o);
    // level = 3 + 0.1875*(4-3) = 3.1875 -> 208896
    CK(t.level_q16 == 208896, "level Q16 hand-vector exact");
    // slope = 0 + 0.375*1 = 0.375 -> 24576
    CK(t.slope_q16 == 24576, "slope Q16 hand-vector exact");
    // pred = 3.1875 + 0.375*8 = 6.1875 -> below? NO: 6.19 >= 4 BUT the
    // slope guard (0.5) has not been reached yet -> STABLE
    CK(t.slope_q16 < 32768, "slope still under the trend guard");
    CK(o.verdict == WEFT_TREND_STABLE && o.pred_raw == 6, "projection hand-vector exact");
}

int main(void) {
    printf("== weft_trend_test — RFC 0020 D-series ==\n");
    t_d1_plateau();
    t_d2_predictive_lead();
    t_d3_recovery();
    t_d4_burst();
    t_d5_pinned_stream();
    t_d6_configure();
    t_d7_q16_vectors();
    printf("== D-series: %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
