//! RFC-0020 trend estimator conformance (D-series), Rust.
//!
//! Mirrors `packages/core/test/trend.test.ts` and
//! `core/c/weft_trend_test.c`. Pinned parity vector: the xorshift
//! verdict stream hashes (FNV-1a over the packed hex bytes) to
//! 0x11187b9a02b378ef — the cross-port fixture surface
//! (fixtures/xlang-trend/, driven by the `trend_xlang` bin).

use weft_core::trend::{
    trend_observe, trend_pack, TrendOut, WeftTrend, TREND_VERDICT_BURST, TREND_VERDICT_FALLING,
    TREND_VERDICT_RISING, TREND_VERDICT_STABLE,
};

/// xorshift32 — 04-LITMUS §0.2, the repo's canonical deterministic RNG.
fn xorshift32(mut x: u32) -> u32 {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    x
}

#[test]
fn d1_plateau_stable_level_converges() {
    let mut t = WeftTrend::default();
    let mut o = trend_observe(&mut t, 5);
    for _ in 1..30 {
        o = trend_observe(&mut t, 5);
    }
    assert_eq!(o.verdict, TREND_VERDICT_STABLE, "constant 5 is STABLE (not RISING)");
    assert!(
        (294_912..=360_448).contains(&t.level_q16),
        "level converged to ~5 (Q16 [4.5, 5.5])"
    );
    assert!(t.slope_q16 < 32768, "slope residue below the trend guard");
}

#[test]
fn d2_predictive_lead() {
    let mut t = WeftTrend::default();
    let mut o = trend_observe(&mut t, 2);
    for _ in 1..12 {
        o = trend_observe(&mut t, 2);
    }
    assert_eq!(o.verdict, TREND_VERDICT_STABLE, "plateau reads STABLE");
    // ramp +1/step: RISING (proactive skip) must fire as the ramp STARTS,
    // well before reactive Snapshot-class reactions would be warranted.
    let mut first_rising: i32 = -1;
    let mut first_skip: u32 = 0;
    for b in 3..=10u32 {
        o = trend_observe(&mut t, b);
        if o.verdict == TREND_VERDICT_RISING && first_rising < 0 {
            first_rising = b as i32;
            first_skip = o.skip_n;
        }
    }
    assert_eq!(first_rising, 4, "RISING fires at raw behind 4 (pinned)");
    assert_eq!(first_skip, 6, "proactive skip 6 (projection 7.4, pinned)");
    assert!(o.pred_raw >= 7, "projection leads the raw signal by 3+ steps");
}

#[test]
fn d3_recovery_falling() {
    let mut t = WeftTrend::default();
    let mut o = trend_observe(&mut t, 8);
    for b in (1..=7u32).rev() {
        o = trend_observe(&mut t, b);
        if b == 1 {
            assert_eq!(o.verdict, TREND_VERDICT_FALLING, "decline tail reads FALLING");
            assert_eq!(o.skip_n, 0, "FALLING carries no skip");
        }
    }
}

#[test]
fn d4_burst() {
    let mut t = WeftTrend::default();
    trend_observe(&mut t, 2);
    trend_observe(&mut t, 2);
    trend_observe(&mut t, 2);
    // single-step jump 62 >= burst_delta 32 -> BURST; the projection
    // saturates past the G5 cap, so the demand-action skip pins at 63.
    let o = trend_observe(&mut t, 64);
    assert_eq!(o.verdict, TREND_VERDICT_BURST, "single-step jump 62 -> BURST");
    assert_eq!(o.skip_n, 63, "skip pinned (projection saturates past the G5 cap)");
}

#[test]
fn d5_pinned_stream() {
    // 2000 samples of behind = state % 64; pack the verdict stream (G5
    // packing: verdict << 6 | skip) and FNV-1a the hex bytes — the pinned
    // cross-port parity vector.
    let mut t = WeftTrend::default();
    let mut state: u32 = 0x00C0_FEEE;
    let mut hex = String::with_capacity(4000);
    for _ in 0..2000 {
        state = xorshift32(state);
        let o = trend_observe(&mut t, state % 64);
        hex.push_str(&format!("{:02x}", trend_pack(&o)));
    }
    let mut h: u64 = 0xcbf29ce484222325;
    for b in hex.as_bytes() {
        h ^= *b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    assert_eq!(h, 0x11187b9a02b378ef, "verdict stream pinned (parity vector)");
    assert_eq!(t.t_samples, 2000, "2000 samples observed");
}

#[test]
fn d7_q16_hand_vectors() {
    let mut t = WeftTrend::default();
    trend_observe(&mut t, 3); // seed
    assert_eq!(t.level_q16, 3 << 16, "seed: level = raw");
    assert_eq!(t.slope_q16, 0, "seed: slope = 0");
    let o = trend_observe(&mut t, 4);
    // level = 3 + 0.1875*(4-3) = 3.1875 -> 208896
    assert_eq!(t.level_q16, 208_896, "level Q16 hand-vector exact");
    // slope = 0 + 0.375*1 = 0.375 -> 24576
    assert_eq!(t.slope_q16, 24_576, "slope Q16 hand-vector exact");
    // pred = 3.1875 + 0.375*8 = 6.1875 -> 6, but the slope guard (0.5)
    // has not been reached — STABLE
    assert_eq!(o.verdict, TREND_VERDICT_STABLE);
    assert_eq!(o.pred_raw, 6, "projection hand-vector exact");
}

#[test]
fn pack_format() {
    // G5 packing: verdict << 6 | min(skip_n, 63) — the trace byte.
    let out = TrendOut { verdict: TREND_VERDICT_STABLE, pred_raw: 0, skip_n: 0 };
    assert_eq!(trend_pack(&out), 0, "stable packs to 0");
    let out = TrendOut { verdict: TREND_VERDICT_RISING, pred_raw: 5, skip_n: 4 };
    assert_eq!(trend_pack(&out), (1 << 6) | 4);
    let out = TrendOut { verdict: TREND_VERDICT_BURST, pred_raw: 500, skip_n: 500 };
    assert_eq!(trend_pack(&out), (3 << 6) | 63, "skip clamped to the G5 cap");
}
