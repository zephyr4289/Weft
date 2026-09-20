//! # RFC 0020 predictive lag-trend estimator — Rust port.
//!
//! Mirror of `core/c/weft_trend.{h,c}` (NORMATIVE): a 1-D alpha-beta
//! filter in pure integer Q16 — level (EWMA of staleness), slope (EWMA of
//! its per-step change), projected HORIZON steps ahead, and a CLOSED
//! verdict set (STABLE/RISING/FALLING/BURST) with a proactive skip_n
//! recommendation. The governor ladder is untouched — this is a sensor
//! the consumer MAY consult (Law 3). No float, no alloc, no loops
//! (Laws 1-2); observe is a pure step (Law 4).
//!
//! The arithmetic is replicated exactly, including the arithmetic `>> 8`
//! (floor semantics) on signed values and the NORMATIVE verdict ordering
//! (burst, rising, falling, stable — with the |slope| >= half a step per
//! step trend guards that keep constant-signal plateaus at STABLE). The
//! packed verdict stream is byte-compared across ports
//! (fixtures/xlang-trend/, driven by the `trend_xlang` bin).
//!
//! Rust note: `i64 >> 8` IS the C's arithmetic shift (floor) — the TS
//! port needs a Math.floor workaround; Rust does not.

// Verdicts are PROTOCOL values (packed into trace bytes; do not
// renumber) — mirror of `weft_trend_verdict_t`.
/// Constant signal (or no settled trend).
pub const TREND_VERDICT_STABLE: u8 = 0;
/// The projection leads the raw signal up with a real trend.
pub const TREND_VERDICT_RISING: u8 = 1;
/// The projection leads the raw signal down with a real trend.
pub const TREND_VERDICT_FALLING: u8 = 2;
/// A single-step jump >= burst_delta — demands action.
pub const TREND_VERDICT_BURST: u8 = 3;

/// Projection distance (steps). Mirror of `WEFT_TREND_HORIZON`.
pub const TREND_HORIZON: u32 = 8;

/// G5 packing cap (`verdict << 6 | skip`). Mirror of
/// `WEFT_TREND_SKIP_MAX`.
pub const TREND_SKIP_MAX: u32 = 63;

/// Defaults (RFC-0020 §1-2). Mirror of the `WEFT_TREND_*_DEFAULT` macros.
pub const TREND_ALPHA_Q8_DEFAULT: u32 = 48; // 0.1875
/// Fast-trend pair (beta 0.375).
pub const TREND_BETA_Q8_DEFAULT: u32 = 96;
/// Single-step delta that reads BURST.
pub const TREND_BURST_DEFAULT: u32 = 32;
/// The governor's Skip rung.
pub const TREND_RISING_DEFAULT: u32 = 4;
/// Falling threshold.
pub const TREND_FALLING_DEFAULT: u32 = 1;

/// One projection result. `skip_n` is valid for RISING/BURST; 0
/// otherwise. Copy — returned by value, zero allocation (Law 2).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TrendOut {
    /// Which verdict fired (a TREND_VERDICT_* protocol value).
    pub verdict: u8,
    /// Projected behind at HORIZON steps.
    pub pred_raw: u32,
    /// Recommended proactive skip.
    pub skip_n: u32,
}

/// Filter state + configuration (mirror of `weft_trend_t`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WeftTrend {
    /// Level estimate, Q16 (EWMA of staleness).
    pub level_q16: u32,
    /// Slope estimate, Q16 per step (signed — falling trends go negative).
    pub slope_q16: i32,
    /// Previous raw sample.
    pub last_raw: i32,
    /// False until the first sample seeds the filter.
    pub has_last: bool,
    /// Level gain, Q8 (fraction of 256).
    pub alpha_q8: u32,
    /// Slope gain, Q8.
    pub beta_q8: u32,
    /// Single-step delta that reads BURST.
    pub burst_delta: u32,
    /// pred_raw at/above this with a real trend reads RISING.
    pub rising_behind: u32,
    /// pred_raw at/below this with a real downtrend reads FALLING.
    pub falling_behind: u32,
    /// Samples observed (advisory telemetry).
    pub t_samples: u64,
    /// Per-verdict counts, indexed by the verdict value (advisory).
    pub t_verdict: [u64; 4],
}

