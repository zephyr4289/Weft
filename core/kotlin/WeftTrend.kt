// WeftTrend.kt — RFC 0020: predictive lag-trend estimator, Kotlin driver
// layer.
//
// WHY EXISTS: RFC-0020's estimator (a 1-D alpha-beta filter in pure
// integer Q16 — level = EWMA of staleness, slope = EWMA of its per-step
// change, a HORIZON-step projection, and a CLOSED verdict set
// STABLE/RISING/FALLING/BURST with a proactive skip_n) shipped as the C
// reference (core/c/weft_trend.{h,c}) with a TS twin
// (packages/core/src/trend.ts) — this file is the Kotlin half,
// arithmetic-identical to both. The governor ladder is untouched — this is
// a sensor the consumer MAY consult (Law 3). Verdict values are PROTOCOL
// (G5 packs verdict<<6|skip into trace bytes — do not renumber).
//
// ARITHMETIC PARITY: every C integer op is mirrored op-for-op, including
// the u32 level wrap, the i32 slope truncation, the arithmetic >>8 (Kotlin
// shr on Long/Int IS arithmetic — matches C), the saturating projection,
// and the verdict ordering (burst, rising, falling, stable — NORMATIVE).
// u32 fields live in Int BIT PATTERNS; the C's (int64_t)u32 widening is
// the explicit `and 0xFFFFFFFFL` mask, and the C's (uint32_t)/(int32_t)
// narrowing casts are Int truncation (toLong().toInt() keeps the low 32
// bits). The slope guards (|slope| >= half a step per step) keep
// constant-signal plateaus at STABLE — the EWMA slope stalls at a small
// nonzero residue on a constant signal, and a plateau must read STABLE.
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
// the x86_64 Linux sandbox — no kotlinc; the arithmetic was validated
// op-for-op against the C reference before translation, and the fixture
// gate runs when android-packages CI brings the toolchain).

package dev.weft

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

/** Verdicts — PROTOCOL values (packed into trace bytes; do not renumber). */
object TrendVerdict {
    const val STABLE: Int = 0
    const val RISING: Int = 1
    const val FALLING: Int = 2
    const val BURST: Int = 3
}

/// Projection distance (steps).
const val WEFT_TREND_HORIZON: Int = 8

/// G5 packing cap (kind<<6 | skip).
const val WEFT_TREND_SKIP_MAX: Int = 63

/// Defaults (RFC-0020 §1-2).
const val WEFT_TREND_ALPHA_Q8_DEFAULT: Int = 48    // 0.1875
const val WEFT_TREND_BETA_Q8_DEFAULT: Int = 96     // 0.375 — fast-trend pair
const val WEFT_TREND_BURST_DEFAULT: Int = 32
const val WEFT_TREND_RISING_DEFAULT: Int = 4       // the governor's Skip rung
const val WEFT_TREND_FALLING_DEFAULT: Int = 1

// ---------------------------------------------------------------------------
// State + result records
// ---------------------------------------------------------------------------

/// One projection result. skipN is valid for RISING/BURST; 0 otherwise.
class TrendOut(
    var verdict: Int = TrendVerdict.STABLE,
    var predRaw: Int = 0, // u32 bit pattern — projected behind at HORIZON steps
    var skipN: Int = 0    // u32 bit pattern — recommended proactive skip
)

/// The estimator. Call [weftTrendInit] (or construct + init) before the
/// first observe — the constructor plants the RFC defaults but init() is
/// the C contract, kept explicit.
class WeftTrend {
    // estimator state (Q16)
    var levelQ16: Int = 0  // u32 bit pattern (unsigned wrap on update)
    var slopeQ16: Int = 0  // i32 — signed: falling trends have negative slope
    var lastRaw: Int = 0   // i32
    var hasLast: Boolean = false

    // configuration (Q8 fractions of 256 / raw thresholds)
    var alphaQ8: Int = WEFT_TREND_ALPHA_Q8_DEFAULT
    var betaQ8: Int = WEFT_TREND_BETA_Q8_DEFAULT
    var burstDelta: Int = WEFT_TREND_BURST_DEFAULT
    var risingBehind: Int = WEFT_TREND_RISING_DEFAULT
    var fallingBehind: Int = WEFT_TREND_FALLING_DEFAULT

    // advisory telemetry
    var tSamples: Long = 0        // u64
    val tVerdict: LongArray = LongArray(4) // u64 x4
}

// ---------------------------------------------------------------------------
// Init / configure
// ---------------------------------------------------------------------------

/// Init with the RFC defaults.
fun weftTrendInit(t: WeftTrend) {
    t.levelQ16 = 0
    t.slopeQ16 = 0
    t.lastRaw = 0
    t.hasLast = false
    t.alphaQ8 = WEFT_TREND_ALPHA_Q8_DEFAULT
    t.betaQ8 = WEFT_TREND_BETA_Q8_DEFAULT
    t.burstDelta = WEFT_TREND_BURST_DEFAULT
    t.risingBehind = WEFT_TREND_RISING_DEFAULT
    t.fallingBehind = WEFT_TREND_FALLING_DEFAULT
    t.tSamples = 0
    for (i in 0..3) t.tVerdict[i] = 0
}

/// Unsigned clamp — the C's clamp_u32 over the u32 domain (a negative Int
/// is a huge u32 bit pattern and must clamp HIGH, not low).
private fun clampU32(v: Int, lo: Int, hi: Int): Int {
    val u = v.toLong() and 0xFFFFFFFFL
    val loU = lo.toLong() and 0xFFFFFFFFL
    val hiU = hi.toLong() and 0xFFFFFFFFL
    return if (u < loU) lo else if (u > hiU) hi else v
}

