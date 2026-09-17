// Governor.swift — RFC 0009: FreshnessGovernor ladder + §cadence
// presentation policies, Swift driver layer (Series 7).
//
// WHY EXISTS: RFC-0009 shipped the governor (the staleness ladder) to C,
// Rust, and TS (PR #9, the nanoseconds work order) and Series 7 brings it
// to the VM ports — this file is the Swift half, arithmetic-identical to
// core/kotlin/Governor.kt, core/dart/governor.dart, and the TS reference
// (packages/core/src/{governor,cadence}.ts); docs/PORTS.md §9.
//
// BOTH HALVES (the Kotlin file's header documents the full ladder; this
// one states the contract once more, Swift-idiomatically):
//
//   1. THE LADDER (FreshnessGovernor) — acts on STALENESS (framesBehind):
//      FastPath (<= fastPathBehind, default 1) / Skip(n) (<= skipBehind,
//      default 4; the n intermediates are dropped BY DECISION and counted —
//      Law 4) / Snapshot (<= snapshotBehind, default 16) / Reseed
//      (rate-limited to one per reseedCooldownMs, default 250; a suppressed
//      Reseed degrades to Snapshot). Kind values 0/1/2/3 are PROTOCOL.
//
//   2. THE CADENCE POLICIES (CadencePolicy) — act on PRESENTATION
//      (latestSeq at display ticks), the Series-7 extension:
//      LATEST_WINS (newest-wins raster) / PACED_INTERPOLATE (display-rate
//      raster one observed period behind; alphaQ12 ladder; saturated alpha
//      holds rather than extrapolates) / BURST_COALESCE (adaptive sub-rate
//      via the Q12 inter-arrival-gap EWMA; K = clamp(round(gapEWMA), 1, 64);
//      present at most once per K ticks). Kind values 0/1/2 are PROTOCOL.
//
// ARITHMETIC PARITY (G5/PC3): step() is a pure function of its trace —
// time is INJECTED (the ladder takes nowMs; the policy counts ticks), all
// arithmetic is Int64/Int32 with non-negative division operands (Swift's
// truncating division matches every port on non-negative operands). The
// canonical traces' FNV-1a-64 hashes are pinned in GovernorTests.swift —
// drift fails the battery locally, no other toolchain needed.
//
// LAW 2 (G4/PC4): step() allocates NOTHING — state is Int64/Int32/Bool;
// the action/decision is the object's OWN identity-stable record, mutated
// in place (the FanoutClaim pattern). Swift's proof is the identity audit
// (===) plus the pinned traces; the JVM's allocated-bytes audit has no
// Swift equivalent — declared, per the repo's per-port honesty culture
// (Swift has no portable allocation counter; instruments-on-device is the
// deferred road).
//
// Pure logic — imports Foundation only (no swift-atomics, no UIKit): the
// module compiles standalone with plain `swiftc` for the xlang emitters.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION (XCTest battery
// runs in apple-packages CI; not compilable in the x86_64 Linux sandbox —
// no Swift toolchain there — the trace-hash pins keep the arithmetic
// honest the moment a toolchain appears).

import Foundation

// ---------------------------------------------------------------------------
// The ladder — RFC-0009 core.
// ---------------------------------------------------------------------------

/// The closed action set. Numeric values are PROTOCOL (G5; do not renumber).
public enum GovernorActionKind {
    public static let fastPath: Int32 = 0
    public static let skip: Int32 = 1
    public static let snapshot: Int32 = 2
    public static let reseed: Int32 = 3
}

/// Identity-stable action record — mutated in place by step(), never
/// allocated per call (G4). skipN is valid only for kind == skip.
public final class GovernorAction {
    public var kind: Int32
    public var skipN: Int32
    fileprivate init(kind: Int32, skipN: Int32) {
        self.kind = kind
        self.skipN = skipN
    }
}

/// Ladder thresholds + cooldown, with RFC-0009's published defaults.
public struct GovernorConfig {
    public var fastPathBehind: Int64
    public var skipBehind: Int64
    public var snapshotBehind: Int64
    public var reseedCooldownMs: Int64
    public init(fastPathBehind: Int64 = 1,
                skipBehind: Int64 = 4,
                snapshotBehind: Int64 = 16,
                reseedCooldownMs: Int64 = 250) {
        self.fastPathBehind = fastPathBehind
        self.skipBehind = skipBehind
        self.snapshotBehind = snapshotBehind
        self.reseedCooldownMs = reseedCooldownMs
    }
}

