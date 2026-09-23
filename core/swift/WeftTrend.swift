// WeftTrend.swift — RFC 0020: predictive lag-trend estimator, Swift driver
// layer.
//
// WHY EXISTS: RFC-0020's estimator (a 1-D alpha-beta filter in pure
// integer Q16 — level = EWMA of staleness, slope = EWMA of its per-step
// change, a HORIZON-step projection, and a CLOSED verdict set
// STABLE/RISING/FALLING/BURST with a proactive skip_n) shipped as the C
// reference (core/c/weft_trend.{h,c}) with TS/Kotlin twins — this file is
// the Swift half, arithmetic-identical to all of them. The governor ladder
// is untouched — this is a sensor the consumer MAY consult (Law 3).
// Verdict values are PROTOCOL (G5 packs verdict<<6|skip into trace bytes —
// do not renumber).
//
// ARITHMETIC PARITY: every C integer op is mirrored op-for-op, including
// the u32 level wrap, the i32 slope truncation (truncatingIfNeeded: = the
// C's narrowing casts), the arithmetic >>8 (Int64 >> IS arithmetic —
// matches C), the saturating projection, and the verdict ordering (burst,
// rising, falling, stable — NORMATIVE). Int64 intermediates carry the C's
// (int64_t) widenings: UInt32→Int64 zero-extends, Int32→Int64 sign-extends.
// The REAL i32 wraps (delta_raw, level_err) use the wrapping &- — Swift's
// plain - traps on overflow, the C wraps. The slope guards (|slope| >=
// half a step per step) keep constant-signal plateaus at STABLE — the EWMA
// slope stalls at a small nonzero residue on a constant signal, and a
// plateau must read STABLE.
//
// PINNED PARITY VECTOR (from the C reference — do not "fix" it): the
// 2000-sample xorshift verdict stream (behind = state % 64, seed
// 0x00C0FFEE) hashes (FNV-1a over the hex stream) to 0x11187b9a02b378ef.
//
// observe() allocates NOTHING — the result is written into a caller-owned
// TrendOut record (the GovernorAction identity-stable-record pattern).
//
// Pure logic — no imports (Swift stdlib only): the module compiles
// standalone with plain `swiftc` for the xlang emitters.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION (not compilable in
// the x86_64 Linux sandbox — no Swift toolchain; the arithmetic was
// validated op-for-op against the C reference before translation, and the
// fixture gate runs when apple-packages CI brings the toolchain).

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

/// Verdicts — PROTOCOL values (packed into trace bytes; do not renumber).
public enum TrendVerdict {
    public static let stable: Int = 0
    public static let rising: Int = 1
    public static let falling: Int = 2
    public static let burst: Int = 3
}

/// Projection distance (steps).
public let weftTrendHorizon: UInt32 = 8

/// G5 packing cap (kind<<6 | skip).
public let weftTrendSkipMax: UInt32 = 63

/// Defaults (RFC-0020 §1-2).
public let weftTrendAlphaQ8Default: UInt32 = 48  // 0.1875
public let weftTrendBetaQ8Default: UInt32 = 96   // 0.375 — fast-trend pair
public let weftTrendBurstDefault: UInt32 = 32
public let weftTrendRisingDefault: UInt32 = 4    // the governor's Skip rung
public let weftTrendFallingDefault: UInt32 = 1

// ---------------------------------------------------------------------------
// State + result records
// ---------------------------------------------------------------------------

/// One projection result. skipN is valid for RISING/BURST; 0 otherwise.
public struct TrendOut {
    public var verdict: Int
    public var predRaw: UInt32 // projected behind at HORIZON steps
    public var skipN: UInt32   // recommended proactive skip
    public init(verdict: Int = TrendVerdict.stable, predRaw: UInt32 = 0, skipN: UInt32 = 0) {
        self.verdict = verdict
        self.predRaw = predRaw
        self.skipN = skipN
    }
}

/// The estimator. Call weftTrendInit(_:) (or construct + init) before the
/// first observe — init() is the C contract, kept explicit.
public struct WeftTrend {
    // estimator state (Q16)
    public var levelQ16: UInt32 = 0 // u32 — unsigned wrap on update
    public var slopeQ16: Int32 = 0  // i32 — signed: falling trends have negative slope
    public var lastRaw: Int32 = 0   // i32
    public var hasLast: Bool = false

    // configuration (Q8 fractions of 256 / raw thresholds)
    public var alphaQ8: UInt32 = weftTrendAlphaQ8Default
    public var betaQ8: UInt32 = weftTrendBetaQ8Default
    public var burstDelta: UInt32 = weftTrendBurstDefault
    public var risingBehind: UInt32 = weftTrendRisingDefault
    public var fallingBehind: UInt32 = weftTrendFallingDefault

    // advisory telemetry
    public var tSamples: UInt64 = 0
    public var tVerdict: [UInt64] = [0, 0, 0, 0]

    public init() {}
}

// ---------------------------------------------------------------------------
// Init / configure
// ---------------------------------------------------------------------------

/// Init with the RFC defaults.
public func weftTrendInit(_ t: inout WeftTrend) {
    t = WeftTrend()
}

private func clampU32(_ v: UInt32, _ lo: UInt32, _ hi: UInt32) -> UInt32 {
    return v < lo ? lo : (v > hi ? hi : v)
}