/// Override the configuration (Q8 gains, thresholds). Values are clamped:
/// gains to [1, 256], burst to [1, 1<<30], thresholds to [0, 1<<20].
fun weftTrendConfigure(
    t: WeftTrend,
    alphaQ8: Int,
    betaQ8: Int,
    burstDelta: Int,
    risingBehind: Int,
    fallingBehind: Int
) {
    t.alphaQ8 = clampU32(alphaQ8, 1, 256)
    t.betaQ8 = clampU32(betaQ8, 1, 256)
    t.burstDelta = clampU32(burstDelta, 1, 1 shl 30)
    t.risingBehind = clampU32(risingBehind, 0, 1 shl 20)
    t.fallingBehind = clampU32(fallingBehind, 0, 1 shl 20)
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
fun weftTrendObserve(t: WeftTrend, behind: Int, out: TrendOut? = null): Int {
    var verdict = TrendVerdict.STABLE
    var predRaw = 0    // u32 bit pattern
    var skipN = 0      // u32 bit pattern

    if (!t.hasLast) {
        // first sample seeds level, zero slope (declared: no prediction
        // is possible from one sample — STABLE until the filter settles)
        t.levelQ16 = behind shl 16 // u32 shl — same bit pattern
        t.slopeQ16 = 0
        t.lastRaw = behind // (int32_t)behind — the bit pattern
        t.hasLast = true
        verdict = TrendVerdict.STABLE
        predRaw = behind
        t.tSamples++
        t.tVerdict[TrendVerdict.STABLE]++
        if (out != null) {
            out.verdict = verdict
            out.predRaw = predRaw
            out.skipN = skipN
        }
        return verdict
    }

    val deltaRaw: Int = behind - t.lastRaw // (int32_t)behind - last_raw — i32 wrap
    t.lastRaw = behind

    // level += alpha * (raw - level)   (Q16, gain Q8)
    // (int32_t)(behind << 16) - (int32_t)level_q16 — Int arithmetic IS the
    // i32 wrap; the (int64_t)u32 widening of level_q16 is the mask below.
    val levelErr: Int = (behind shl 16) - t.levelQ16
    val levelSum: Long = (t.levelQ16.toLong() and 0xFFFFFFFFL) +
        (((t.alphaQ8.toLong() and 0xFFFFFFFFL) * levelErr.toLong()) shr 8)
    t.levelQ16 = levelSum.toInt() // (uint32_t) — low 32 bits

    // slope += beta * (delta - slope)  (Q16 per step)
    val deltaQ16: Long = deltaRaw.toLong() shl 16 // (int64_t)delta_raw << 16
    val slopeSum: Long = t.slopeQ16.toLong() +
        (((t.betaQ8.toLong() and 0xFFFFFFFFL) * (deltaQ16 - t.slopeQ16.toLong())) shr 8)
    t.slopeQ16 = slopeSum.toInt() // (int32_t) — low 32 bits, signed

    // projection: level + slope * HORIZON (saturating shift)
    var predQ16: Long = (t.levelQ16.toLong() and 0xFFFFFFFFL) +
        (t.slopeQ16.toLong() * WEFT_TREND_HORIZON)
    if (predQ16 < 0) predQ16 = 0
    if (predQ16 > 0x7FFFFFFFL) predQ16 = 0x7FFFFFFFL
    predRaw = (predQ16 shr 16).toInt() // (uint32_t)(pred_q16 >> 16)

    // Verdict order is NORMATIVE (RFC-0020 §2): burst, rising, falling,
    // stable. RISING/FALLING additionally require a REAL trend (|slope| >=
    // half a step per step — 0.5 in Q16 is 32768) — the EWMA slope stalls
    // at a small nonzero residue on a constant signal, and a plateau must
    // read STABLE.
    // Threshold guards are u32-vs-u32 (the C's unsigned compares) — the
    // masks make that explicit even for directly-set configuration fields.
    if (deltaRaw >= 0 && (deltaRaw.toLong() and 0xFFFFFFFFL) >= (t.burstDelta.toLong() and 0xFFFFFFFFL)) {
        // (uint32_t)delta_raw == delta_raw on this path (deltaRaw >= 0).
        verdict = TrendVerdict.BURST
        skipN = minOf(predRaw, WEFT_TREND_SKIP_MAX)
        if (skipN == 0) skipN = 1 // a burst demands action
    } else if ((predRaw.toLong() and 0xFFFFFFFFL) >= (t.risingBehind.toLong() and 0xFFFFFFFFL) &&
        t.slopeQ16 >= 32768
    ) {
        verdict = TrendVerdict.RISING
        val skip = if (predRaw >= 1) predRaw - 1 else 0
        skipN = minOf(skip, WEFT_TREND_SKIP_MAX)
    } else if ((predRaw.toLong() and 0xFFFFFFFFL) <= (t.fallingBehind.toLong() and 0xFFFFFFFFL) &&
        t.slopeQ16 <= -32768
    ) {
        verdict = TrendVerdict.FALLING
    } else {
        verdict = TrendVerdict.STABLE
    }

    t.tSamples++
    t.tVerdict[verdict]++
    if (out != null) {
        out.verdict = verdict
        out.predRaw = predRaw
        out.skipN = skipN
    }
    return verdict
}

/// G5-style packed verdict byte: (verdict << 6) | min(skip_n, 63).
fun weftTrendPack(out: TrendOut): Int {
    val skip = minOf(out.skipN, WEFT_TREND_SKIP_MAX)
    return ((out.verdict shl 6) or skip) and 0xFF
}