impl Default for WeftTrend {
    /// RFC-0020 defaults: alpha 48 (0.1875) + beta 96 (0.375) — the
    /// fast-trend pair — burst 32, rising 4 (the governor's Skip rung),
    /// falling 1; all filter state zero (the first sample seeds it).
    fn default() -> Self {
        Self {
            level_q16: 0,
            slope_q16: 0,
            last_raw: 0,
            has_last: false,
            alpha_q8: TREND_ALPHA_Q8_DEFAULT,
            beta_q8: TREND_BETA_Q8_DEFAULT,
            burst_delta: TREND_BURST_DEFAULT,
            rising_behind: TREND_RISING_DEFAULT,
            falling_behind: TREND_FALLING_DEFAULT,
            t_samples: 0,
            t_verdict: [0; 4],
        }
    }
}

/// Observe one staleness sample. Pure integer step; returns the
/// projection (and counts the verdict).
pub fn trend_observe(t: &mut WeftTrend, behind: u32) -> TrendOut {
    if !t.has_last {
        // First sample seeds level, zero slope: no prediction is possible
        // from one sample — STABLE until the filter settles.
        t.level_q16 = behind << 16;
        t.slope_q16 = 0;
        t.last_raw = behind as i32;
        t.has_last = true;
        t.t_samples += 1;
        t.t_verdict[TREND_VERDICT_STABLE as usize] += 1;
        return TrendOut {
            verdict: TREND_VERDICT_STABLE,
            pred_raw: behind,
            skip_n: 0,
        };
    }

    let delta_raw: i32 = behind as i32 - t.last_raw;
    t.last_raw = behind as i32;

    // level += alpha * (raw - level)   (Q16 state, Q8 gain). The error is
    // formed as (int32)(behind << 16) - (int32)level_q16 exactly like the
    // C — behind < 2^16 in the protocol domain keeps it exact — then
    // widened to i64 for the multiply/shift.
    let level_err: i64 = ((behind << 16) as i32 as i64) - (t.level_q16 as i32 as i64);
    t.level_q16 = (t.level_q16 as i64 + ((t.alpha_q8 as i64 * level_err) >> 8)) as u32;

    // slope += beta * (delta - slope)  (Q16 per step).
    let delta_q16: i64 = delta_raw as i64 << 16;
    t.slope_q16 = (t.slope_q16 as i64
        + ((t.beta_q8 as i64 * (delta_q16 - t.slope_q16 as i64)) >> 8)) as i32;

    // Projection: level + slope * HORIZON, saturated to [0, INT32_MAX]
    // (the saturating shift).
    let mut pred_q16: i64 = t.level_q16 as i64 + t.slope_q16 as i64 * TREND_HORIZON as i64;
    if pred_q16 < 0 {
        pred_q16 = 0;
    }
    if pred_q16 > 0x7FFF_FFFF {
        pred_q16 = 0x7FFF_FFFF;
    }
    let pred_raw: u32 = (pred_q16 >> 16) as u32;

    // Verdict order is NORMATIVE (RFC-0020 §2): burst, rising, falling,
    // stable. RISING/FALLING additionally require a REAL trend (|slope|
    // >= half a step per step) — the EWMA slope stalls at a small nonzero
    // residue on a constant signal, and a plateau must read STABLE.
    let out = if delta_raw >= 0 && delta_raw as u32 >= t.burst_delta {
        // A burst demands action even if the projection is still low.
        let skip_n = pred_raw.min(TREND_SKIP_MAX);
        TrendOut {
            verdict: TREND_VERDICT_BURST,
            pred_raw,
            skip_n: if skip_n == 0 { 1 } else { skip_n },
        }
    } else if pred_raw >= t.rising_behind && t.slope_q16 >= 32768 {
        TrendOut {
            verdict: TREND_VERDICT_RISING,
            pred_raw,
            skip_n: pred_raw.saturating_sub(1).min(TREND_SKIP_MAX),
        }
    } else if pred_raw <= t.falling_behind && t.slope_q16 <= -32768 {
        TrendOut {
            verdict: TREND_VERDICT_FALLING,
            pred_raw,
            skip_n: 0,
        }
    } else {
        TrendOut {
            verdict: TREND_VERDICT_STABLE,
            pred_raw,
            skip_n: 0,
        }
    };

    t.t_samples += 1;
    t.t_verdict[out.verdict as usize] += 1;
    out
}

/// G5-style packed verdict byte: `(verdict << 6) | min(skip_n, 63)`.
pub fn trend_pack(out: &TrendOut) -> u8 {
    let skip = out.skip_n.min(TREND_SKIP_MAX);
    (((out.verdict as u32) << 6) | skip) as u8
}
