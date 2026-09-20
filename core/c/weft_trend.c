// weft_trend.c — RFC 0020: predictive lag-trend estimator, C reference.
//
// The arithmetic is NORMATIVE (RFC-0020 §1-2); every port mirrors it
// exactly, including the saturating shift and the verdict ordering.

#include "weft_trend.h"

#include <string.h>

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void weft_trend_init(weft_trend_t* t) {
    if (t == NULL) return;
    memset(t, 0, sizeof(*t));
    t->alpha_q8 = WEFT_TREND_ALPHA_Q8_DEFAULT;
    t->beta_q8 = WEFT_TREND_BETA_Q8_DEFAULT;
    t->burst_delta = WEFT_TREND_BURST_DEFAULT;
    t->rising_behind = WEFT_TREND_RISING_DEFAULT;
    t->falling_behind = WEFT_TREND_FALLING_DEFAULT;
}

void weft_trend_configure(weft_trend_t* t, uint32_t alpha_q8, uint32_t beta_q8,
                          uint32_t burst_delta, uint32_t rising_behind,
                          uint32_t falling_behind) {
    if (t == NULL) return;
    t->alpha_q8 = clamp_u32(alpha_q8, 1, 256);
    t->beta_q8 = clamp_u32(beta_q8, 1, 256);
    t->burst_delta = clamp_u32(burst_delta, 1, 1u << 30);
    t->rising_behind = clamp_u32(rising_behind, 0, 1u << 20);
    t->falling_behind = clamp_u32(falling_behind, 0, 1u << 20);
}

weft_trend_verdict_t weft_trend_observe(weft_trend_t* t, uint32_t behind,
                                        weft_trend_out_t* out) {
    if (t == NULL) return WEFT_TREND_STABLE;
    weft_trend_out_t o;
    memset(&o, 0, sizeof(o));

    if (!t->has_last) {
        // first sample seeds level, zero slope (declared: no prediction
        // is possible from one sample — STABLE until the filter settles)
        t->level_q16 = behind << 16;
        t->slope_q16 = 0;
        t->last_raw = (int32_t)behind;
        t->has_last = 1;
        o.verdict = WEFT_TREND_STABLE;
        o.pred_raw = behind;
        t->t_samples++;
        t->t_verdict[WEFT_TREND_STABLE]++;
        if (out) *out = o;
        return o.verdict;
    }

    int32_t delta_raw = (int32_t)behind - t->last_raw;
    t->last_raw = (int32_t)behind;

    // level += alpha * (raw - level)   (Q16, gain Q8)
    int32_t level_err = (int32_t)(behind << 16) - (int32_t)t->level_q16;
    t->level_q16 = (uint32_t)((int64_t)t->level_q16 +
                              (((int64_t)t->alpha_q8 * level_err) >> 8));
    // slope += beta * (delta - slope)  (Q16 per step)
    int64_t delta_q16 = (int64_t)delta_raw << 16;
    t->slope_q16 = (int32_t)((int64_t)t->slope_q16 +
                             (((int64_t)t->beta_q8 * (delta_q16 - t->slope_q16)) >> 8));

    // projection: level + slope * HORIZON (saturating shift)
    int64_t pred_q16 = (int64_t)t->level_q16 +
                       (int64_t)t->slope_q16 * WEFT_TREND_HORIZON;
    if (pred_q16 < 0) pred_q16 = 0;
    if (pred_q16 > (int64_t)0x7FFFFFFF) pred_q16 = 0x7FFFFFFF;
    uint32_t pred_raw = (uint32_t)(pred_q16 >> 16);
    o.pred_raw = pred_raw;

    // Verdict order is NORMATIVE (RFC-0020 §2): burst, rising, falling,
    // stable. RISING/FALLING additionally require a REAL trend (|slope|
    // >= half a step per step) — the EWMA slope stalls at a small nonzero
    // residue on a constant signal, and a plateau must read STABLE.
    if (delta_raw >= 0 && (uint32_t)delta_raw >= t->burst_delta) {
        o.verdict = WEFT_TREND_BURST;
        o.skip_n = pred_raw > WEFT_TREND_SKIP_MAX ? WEFT_TREND_SKIP_MAX : pred_raw;
        if (o.skip_n == 0) o.skip_n = 1;  // a burst demands action
    } else if (pred_raw >= t->rising_behind && t->slope_q16 >= 32768) {
        o.verdict = WEFT_TREND_RISING;
        uint32_t skip = pred_raw >= 1 ? pred_raw - 1 : 0;
        o.skip_n = skip > WEFT_TREND_SKIP_MAX ? WEFT_TREND_SKIP_MAX : skip;
    } else if (pred_raw <= t->falling_behind && t->slope_q16 <= -32768) {
        o.verdict = WEFT_TREND_FALLING;
    } else {
        o.verdict = WEFT_TREND_STABLE;
    }

    t->t_samples++;
    t->t_verdict[o.verdict]++;
    if (out) *out = o;
    return o.verdict;
}

uint8_t weft_trend_pack(const weft_trend_out_t* out) {
    if (out == NULL) return 0;
    uint32_t skip = out->skip_n > WEFT_TREND_SKIP_MAX ? WEFT_TREND_SKIP_MAX
                                                      : out->skip_n;
    return (uint8_t)(((uint32_t)out->verdict << 6) | skip);
}
