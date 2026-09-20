// trend.ts — RFC 0020 predictive lag-trend estimator, TS port.
//
// Mirror of core/c/weft_trend.{h,c}: 1-D alpha-beta filter in pure integer
// Q16 (level = EWMA of staleness, slope = EWMA of per-step delta), projected
// HORIZON steps ahead, CLOSED verdict set with a proactive skip_n.
// Verdict values are PROTOCOL (G5 packing verdict<<6|skip — do not renumber).
//
// 32-bit care: all Q16 state fits doubles exactly (<= 2^32); the C's
// arithmetic >>8 on negative values is Math.floor(x / 256) — reproduced by
// asr8 below. The slope guards (|slope| >= 0.5/step) keep constant-signal
// plateaus at STABLE (the EWMA slope stalls at a tiny nonzero residue).

export const WEFT_TREND_HORIZON = 8;
export const WEFT_TREND_SKIP_MAX = 63;

export const TrendVerdict = {
  Stable: 0,
  Rising: 1,
  Falling: 2,
  Burst: 3,
} as const;
export type TrendVerdict = (typeof TrendVerdict)[keyof typeof TrendVerdict];

export const TREND_ALPHA_Q8_DEFAULT = 48;   // 0.1875
export const TREND_BETA_Q8_DEFAULT = 96;    // 0.375 — fast-trend pair
export const TREND_BURST_DEFAULT = 32;
export const TREND_RISING_DEFAULT = 4;      // the governor's Skip rung
export const TREND_FALLING_DEFAULT = 1;

export interface WeftTrend {
  level_q16: number;      // u32
  slope_q16: number;      // i32
  last_raw: number;       // i32
  has_last: boolean;
  alpha_q8: number;
  beta_q8: number;
  burst_delta: number;
  rising_behind: number;
  falling_behind: number;
  t_samples: number;
  t_verdict: [number, number, number, number];
}

export interface TrendOut {
  verdict: TrendVerdict;
  pred_raw: number;       // projected behind at HORIZON steps
  skip_n: number;         // recommended proactive skip (RISING/BURST)
}

/// C arithmetic shift right by 8 on a signed value (floor semantics).
function asr8(x: number): number {
  return Math.floor(x / 256);
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

export function trendInit(t?: Partial<WeftTrend>): WeftTrend {
  return {
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
    t_verdict: [0, 0, 0, 0],
    ...t,
  };
}

export function trendConfigure(t: WeftTrend, alphaQ8: number, betaQ8: number,
                               burstDelta: number, risingBehind: number,
                               fallingBehind: number): void {
  t.alpha_q8 = clamp(alphaQ8, 1, 256);
  t.beta_q8 = clamp(betaQ8, 1, 256);
  t.burst_delta = clamp(burstDelta, 1, 1 << 30);
  t.rising_behind = clamp(risingBehind, 0, 1 << 20);
  t.falling_behind = clamp(fallingBehind, 0, 1 << 20);
}

export function trendObserve(t: WeftTrend, behind: number,
                             out?: TrendOut): TrendVerdict {
  const o: TrendOut = { verdict: TrendVerdict.Stable, pred_raw: 0, skip_n: 0 };

  if (!t.has_last) {
    // first sample seeds level, zero slope (no prediction from one sample)
    t.level_q16 = behind * 65536;   // <= 2^46 — exact in double
    t.slope_q16 = 0;
    t.last_raw = behind | 0;
    t.has_last = true;
    o.verdict = TrendVerdict.Stable;
    o.pred_raw = behind;
    t.t_samples++;
    t.t_verdict[TrendVerdict.Stable]++;
    if (out) Object.assign(out, o);
    return o.verdict;
  }

  const deltaRaw = (behind | 0) - t.last_raw;
  t.last_raw = behind | 0;

  // level += alpha * (raw - level)   (Q16, gain Q8)
  const levelErr = behind * 65536 - t.level_q16;
  t.level_q16 = (t.level_q16 + asr8(t.alpha_q8 * levelErr)) >>> 0;
  // slope += beta * (delta - slope)  (Q16 per step)
  const deltaQ16 = deltaRaw * 65536;
  t.slope_q16 = (t.slope_q16 + asr8(t.beta_q8 * (deltaQ16 - t.slope_q16))) | 0;

  // projection: level + slope * HORIZON (saturating shift)
  let predQ16 = t.level_q16 + t.slope_q16 * WEFT_TREND_HORIZON;
  if (predQ16 < 0) predQ16 = 0;
  if (predQ16 > 0x7fffffff) predQ16 = 0x7fffffff;
  const predRaw = Math.floor(predQ16 / 65536);
  o.pred_raw = predRaw;

  // verdict order is NORMATIVE: burst, rising, falling, stable — with the
  // slope guards (|slope| >= 0.5/step) that keep plateaus STABLE.
  if (deltaRaw >= 0 && deltaRaw >= t.burst_delta) {
    o.verdict = TrendVerdict.Burst;
    o.skip_n = Math.min(predRaw, WEFT_TREND_SKIP_MAX);
    if (o.skip_n === 0) o.skip_n = 1;  // a burst demands action
  } else if (predRaw >= t.rising_behind && t.slope_q16 >= 32768) {
    o.verdict = TrendVerdict.Rising;
    o.skip_n = Math.min(predRaw >= 1 ? predRaw - 1 : 0, WEFT_TREND_SKIP_MAX);
  } else if (predRaw <= t.falling_behind && t.slope_q16 <= -32768) {
    o.verdict = TrendVerdict.Falling;
  } else {
    o.verdict = TrendVerdict.Stable;
  }

  t.t_samples++;
  t.t_verdict[o.verdict]++;
  if (out) Object.assign(out, o);
  return o.verdict;
}

/// G5-style packed verdict byte: (verdict << 6) | min(skip_n, 63).
export function trendPack(out: TrendOut): number {
  const skip = Math.min(out.skip_n, WEFT_TREND_SKIP_MAX);
  return ((out.verdict << 6) | skip) & 0xff;
}
