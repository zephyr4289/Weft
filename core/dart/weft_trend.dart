// weft_trend.dart — RFC 0020: predictive lag-trend estimator, Dart driver
// layer.
//
// WHY EXISTS: RFC-0020's estimator (a 1-D alpha-beta filter in pure
// integer Q16 — level = EWMA of staleness, slope = EWMA of its per-step
// change, a HORIZON-step projection, and a CLOSED verdict set
// STABLE/RISING/FALLING/BURST with a proactive skip_n) shipped as the C
// reference (core/c/weft_trend.{h,c}) with TS/Kotlin/Swift twins — this
// file is the Dart half, arithmetic-identical to all of them. The governor
// ladder is untouched — this is a sensor the consumer MAY consult (Law 3).
// Verdict values are PROTOCOL (G5 packs verdict<<6|skip into trace bytes —
// do not renumber).
//
// ARITHMETIC PARITY: Dart VM ints ARE 64-bit two's complement, so the Q16
// updates run natively — arithmetic >> 8 matches the C exactly, and the
// (int64_t) widenings are Dart's own sign/zero extension of stored values.
// The u32/i32 fields are emulated with explicit masks: _toU32 after every
// update that can wrap (level), _toI32 for the C's (int32_t)
// reinterpretations (slope, last_raw, delta_raw, level_err). The slope
// guards (|slope| >= half a step per step) keep constant-signal plateaus
// at STABLE — the EWMA slope stalls at a small nonzero residue on a
// constant signal, and a plateau must read STABLE.
//
// PINNED PARITY VECTOR (from the C reference — do not "fix" it): the
// 2000-sample xorshift verdict stream (behind = state % 64, seed
// 0x00C0FFEE) hashes (FNV-1a over the hex stream) to 0x11187b9a02b378ef.
//
// observe() allocates NOTHING — the result is written into a caller-owned
// [TrendOut] record (the GovernorAction identity-stable-record pattern);
// pass null to discard it.
//
// STATUS: SOURCE-ONLY, PENDING TOOLCHAIN VERIFICATION (not compilable in
// the x86_64 Linux sandbox — no dart; the arithmetic was validated
// op-for-op against the C reference before translation, and the fixture
// gate runs when flutter-packages CI brings the toolchain).

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

/// Verdicts — PROTOCOL values (packed into trace bytes; do not renumber).
abstract final class TrendVerdict {
  static const int stable = 0;
  static const int rising = 1;
  static const int falling = 2;
  static const int burst = 3;
}

/// Projection distance (steps).
const int weftTrendHorizon = 8;

/// G5 packing cap (kind<<6 | skip).
const int weftTrendSkipMax = 63;

/// Defaults (RFC-0020 §1-2).
const int weftTrendAlphaQ8Default = 48; // 0.1875
const int weftTrendBetaQ8Default = 96;  // 0.375 — fast-trend pair
const int weftTrendBurstDefault = 32;
const int weftTrendRisingDefault = 4;   // the governor's Skip rung
const int weftTrendFallingDefault = 1;

// ---------------------------------------------------------------------------
// State + result records
// ---------------------------------------------------------------------------

/// One projection result. skipN is valid for RISING/BURST; 0 otherwise.
class TrendOut {
  int verdict; // TrendVerdict protocol value
  int predRaw; // u32 — projected behind at HORIZON steps
  int skipN;   // u32 — recommended proactive skip
  TrendOut()
      : verdict = TrendVerdict.stable,
        predRaw = 0,
        skipN = 0;
}

/// The estimator. Call [weftTrendInit] before the first observe — the
/// constructor plants the RFC defaults but init() is the C contract, kept
/// explicit.
class WeftTrend {
  // estimator state (Q16)
  int levelQ16 = 0;    // u32 (masked)
  int slopeQ16 = 0;    // i32 (via _toI32)
  int lastRaw = 0;     // i32 (via _toI32)
  bool hasLast = false;

