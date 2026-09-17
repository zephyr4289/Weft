// Governor.kt — RFC 0009: FreshnessGovernor ladder + §cadence presentation
// policies, Kotlin/JVM driver layer (Series 7).
//
// WHY EXISTS: RFC-0009 shipped the governor (the staleness ladder) to C,
// Rust, and TS (PR #9, the nanoseconds work order) — but the three VM
// ports had no equivalent: Android apps and JVM engines consuming a fan-out
// ring or a Triad hand-rolled the skip/snapshot/rebuild ladder per screen,
// tuned by vibes, untestable. This module brings BOTH halves of RFC-0009 to
// Kotlin with arithmetic-identical behavior (docs/PORTS.md §9):
//
//   1. THE LADDER (FreshnessGovernor) — acts on STALENESS (framesBehind):
//        FastPath  behind <= fastPathBehind (default 1). Draw live.
//        Skip(n)   behind <= skipBehind (default 4). Draw newest only; the
//                  n = behind - fastPathBehind intermediates are dropped BY
//                  DECISION and counted (Law 4, distinct from ring counters).
//        Snapshot  behind <= snapshotBehind (default 16). Draw once, re-sync.
//        Reseed    behind > snapshotBehind. Rebuild; rate-limited to one per
//                  reseedCooldownMs (default 250) — a suppressed Reseed
//                  degrades to Snapshot (the documented fallback).
//      Kind values 0/1/2/3 are PROTOCOL (G5 packs them into trace bytes).
//
//   2. THE CADENCE POLICIES (CadencePolicy) — act on PRESENTATION
//      (latestSeq at display ticks), the Series-7 extension:
//        LATEST_WINS       present iff seq advanced; jumps coalesced by
//                          decision. At most one present per tick.
//        PACED_INTERPOLATE display-rate raster one observed period behind:
//                          blend(prev, newest, alphaQ12) with
//                          alphaQ12 = min(4096, dt*4096/period), period =
//                          observed inter-arrival ticks; arrival ticks
//                          re-window at alpha 0 — continuous by
//                          construction; saturated alpha holds (elides)
//                          rather than extrapolating.
//        BURST_COALESCE    adaptive sub-rate: Q12 EWMA (weight 1/4) of
//                          inter-arrival gaps; every reassessTicks ticks
//                          K = clamp(round(gapEWMA), 1, 64); present at
//                          most once per K ticks, newest only. K=1 at/above
//                          display rate; K locks onto the content beat at
//                          steady sub-rates (30 Hz on 120 Hz -> K=4).
//      Kind values 0/1/2 are PROTOCOL (PC3 packs them in stream order).
//
// ARITHMETIC PARITY (G5/PC3): step() is a pure function of its trace —
// time is INJECTED (the ladder takes nowMs; the policy counts ticks
// internally, no clock reads), all arithmetic is Long/Int with
// non-negative division operands (identical truncation in every port).
// The same trace MUST produce the identical packed action/decision log in
// TS, C, Rust (ladder) and TS/Kotlin/Swift/Dart (policies) — proven by
// fixtures/xlang-governor (G5, this port joins via fixtures/xlang-governor/
// vm/) and fixtures/xlang-cadence (PC3).
//
// LAW 2 (G4/PC4): step() allocates NOTHING — state is Longs/Ints; the
// returned action/decision is the object's OWN identity-stable record,
// mutated in place (the FanoutClaim pattern). Read it synchronously; do
// not retain it across steps. The JVM proof is stronger than an audit:
// GovernorTest's G4 leg measures ThreadMXBean allocated bytes across 100k
// steps and asserts a ZERO delta (HotSpot-only, guarded — the identity
// check pins the contract everywhere else).
//
// LAW 4: decidedDrops/coalescedByDecision count frames dropped BY DECISION;
// interpFrames counts synthesized (strictly-between blend) presents —
// invention is a decision too. Exact telescoping identities (PC2):
//   LATEST_WINS / BURST_COALESCE: sum(coalesced) == lastPresentedSeq - presents
//   PACED_INTERPOLATE:            sum(coalesced) == newestSeq - arrivalTicks
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION (JVM battery green;
// android-packages gradle CI is the cover).

package dev.weft

// ---------------------------------------------------------------------------
// The ladder — RFC-0009 core (arithmetic-identical to governor.ts /
// governor.c / governor.rs).
// ---------------------------------------------------------------------------

/** The closed action set. Numeric values are PROTOCOL (G5; do not renumber). */
object GovernorActionKind {
    const val FAST_PATH: Int = 0
    const val SKIP: Int = 1
    const val SNAPSHOT: Int = 2
    const val RESEED: Int = 3
}

/// Identity-stable action record — mutated in place by step(), never
/// allocated per call (G4). skipN is valid only for kind == SKIP.
class GovernorAction(
    var kind: Int,
    var skipN: Int
)

/// Ladder thresholds + cooldown, with RFC-0009's published defaults.
class GovernorConfig(
    val fastPathBehind: Long,
    val skipBehind: Long,
    val snapshotBehind: Long,
    val reseedCooldownMs: Long
)

val GOVERNOR_DEFAULTS = GovernorConfig(
    fastPathBehind = 1L,
    skipBehind = 4L,
    snapshotBehind = 16L,
    reseedCooldownMs = 250L
)

private const val NEVER_RESEEDED: Long = -1L

/**
 * The staleness ladder. One step per consumer observation; the input is ANY
 * monotonic per-consumer staleness count (FrameCursor's framesBehind, or a
 * fan-out reader's claim.dropped — same semantics per PORTS.md §7).
 * Composed by the app; the governor never touches a Weft, a ring, or a
 * Triad (RFC-0009's "advisory only" lean).
 */
class FreshnessGovernor(
    /** The ladder + cooldown. The ladder SHAPE is the spec (RFC-0009). */
    config: GovernorConfig = GOVERNOR_DEFAULTS
) {
    val cfg: GovernorConfig = config
    private var lastReseedMs: Long = NEVER_RESEEDED

    /// Law 4: frames dropped BY DECISION (Skip(n) intermediates), distinct
    /// from any ring-level drop counter.
    var decidedDrops: Long = 0
        private set
    /// Emitted Reseeds (post-cooldown only).
    var reseeds: Long = 0
        private set
    /// Total step() calls (advisory).
    var steps: Long = 0
        private set

    /// The identity-stable action record step() returns.
    val act: GovernorAction = GovernorAction(GovernorActionKind.FAST_PATH, 0)

    /// One decision. Pure except the Reseed cooldown bookkeeping; zero
    /// allocation (G4). `nowMs` is caller-injected monotonic milliseconds —
    /// the same (behind, nowMs) trace yields the same actions in every
    /// port (G5).
    fun step(framesBehind: Long, nowMs: Long): GovernorAction {
        steps++
        val behind = framesBehind.coerceAtLeast(0L)
        val a = act
        if (behind <= cfg.fastPathBehind) {
            a.kind = GovernorActionKind.FAST_PATH
            a.skipN = 0
        } else if (behind <= cfg.skipBehind) {
            val n = behind - cfg.fastPathBehind
            decidedDrops += n // Law 4: decided drops are decisions
            a.kind = GovernorActionKind.SKIP
            a.skipN = n.toInt()
        } else if (behind <= cfg.snapshotBehind) {
            a.kind = GovernorActionKind.SNAPSHOT
            a.skipN = 0
        } else {
            // behind > snapshotBehind: Reseed, rate-limited by the cooldown.
            if (lastReseedMs == NEVER_RESEEDED ||
                nowMs - lastReseedMs >= cfg.reseedCooldownMs
            ) {
                lastReseedMs = nowMs
                reseeds++
                a.kind = GovernorActionKind.RESEED
                a.skipN = 0
            } else {
                // Rate-limited: degrade to the best non-rebuild action.
                a.kind = GovernorActionKind.SNAPSHOT
                a.skipN = 0
            }
        }
        return a
    }

    /// Reset the cooldown state (a rebuilt consumer starts fresh).
    fun reset() {
        lastReseedMs = NEVER_RESEEDED
        decidedDrops = 0
        reseeds = 0
        steps = 0
        act.kind = GovernorActionKind.FAST_PATH
        act.skipN = 0
    }
}

// ---------------------------------------------------------------------------
// The cadence policies — RFC-0009 §Cadence presentation policies (Series 7;
// arithmetic-identical to packages/core/src/cadence.ts, Governor.swift,
// governor.dart).
// ---------------------------------------------------------------------------

/** The closed policy set. Numeric values are PROTOCOL (PC3; do not renumber). */
object CadencePolicyKind {
    const val LATEST_WINS: Int = 0
    const val PACED_INTERPOLATE: Int = 1
    const val BURST_COALESCE: Int = 2
}

/// Identity-stable decision record — mutated in place by step(), never
/// allocated per call (PC4). interp/alphaQ12 describe the CURRENT raster
/// state (meaningful even when present=false); PACED only.
class PresentDecision(
    var present: Boolean,
    var interp: Boolean,
    /** Blend weight in Q12 (0..4096). */
    var alphaQ12: Int,
    /// Frames coalesced BY DECISION this tick (Law 4).
    var coalesced: Long,
    /// Seq the presented raster derives from.
    var presentSeq: Long,
    /// BURST_COALESCE only: the current pacing divisor (advisory HUD).
    var k: Int
)

/// Policy configuration with the RFC-0009 §cadence published defaults.
class CadenceConfig(
    val policy: Int,
    /// BURST_COALESCE: reassess cadence for K, in ticks (the hysteresis).
    val reassessTicks: Int = 8
)

/// Q12 one (the saturated blend).
const val CADENCE_ALPHA_ONE_Q12: Int = 4096

/// BURST_COALESCE bounds for K.
const val CADENCE_K_MIN: Int = 1
const val CADENCE_K_MAX: Int = 64

private const val EMA_ONE_Q12: Long = 4096L

/**
 * One cadence policy instance. One step() per DISPLAY TICK; `latestSeq` is
 * the newest seq observed at this tick (a fan-out reader's claim.seq, the
 * cursor's latest) — monotonic by the ring contract; a regressed input is
 * clamped to the high-water mark (defensive, declared).
 */
class CadencePolicy(
    /** The closed policy set member; the ladder SHAPE is the spec. */
    config: CadenceConfig
) {
    var cfg: CadenceConfig = config
        private set
    /// Total step() calls — the tick counter (the policy's only clock).
    private var ticks: Long = 0

    // --- shared state ---
    private var lastPresentedSeq: Long = 0

    // --- PACED_INTERPOLATE state (the two-frame window) ---
    private var prevSeq: Long = 0
    private var prevObsTick: Long = 0
    private var newestSeq: Long = 0
    private var newestObsTick: Long = 0
    /// Last PRESENTED (base, target, alpha) triple — the elision key.
    private var lastBaseSeq: Long = -1
    private var lastTargetSeq: Long = -1
    private var lastAlpha: Int = -1

    // --- BURST_COALESCE state ---
    /// Q12 EWMA of inter-arrival gaps (ticks). Init 4096 = gap 1.
    private var ewmaGapQ12: Long = 4096
    private var haveGap: Boolean = false
    private var lastArrivalTick: Long = 0
    private var k: Int = CADENCE_K_MIN
    private var ticksSinceAssess: Int = 0
    private var tickInCycle: Int = 0
    private var lastSeenLatest: Long = 0

    // --- counters (advisory, AXIOM T; exact per PC2) ---
    /// Presents issued (real + interpolated).
    var presents: Long = 0
        private set
    /// Frames coalesced BY DECISION (Law 4).
    var coalescedByDecision: Long = 0
        private set
    /// Synthesized (strictly-between blend) presents — invention counted.
    var interpFrames: Long = 0
        private set
    /// PACED: ticks on which a new seq arrived (PC2's arrivalTicks).
    var arrivalTicks: Long = 0
        private set
    /// Ticks with no raster (nothing changed / not on the beat).
    var elided: Long = 0
        private set
    /// BURST: present ticks that found nothing newer (counted, never silent).
    var missedPresentTicks: Long = 0
        private set

    /// The identity-stable decision record step() returns.
    val act: PresentDecision = PresentDecision(
        present = false, interp = false, alphaQ12 = 0,
        coalesced = 0, presentSeq = 0, k = CADENCE_K_MIN
    )

    /** Convenience: default reassess cadence. */
    constructor(policy: Int) : this(CadenceConfig(policy))

    /// One display tick. Pure function of the arrival trace + internal
    /// state; zero allocation (PC4).
    fun step(latestSeq: Long): PresentDecision {
        ticks++
        val a = act
        a.present = false
        a.interp = false
        a.alphaQ12 = 0
        a.coalesced = 0
        a.k = k

        when (cfg.policy) {
            CadencePolicyKind.LATEST_WINS -> {
                if (latestSeq > lastPresentedSeq) {
                    a.coalesced = latestSeq - lastPresentedSeq - 1
                    coalescedByDecision += a.coalesced
                    lastPresentedSeq = latestSeq
                    presents++
                    a.present = true
                    a.presentSeq = latestSeq
                } else {
                    elided++
                    a.presentSeq = lastPresentedSeq
                }
                return a
            }

            CadencePolicyKind.PACED_INTERPOLATE -> {
                if (latestSeq > newestSeq) {
                    // Arrival: close the window, open the next at alpha=0.
                    a.coalesced = latestSeq - newestSeq - 1
                    coalescedByDecision += a.coalesced
                    prevSeq = newestSeq
                    prevObsTick = newestObsTick
                    newestSeq = latestSeq
                    newestObsTick = ticks
                    arrivalTicks++
                    a.interp = true
                    a.alphaQ12 = 0
                    a.presentSeq = latestSeq
                } else {
                    // Interpolation window: advance alpha toward the newest.
                    val period = (newestObsTick - prevObsTick).coerceAtLeast(1L)
                    val dt = ticks - newestObsTick
                    a.interp = true
                    a.alphaQ12 = ((dt * CADENCE_ALPHA_ONE_Q12) / period)
                        .coerceAtMost(CADENCE_ALPHA_ONE_Q12.toLong()).toInt()
                    a.presentSeq = newestSeq
                }
                // Present iff the raster triple changed (the elision key).
                if (lastBaseSeq != prevSeq || lastTargetSeq != newestSeq || lastAlpha != a.alphaQ12) {
                    lastBaseSeq = prevSeq
                    lastTargetSeq = newestSeq
                    lastAlpha = a.alphaQ12
                    presents++
                    // True blends only: endpoint rasters (alpha 0/4096, or
                    // the pre-history window prev==0) show a REAL frame;
                    // synthesis is the strictly-between mixture (Law 4).
                    if (prevSeq != 0L && a.alphaQ12 > 0 && a.alphaQ12 < CADENCE_ALPHA_ONE_Q12) {
                        interpFrames++
                    }
                    a.present = true
                } else {
                    elided++
                }
                return a
            }

            CadencePolicyKind.BURST_COALESCE -> {
                // 1. Arrival detection (high-water clamp — the transport
                //    never regresses; a defensive input is absorbed).
                val seen = maxOf(lastSeenLatest, latestSeq)
                val arrivals = seen - lastSeenLatest // >= 0 by construction
                lastSeenLatest = seen
                // 2. Gap EWMA (Q12, weight 1/4), updated ONLY on arrival
                //    ticks — the inter-arrival gap is the content-cadence
                //    estimate; a constant gap converges exactly. The
                //    non-negative split keeps truncation identical in
                //    every port.
                if (arrivals > 0) {
                    if (haveGap) {
                        val gap = ticks - lastArrivalTick
                        val target = gap * EMA_ONE_Q12
                        val delta = target - ewmaGapQ12
                        ewmaGapQ12 += if (delta >= 0) delta / 4 else -((-delta) / 4)
                    } else {
                        haveGap = true // first arrival: no gap observed yet
                    }
                    lastArrivalTick = ticks
                }
                // 3. Periodic reassess — the hysteresis: K changes at most
                //    once per reassessTicks window.
                ticksSinceAssess++
                if (ticksSinceAssess >= cfg.reassessTicks) {
                    ticksSinceAssess = 0
                    // round(ewmaGapQ12 / 4096), clamped.
                    var kNew = ((ewmaGapQ12 + EMA_ONE_Q12 / 2) / EMA_ONE_Q12).toInt()
                    if (kNew < CADENCE_K_MIN) kNew = CADENCE_K_MIN
                    if (kNew > CADENCE_K_MAX) kNew = CADENCE_K_MAX
                    k = kNew
                    a.k = kNew
                }
                // 4. The paced present: at most once per K ticks, newest only.
                tickInCycle++
                if (tickInCycle >= k) {
                    tickInCycle = 0
                    if (latestSeq > lastPresentedSeq) {
                        a.coalesced = latestSeq - lastPresentedSeq - 1
                        coalescedByDecision += a.coalesced
                        lastPresentedSeq = latestSeq
                        presents++
                        a.present = true
                        a.presentSeq = latestSeq
                    } else {
                        missedPresentTicks++
                        a.presentSeq = lastPresentedSeq
                    }
                } else {
                    elided++
                    a.presentSeq = lastPresentedSeq
                }
                return a
            }

            else -> {
                // Closed set — an unknown kind is a programming error, not
                // a runtime path (the battery pins the three kinds).
                throw IllegalStateException("unknown cadence policy kind: ${cfg.policy}")
            }
        }
    }

    /// Test-only observability for the PC2 telescoping identities: the
    /// public counters plus these two endpoints close the equations.
    /// internal — the battery lives in the same compilation unit.
    internal fun lastPresentedSeqForTest(): Long = lastPresentedSeq
    internal fun newestSeqForTest(): Long = newestSeq

    /// Reset to a freshly-constructed state for `policy` (counters and
    /// window included). Used by PC6 switch tests and consumer rebuilds.
    fun reset(policy: Int = cfg.policy) {
        cfg = CadenceConfig(policy, cfg.reassessTicks)
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
        k = CADENCE_K_MIN
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
        act.k = CADENCE_K_MIN
    }
}