/// RFC-0009 published defaults (the TS GOVERNOR_DEFAULTS / C macros).
public let governorDefaults = GovernorConfig()

private let neverReseeded: Int64 = -1

/**
 * The staleness ladder. One step per consumer observation; the input is
 * ANY monotonic per-consumer staleness count (FrameCursor's framesBehind,
 * or a fan-out reader's claim.dropped — same semantics per PORTS.md §7).
 * Composed by the app; the governor never touches a Weft, a ring, or a
 * Triad (RFC-0009's "advisory only" lean).
 */
public final class FreshnessGovernor {
    public let cfg: GovernorConfig
    private var lastReseedMs: Int64 = neverReseeded

    /// Law 4: frames dropped BY DECISION (Skip(n) intermediates), distinct
    /// from any ring-level drop counter.
    public private(set) var decidedDrops: Int64 = 0
    /// Emitted Reseeds (post-cooldown only).
    public private(set) var reseeds: Int64 = 0
    /// Total step() calls (advisory).
    public private(set) var steps: Int64 = 0

    /// The identity-stable action record step() returns.
    public let act = GovernorAction(kind: GovernorActionKind.fastPath, skipN: 0)

    public init(config: GovernorConfig = governorDefaults) {
        self.cfg = config
    }

    /// One decision. Pure except the Reseed cooldown bookkeeping; zero
    /// allocation (G4). `nowMs` is caller-injected monotonic milliseconds —
    /// the same (behind, nowMs) trace yields the same actions in every
    /// port (G5).
    @discardableResult
    public func step(framesBehind: Int64, nowMs: Int64) -> GovernorAction {
        steps += 1
        let behind = max(framesBehind, 0)
        let a = act
        if behind <= cfg.fastPathBehind {
            a.kind = GovernorActionKind.fastPath
            a.skipN = 0
        } else if behind <= cfg.skipBehind {
            let n = behind - cfg.fastPathBehind
            decidedDrops += n // Law 4: decided drops are decisions
            a.kind = GovernorActionKind.skip
            a.skipN = Int32(truncatingIfNeeded: n)
        } else if behind <= cfg.snapshotBehind {
            a.kind = GovernorActionKind.snapshot
            a.skipN = 0
        } else {
            // behind > snapshotBehind: Reseed, rate-limited by the cooldown.
            if lastReseedMs == neverReseeded || nowMs - lastReseedMs >= cfg.reseedCooldownMs {
                lastReseedMs = nowMs
                reseeds += 1
                a.kind = GovernorActionKind.reseed
                a.skipN = 0
            } else {
                // Rate-limited: degrade to the best non-rebuild action.
                a.kind = GovernorActionKind.snapshot
                a.skipN = 0
            }
        }
        return a
    }

    /// Reset the cooldown state (a rebuilt consumer starts fresh).
    public func reset() {
        lastReseedMs = neverReseeded
        decidedDrops = 0
        reseeds = 0
        steps = 0
        act.kind = GovernorActionKind.fastPath
        act.skipN = 0
    }
}

// ---------------------------------------------------------------------------
// The cadence policies — RFC-0009 §Cadence presentation policies (Series 7).
// ---------------------------------------------------------------------------

/// The closed policy set. Numeric values are PROTOCOL (PC3; do not renumber).
public enum CadencePolicyKind {
    public static let latestWins: Int32 = 0
    public static let pacedInterpolate: Int32 = 1
    public static let burstCoalesce: Int32 = 2
}

/// Identity-stable decision record — mutated in place by step(), never
/// allocated per call (PC4). interp/alphaQ12 describe the CURRENT raster
/// state (meaningful even when present=false); PACED only.
public final class PresentDecision {
    /// Raster this tick (the app's one draw call).
    public var present: Bool
    /// The current raster state — blend(prev, newest, alphaQ12/4096).
    public var interp: Bool
    /// Blend weight in Q12 (0..4096).
    public var alphaQ12: Int32
    /// Frames coalesced BY DECISION this tick (Law 4).
    public var coalesced: Int64
    /// Seq the presented raster derives from.
    public var presentSeq: Int64
    /// BURST_COALESCE only: the current pacing divisor (advisory HUD).
    public var k: Int32
    fileprivate init() {
        present = false
        interp = false
        alphaQ12 = 0
        coalesced = 0
        presentSeq = 0
        k = 1
    }
}