  // configuration (Q8 fractions of 256 / raw thresholds)
  int alphaQ8 = weftTrendAlphaQ8Default;
  int betaQ8 = weftTrendBetaQ8Default;
  int burstDelta = weftTrendBurstDefault;
  int risingBehind = weftTrendRisingDefault;
  int fallingBehind = weftTrendFallingDefault;

  // advisory telemetry
  int tSamples = 0;          // u64 counter
  final List<int> tVerdict = [0, 0, 0, 0]; // u64 x4
}

// ---------------------------------------------------------------------------
// u32 / i32 reinterpret helpers (the C's casts)
// ---------------------------------------------------------------------------

/// (uint32_t)v — the low 32 bits as a non-negative int.
int _toU32(int v) => v & 0xFFFFFFFF;

/// (int32_t)v — the low 32 bits reinterpreted signed.
int _toI32(int v) {
  final u = v & 0xFFFFFFFF;
  return u >= 0x80000000 ? u - 0x100000000 : u;
}

// ---------------------------------------------------------------------------
// Init / configure
// ---------------------------------------------------------------------------

/// Init with the RFC defaults.
void weftTrendInit(WeftTrend t) {
  t.levelQ16 = 0;
  t.slopeQ16 = 0;
  t.lastRaw = 0;
  t.hasLast = false;
  t.alphaQ8 = weftTrendAlphaQ8Default;
  t.betaQ8 = weftTrendBetaQ8Default;
  t.burstDelta = weftTrendBurstDefault;
  t.risingBehind = weftTrendRisingDefault;
  t.fallingBehind = weftTrendFallingDefault;
  t.tSamples = 0;
  for (var i = 0; i < 4; i++) {
    t.tVerdict[i] = 0;
  }
}

/// Unsigned clamp — the C's clamp_u32 over the u32 domain (a value whose
/// u32 bit pattern is huge must clamp HIGH, not low).
int _clampU32(int v, int lo, int hi) {
  final u = _toU32(v);
  if (u < lo) return lo;
  if (u > hi) return hi;
  return u;
}

/// Override the configuration (Q8 gains, thresholds). Values are clamped:
/// gains to [1, 256], burst to [1, 1<<30], thresholds to [0, 1<<20].
void weftTrendConfigure(WeftTrend t, int alphaQ8, int betaQ8, int burstDelta,
    int risingBehind, int fallingBehind) {
  t.alphaQ8 = _clampU32(alphaQ8, 1, 256);
  t.betaQ8 = _clampU32(betaQ8, 1, 256);
  t.burstDelta = _clampU32(burstDelta, 1, 1 << 30);
  t.risingBehind = _clampU32(risingBehind, 0, 1 << 20);
  t.fallingBehind = _clampU32(fallingBehind, 0, 1 << 20);
}

// ---------------------------------------------------------------------------
// Observe
// ---------------------------------------------------------------------------

