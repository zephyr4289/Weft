// weft_trend.h — RFC 0020: predictive lag-trend estimator (governor AI).
//
// A 1-D alpha-beta filter in pure integer Q16: level (EWMA of staleness),
// slope (EWMA of its per-step change), projected HORIZON steps ahead, and
// a CLOSED verdict set (STABLE/RISING/FALLING/BURST) with a proactive
// skip_n recommendation. The governor ladder is untouched — this is a
// sensor the consumer MAY consult (Law 3). No float, no alloc, no loops
// (Laws 1-2); observe is a pure step (Law 4).

#ifndef WEFT_TREND_H
#define WEFT_TREND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEFT_TREND_HORIZON      8u    // projection distance (steps)
#define WEFT_TREND_SKIP_MAX     63u   // G5 packing cap (kind<<6 | skip)

/// Verdicts — PROTOCOL values (packed into trace bytes; do not renumber).
typedef enum {
    WEFT_TREND_STABLE  = 0,
    WEFT_TREND_RISING  = 1,
    WEFT_TREND_FALLING = 2,
    WEFT_TREND_BURST   = 3,
} weft_trend_verdict_t;

/// Defaults (RFC-0020 §1-2).
#define WEFT_TREND_ALPHA_Q8_DEFAULT 48u   // 0.1875
#define WEFT_TREND_BETA_Q8_DEFAULT  96u   // 0.375 — fast-trend pair
#define WEFT_TREND_BURST_DEFAULT    32u
#define WEFT_TREND_RISING_DEFAULT   4u   // the governor's Skip rung
#define WEFT_TREND_FALLING_DEFAULT  1u

typedef struct {
    // estimator state (Q16)
    uint32_t level_q16;
    int32_t  slope_q16;    // signed: falling trends have negative slope
    int32_t  last_raw;
    uint8_t  has_last;
    // configuration (Q8 fractions of 256 / raw thresholds)
    uint32_t alpha_q8;
    uint32_t beta_q8;
    uint32_t burst_delta;
    uint32_t rising_behind;
    uint32_t falling_behind;
    // advisory telemetry
    uint64_t t_samples;
    uint64_t t_verdict[4];
} weft_trend_t;

/// One projection result. skip_n is valid for RISING/BURST; 0 otherwise.
typedef struct {
    weft_trend_verdict_t verdict;
    uint32_t pred_raw;     // projected behind at HORIZON steps
    uint32_t skip_n;       // recommended proactive skip
} weft_trend_out_t;

/// Init with the RFC defaults.
void weft_trend_init(weft_trend_t* t);

/// Override the configuration (Q8 gains, thresholds). Values are clamped:
/// gains to [1, 256], burst to [1, 1<<30], thresholds to [0, 1<<20].
void weft_trend_configure(weft_trend_t* t, uint32_t alpha_q8, uint32_t beta_q8,
                          uint32_t burst_delta, uint32_t rising_behind,
                          uint32_t falling_behind);

/// Observe one staleness sample. Pure integer step; writes the projection
/// to *out (may be NULL). Returns the verdict for convenience.
weft_trend_verdict_t weft_trend_observe(weft_trend_t* t, uint32_t behind,
                                        weft_trend_out_t* out);

/// G5-style packed verdict byte: (verdict << 6) | min(skip_n, 63).
uint8_t weft_trend_pack(const weft_trend_out_t* out);

#ifdef __cplusplus
}
#endif

#endif  // WEFT_TREND_H