/// Override the configuration (Q8 gains, thresholds). Values are clamped:
/// gains to [1, 256], burst to [1, 1<<30], thresholds to [0, 1<<20].
public func weftTrendConfigure(_ t: inout WeftTrend, alphaQ8: UInt32, betaQ8: UInt32,
                               burstDelta: UInt32, risingBehind: UInt32,
                               fallingBehind: UInt32) {
    t.alphaQ8 = clampU32(alphaQ8, 1, 256)
    t.betaQ8 = clampU32(betaQ8, 1, 256)
    t.burstDelta = clampU32(burstDelta, 1, 1 << 30)
    t.risingBehind = clampU32(risingBehind, 0, 1 << 20)
    t.fallingBehind = clampU32(fallingBehind, 0, 1 << 20)
}

// ---------------------------------------------------------------------------
// Observe
// ---------------------------------------------------------------------------

extension WeftTrend {
    /// Observe one staleness sample. Pure integer step; writes the
    /// projection to `out`. Returns the verdict for convenience.
    ///
    /// The arithmetic is NORMATIVE (weft_trend.c) — mirror, never redesign:
    ///   level += alpha * (raw - level)   (Q16, gain Q8)
    ///   slope += beta * (delta - slope)  (Q16 per step)
    ///   pred   = level + slope * HORIZON (saturating shift)
    /// (The clamped gains bound every Int64 product below — no overflow.)
    @discardableResult
    public mutating func observe(behind: UInt32, out: inout TrendOut) -> Int {
        var verdict = TrendVerdict.stable
        var predRaw: UInt32 = 0
        var skipN: UInt32 = 0

        if !hasLast {
            // first sample seeds level, zero slope (declared: no prediction
            // is possible from one sample — STABLE until the filter settles)
            levelQ16 = behind << 16 // u32 shl — overflow bits discarded, as in C
            slopeQ16 = 0
            lastRaw = Int32(bitPattern: behind)
            hasLast = true
            verdict = TrendVerdict.stable
            predRaw = behind
            tSamples &+= 1
            tVerdict[TrendVerdict.stable] &+= 1
            out.verdict = verdict
            out.predRaw = predRaw
            out.skipN = skipN
            return verdict
        }

        let deltaRaw: Int32 = Int32(bitPattern: behind) &- lastRaw
        lastRaw = Int32(bitPattern: behind)

        // level += alpha * (raw - level)   (Q16, gain Q8)
        let levelErr: Int32 = Int32(bitPattern: behind << 16) &- Int32(bitPattern: levelQ16)
        let levelSum: Int64 = Int64(levelQ16) + ((Int64(alphaQ8) * Int64(levelErr)) >> 8)
        levelQ16 = UInt32(truncatingIfNeeded: levelSum)

        // slope += beta * (delta - slope)  (Q16 per step)
        let deltaQ16: Int64 = Int64(deltaRaw) << 16
        let slopeSum: Int64 = Int64(slopeQ16) +
            ((Int64(betaQ8) * (deltaQ16 - Int64(slopeQ16))) >> 8)
        slopeQ16 = Int32(truncatingIfNeeded: slopeSum)

        // projection: level + slope * HORIZON (saturating shift)
        var predQ16: Int64 = Int64(levelQ16) + Int64(slopeQ16) * Int64(weftTrendHorizon)
        if predQ16 < 0 { predQ16 = 0 }
        if predQ16 > Int64(0x7FFFFFFF) { predQ16 = Int64(0x7FFFFFFF) }
        predRaw = UInt32(truncatingIfNeeded: predQ16 >> 16)

        // Verdict order is NORMATIVE (RFC-0020 §2): burst, rising, falling,
        // stable. RISING/FALLING additionally require a REAL trend (|slope|
        // >= half a step per step — 0.5 in Q16 is 32768) — the EWMA slope
        // stalls at a small nonzero residue on a constant signal, and a
        // plateau must read STABLE.
        if deltaRaw >= 0 && UInt32(bitPattern: deltaRaw) >= burstDelta {
            verdict = TrendVerdict.burst
            skipN = predRaw > weftTrendSkipMax ? weftTrendSkipMax : predRaw
            if skipN == 0 { skipN = 1 } // a burst demands action
        } else if predRaw >= risingBehind && slopeQ16 >= 32768 {
            verdict = TrendVerdict.rising
            let skip: UInt32 = predRaw >= 1 ? predRaw - 1 : 0
            skipN = skip > weftTrendSkipMax ? weftTrendSkipMax : skip
        } else if predRaw <= fallingBehind && slopeQ16 <= -32768 {
            verdict = TrendVerdict.falling
        } else {
            verdict = TrendVerdict.stable
        }

        tSamples &+= 1
        tVerdict[verdict] &+= 1
        out.verdict = verdict
        out.predRaw = predRaw
        out.skipN = skipN
        return verdict
    }
}

/// G5-style packed verdict byte: (verdict << 6) | min(skip_n, 63).
public func weftTrendPack(_ out: TrendOut) -> UInt8 {
    let skip: UInt32 = out.skipN > weftTrendSkipMax ? weftTrendSkipMax : out.skipN
    return UInt8(truncatingIfNeeded: (UInt32(out.verdict) << 6) | skip)
}