/// Observe one staleness sample. Pure integer step; writes the projection
/// to `out` (may be null). Returns the verdict for convenience.
///
/// The arithmetic is NORMATIVE (weft_trend.c) — mirror, never redesign:
///   level += alpha * (raw - level)   (Q16, gain Q8)
///   slope += beta * (delta - slope)  (Q16 per step)
///   pred   = level + slope * HORIZON (saturating shift)
int weftTrendObserve(WeftTrend t, int behind, TrendOut? out) {
  var verdict = TrendVerdict.stable;
  var predRaw = 0; // u32
  var skipN = 0;   // u32

  if (!t.hasLast) {
    // first sample seeds level, zero slope (declared: no prediction is
    // possible from one sample — STABLE until the filter settles)
    t.levelQ16 = _toU32(behind << 16); // u32 shl
    t.slopeQ16 = 0;
    t.lastRaw = _toI32(behind);
    t.hasLast = true;
    verdict = TrendVerdict.stable;
    predRaw = _toU32(behind);
    t.tSamples++;
    t.tVerdict[TrendVerdict.stable]++;
    if (out != null) {
      out.verdict = verdict;
      out.predRaw = predRaw;
      out.skipN = skipN;
    }
    return verdict;
  }

  final deltaRaw = _toI32(_toI32(behind) - t.lastRaw);
  t.lastRaw = _toI32(behind);

  // level += alpha * (raw - level)   (Q16, gain Q8)
  // (int32_t)(behind << 16) - (int32_t)level_q16 — the i64 subtraction
  // then _toI32 is congruent mod 2^32 to the C's i32 wrap.
  final levelErr = _toI32(_toI32(behind << 16) - _toI32(t.levelQ16));
  // (int64_t)level_q16 (u32 zero-extend: stored masked, non-negative) +
  // (alpha * levelErr) >> 8 — the & mask is the C's (int64_t)u32 widening;
  // Dart >> on a negative int IS arithmetic.
  t.levelQ16 = _toU32(t.levelQ16 + (((t.alphaQ8 & 0xFFFFFFFF) * levelErr) >> 8));

  // slope += beta * (delta - slope)  (Q16 per step)
  final deltaQ16 = deltaRaw << 16; // (int64_t)delta_raw << 16
  t.slopeQ16 = _toI32(
      t.slopeQ16 + (((t.betaQ8 & 0xFFFFFFFF) * (deltaQ16 - t.slopeQ16)) >> 8));

  // projection: level + slope * HORIZON (saturating shift)
  var predQ16 = t.levelQ16 + t.slopeQ16 * weftTrendHorizon;
  if (predQ16 < 0) predQ16 = 0;
  if (predQ16 > 0x7FFFFFFF) predQ16 = 0x7FFFFFFF;
  predRaw = _toU32(predQ16 >> 16); // (uint32_t)(pred_q16 >> 16)

  // Verdict order is NORMATIVE (RFC-0020 §2): burst, rising, falling,
  // stable. RISING/FALLING additionally require a REAL trend (|slope| >=
  // half a step per step — 0.5 in Q16 is 32768) — the EWMA slope stalls at
  // a small nonzero residue on a constant signal, and a plateau must read
  // STABLE.
  // Threshold guards are u32-vs-u32 (the C's unsigned compares) — the
  // masks make that explicit even for directly-set configuration fields.
  if (deltaRaw >= 0 && _toU32(deltaRaw) >= _toU32(t.burstDelta)) {
    // (uint32_t)delta_raw == delta_raw on this path (deltaRaw >= 0).
    verdict = TrendVerdict.burst;
    skipN = predRaw > weftTrendSkipMax ? weftTrendSkipMax : predRaw;
    if (skipN == 0) skipN = 1; // a burst demands action
  } else if (_toU32(predRaw) >= _toU32(t.risingBehind) && t.slopeQ16 >= 32768) {
    verdict = TrendVerdict.rising;
    final skip = predRaw >= 1 ? predRaw - 1 : 0;
    skipN = skip > weftTrendSkipMax ? weftTrendSkipMax : skip;
  } else if (_toU32(predRaw) <= _toU32(t.fallingBehind) &&
      t.slopeQ16 <= -32768) {
    verdict = TrendVerdict.falling;
  } else {
    verdict = TrendVerdict.stable;
  }

  t.tSamples++;
  t.tVerdict[verdict]++;
  if (out != null) {
    out.verdict = verdict;
    out.predRaw = predRaw;
    out.skipN = skipN;
  }
  return verdict;
}

/// G5-style packed verdict byte: (verdict << 6) | min(skip_n, 63).
int weftTrendPack(TrendOut out) {
  final skip = out.skipN > weftTrendSkipMax ? weftTrendSkipMax : out.skipN;
  return ((out.verdict << 6) | skip) & 0xFF;
}