/// Policy configuration with the RFC-0009 §cadence published defaults.
public struct CadenceConfig {
    public var policy: Int32
    /// BURST_COALESCE: reassess cadence for K, in ticks (the hysteresis).
    public var reassessTicks: Int
    public init(policy: Int32, reassessTicks: Int = 8) {
        self.policy = policy
        self.reassessTicks = reassessTicks
    }
}

/// Q12 one (the saturated blend).
public let cadenceAlphaOneQ12: Int32 = 4096

/// BURST_COALESCE bounds for K.
public let cadenceKMin: Int32 = 1
public let cadenceKMax: Int32 = 64

private let emaOneQ12: Int64 = 4096

/**
 * One cadence policy instance. One step() per DISPLAY TICK; `latestSeq` is
 * the newest seq observed at this tick (a fan-out reader's claim.seq, the
 * cursor's latest) — monotonic by the ring contract; a regressed input is
 * clamped to the high-water mark (defensive, declared).
 */
public final class CadencePolicy {
    public private(set) var cfg: CadenceConfig
    /// Total step() calls — the tick counter (the policy's only clock).
    private var ticks: Int64 = 0

    // --- shared state ---
    private var lastPresentedSeq: Int64 = 0

    // --- PACED_INTERPOLATE state (the two-frame window) ---
    private var prevSeq: Int64 = 0
    private var prevObsTick: Int64 = 0
    private var newestSeq: Int64 = 0
    private var newestObsTick: Int64 = 0
    /// Last PRESENTED (base, target, alpha) triple — the elision key.
    private var lastBaseSeq: Int64 = -1
    private var lastTargetSeq: Int64 = -1
    private var lastAlpha: Int32 = -1

    // --- BURST_COALESCE state ---
    /// Q12 EWMA of inter-arrival gaps (ticks). Init 4096 = gap 1.
    private var ewmaGapQ12: Int64 = 4096
    private var haveGap = false
    private var lastArrivalTick: Int64 = 0
    private var k: Int32 = cadenceKMin
    private var ticksSinceAssess: Int = 0
    private var tickInCycle: Int = 0
    private var lastSeenLatest: Int64 = 0

    // --- counters (advisory, AXIOM T; exact per PC2) ---
    /// Presents issued (real + interpolated).
    public private(set) var presents: Int64 = 0
    /// Frames coalesced BY DECISION (Law 4).
    public private(set) var coalescedByDecision: Int64 = 0
    /// Synthesized (strictly-between blend) presents — invention counted.
    public private(set) var interpFrames: Int64 = 0
    /// PACED: ticks on which a new seq arrived (PC2's arrivalTicks).
    public private(set) var arrivalTicks: Int64 = 0
    /// Ticks with no raster (nothing changed / not on the beat).
    public private(set) var elided: Int64 = 0
    /// BURST: present ticks that found nothing newer (counted, never silent).
    public private(set) var missedPresentTicks: Int64 = 0

    /// The identity-stable decision record step() returns.
    public let act = PresentDecision()

    public init(config: CadenceConfig) {
        self.cfg = config
    }

    public convenience init(policy: Int32) {
        self.init(config: CadenceConfig(policy: policy))
    }

    /// One display tick. Pure function of the arrival trace + internal
    /// state; zero allocation (PC4).
    @discardableResult
    public func step(latestSeq: Int64) -> PresentDecision {
        ticks += 1
        let a = act
        a.present = false
        a.interp = false
        a.alphaQ12 = 0
        a.coalesced = 0
        a.k = k

        switch cfg.policy {
        case CadencePolicyKind.latestWins:
            if latestSeq > lastPresentedSeq {
                a.coalesced = latestSeq - lastPresentedSeq - 1
                coalescedByDecision += a.coalesced
                lastPresentedSeq = latestSeq
                presents += 1
                a.present = true
                a.presentSeq = latestSeq
            } else {
                elided += 1
                a.presentSeq = lastPresentedSeq
            }
            return a

        case CadencePolicyKind.pacedInterpolate:
            if latestSeq > newestSeq {
                // Arrival: close the window, open the next at alpha=0.
                a.coalesced = latestSeq - newestSeq - 1
                coalescedByDecision += a.coalesced
                prevSeq = newestSeq
                prevObsTick = newestObsTick
                newestSeq = latestSeq
                newestObsTick = ticks
                arrivalTicks += 1
                a.interp = true
                a.alphaQ12 = 0
                a.presentSeq = latestSeq
            } else {
                // Interpolation window: advance alpha toward the newest.
                let period = max(newestObsTick - prevObsTick, 1)
                let dt = ticks - newestObsTick
                a.interp = true
                a.alphaQ12 = Int32(min((dt * Int64(cadenceAlphaOneQ12)) / period,
                                       Int64(cadenceAlphaOneQ12)))
                a.presentSeq = newestSeq
            }
            // Present iff the raster triple changed (the elision key).
            if lastBaseSeq != prevSeq || lastTargetSeq != newestSeq || lastAlpha != a.alphaQ12 {
                lastBaseSeq = prevSeq
                lastTargetSeq = newestSeq
                lastAlpha = a.alphaQ12
                presents += 1
                // True blends only (Law 4): endpoint rasters show a REAL
                // frame; synthesis is the strictly-between mixture.
                if prevSeq != 0 && a.alphaQ12 > 0 && a.alphaQ12 < cadenceAlphaOneQ12 {
                    interpFrames += 1
                }
                a.present = true
            } else {
                elided += 1
            }
            return a

        case CadencePolicyKind.burstCoalesce:
            // 1. Arrival detection (high-water clamp).
            let seen = max(lastSeenLatest, latestSeq)
            let arrivals = seen - lastSeenLatest // >= 0 by construction
            lastSeenLatest = seen
            // 2. Gap EWMA (Q12, weight 1/4), updated ONLY on arrival ticks.
            if arrivals > 0 {
                if haveGap {
                    let gap = ticks - lastArrivalTick
                    let target = gap * emaOneQ12
                    let delta = target - ewmaGapQ12
                    if delta >= 0 {
                        ewmaGapQ12 += delta / 4
                    } else {
                        ewmaGapQ12 -= (-delta) / 4
                    }
                } else {
                    haveGap = true // first arrival: no gap observed yet
                }
                lastArrivalTick = ticks
            }
            // 3. Periodic reassess — the hysteresis.
            ticksSinceAssess += 1
            if ticksSinceAssess >= cfg.reassessTicks {
                ticksSinceAssess = 0
                // round(ewmaGapQ12 / 4096), clamped.
                var kNew = Int32((ewmaGapQ12 + emaOneQ12 / 2) / emaOneQ12)
                if kNew < cadenceKMin { kNew = cadenceKMin }
                if kNew > cadenceKMax { kNew = cadenceKMax }
                k = kNew
                a.k = kNew
            }
            // 4. The paced present: at most once per K ticks, newest only.
            tickInCycle += 1
            if tickInCycle >= Int(k) {
                tickInCycle = 0
                if latestSeq > lastPresentedSeq {
                    a.coalesced = latestSeq - lastPresentedSeq - 1
                    coalescedByDecision += a.coalesced
                    lastPresentedSeq = latestSeq
                    presents += 1
                    a.present = true
                    a.presentSeq = latestSeq
                } else {
                    missedPresentTicks += 1
                    a.presentSeq = lastPresentedSeq
                }
            } else {
                elided += 1
                a.presentSeq = lastPresentedSeq
            }
            return a

        default:
            // Closed set — an unknown kind is a programming error, not a
            // runtime path (the battery pins the three kinds).
            fatalError("unknown cadence policy kind: \(cfg.policy)")
        }
    }

    /// Test-only observability for the PC2 telescoping identities.
    func lastPresentedSeqForTest() -> Int64 { lastPresentedSeq }
    func newestSeqForTest() -> Int64 { newestSeq }

    /// Reset to a freshly-constructed state for `policy` (counters and
    /// window included). Used by PC6 switch tests and consumer rebuilds.
    public func reset(policy: Int32? = nil) {
        if let policy = policy {
            cfg = CadenceConfig(policy: policy, reassessTicks: cfg.reassessTicks)
        }
        ticks = 0
        lastPresentedSeq = 0
        prevSeq = 0
        prevObsTick = 0
        newestSeq = 0
        newestObsTick = 0
        lastBaseSeq = -1
        lastTargetSeq = -1
        lastAlpha = -1
        ewmaGapQ12 = 4096
        haveGap = false
        lastArrivalTick = 0
        k = cadenceKMin
        ticksSinceAssess = 0
        tickInCycle = 0
        lastSeenLatest = 0
        presents = 0
        coalescedByDecision = 0
        interpFrames = 0
        arrivalTicks = 0
        elided = 0
        missedPresentTicks = 0
        act.present = false
        act.interp = false
        act.alphaQ12 = 0
        act.coalesced = 0
        act.presentSeq = 0
        act.k = cadenceKMin
    }
}
