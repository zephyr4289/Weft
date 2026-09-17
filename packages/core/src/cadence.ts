// cadence.ts — RFC-0009 §Cadence presentation policies (Series 7), TS reference.
//
// WHY EXISTS: the governor ladder (governor.ts) decides which CLASS of
// action fits a consumer's staleness; the display loop still hand-rolls
// WHAT TO RASTER each vsync — "draw if new, else skip" (LATEST_WINS),
// "always draw, lerp the last two" (PACED_INTERPOLATE), "throttle to the
// content beat when the feed is bursty" (BURST_COALESCE). Duplicated per
// app, tuned by vibes, untestable. This module is the spec'd, pure,
// zero-allocation version — one step() per DISPLAY TICK, closed decision
// record, integer-only arithmetic (Q12 fixed point), no clock reads:
//
//     ticker ──tick + latestSeq──> CADENCE POLICY ──{present? interp? alphaQ12 coalesced}──> draw loop
//
// THE POLICIES (closed set — a fourth is a new RFC):
//   LATEST_WINS (0)      present iff latestSeq advanced; jumped frames are
//                        coalesced BY DECISION (Law 4). At most one
//                        present per tick, never a stale re-raster.
//   PACED_INTERPOLATE(1) display-rate raster, one observed period behind:
//                        present blend(prev, newest, alpha) with
//                        alphaQ12 = clamp((tick - newestObsTick)*4096 /
//                        period, 0, 4096), period = observed inter-arrival
//                        ticks. Arrival ticks re-window at alpha=0 — the
//                        raster is continuous BY CONSTRUCTION (the closed
//                        window's saturated blend equals the new window's
//                        alpha-0 blend). Never extrapolates past newest
//                        (saturated alpha = honest holding).
//   BURST_COALESCE (2)   adaptive sub-rate: integer Q12 EWMA (weight 1/4)
//                        of inter-arrival GAPS (ticks between arrival
//                        ticks); every REASSESS_TICKS ticks K =
//                        clamp(round(gapEWMA), 1, 64); present at most
//                        once per K ticks (newest at each present tick).
//                        K->1 at/above display rate (degrades to
//                        newest-wins); K locks onto the content beat at
//                        steady sub-rates (30 Hz on 120 Hz -> K=4); burst
//                        feeds pace at one present per burst. No queueing
//                        (the RFC-0009 alternative rejection): bursts
//                        absorb latest-wins, only the present pace adapts.
//
// DETERMINISM (PC3): step() is a pure function of the (latestSeq) tick
// trace — ticks are counted internally, not read from a clock; all
// arithmetic is integer with non-negative division operands (identical
// truncation in TS/Kotlin/Swift/Dart/C). The same arrival trace MUST
// produce the identical packed decision log in every port
// (fixtures/xlang-cadence/).
//
// Zero allocation per step (Law 2, PC4): state is integers; the decision
// record is the policy's OWN identity-stable object, mutated in place —
// the weft_gov_action_t / governor.ts `act` pattern. Read it
// synchronously; do not retain it across steps.
//
// LAW 4: `coalesced` counts frames dropped BY DECISION (never presented,
// never silent); `interpFrames` counts synthesized presents (invention is
// a decision too). Both are advisory-per-AXIOM-T but exact per PC2's
// telescoping identities:
//   LATEST_WINS / BURST_COALESCE: sum(coalesced) == lastPresentedSeq - presents
//   PACED_INTERPOLATE:           sum(coalesced) == newestSeq - arrivalTicks

/// The closed policy set (plain const object, house style). Numeric values
/// are PROTOCOL (PC3 packs them into the fixture's stream order; do not
/// renumber).
export const CadencePolicyKind = {
  LATEST_WINS: 0,
  PACED_INTERPOLATE: 1,
  BURST_COALESCE: 2,
} as const;
export type CadencePolicyKind =
  (typeof CadencePolicyKind)[keyof typeof CadencePolicyKind];

/// Identity-stable decision record — mutated in place by step(), never
/// allocated per call (PC4). `alphaQ12` is valid when `interp` is true
/// (0..4096); `presentSeq` is the seq the raster is built from (the
/// newest for non-interp, the target of the blend for interp).
export interface PresentDecision {
  /** Raster this tick (the app's one draw call). */
  present: boolean;
  /** The current raster state — blend(prev, newest, alphaQ12/4096) —
   *  meaningful even when present=false (it describes what WOULD be
   *  drawn); prev==0 or alphaQ12==4096 means "draw newest directly".
   *  PACED_INTERPOLATE only; false on the other policies. */
  interp: boolean;
  /** Blend weight in Q12 (0..4096). */
  alphaQ12: number;
  /** Frames coalesced BY DECISION this tick (Law 4). */
  coalesced: number;
  /** Seq the presented raster derives from. */
  presentSeq: number;
  /** BURST_COALESCE only: the current pacing divisor (advisory HUD). */
  k: number;
}

/// Policy configuration with the RFC-0009 §cadence published defaults.
export interface CadenceConfig {
  /// The closed policy set member.
  policy: CadencePolicyKind;
  /// BURST_COALESCE: reassess cadence for K, in ticks (hysteresis — K
  /// changes at most once per window). Default 8.
  reassessTicks: number;
}

export const CADENCE_DEFAULTS: Readonly<Omit<CadenceConfig, 'policy'>> = {
  reassessTicks: 8,
};

/// Q12 one (the saturated blend).
export const CADENCE_ALPHA_ONE_Q12 = 4096;

/// BURST_COALESCE bounds for K (present every K ticks; 64 caps stall
/// recovery — with ewma==0 K parks at the cap, presents stop until
/// content resumes, and the reassess window bounds re-lock latency).
export const CADENCE_K_MIN = 1;
export const CADENCE_K_MAX = 64;

/// EMA weight in Q12: 1024/4096 = 1/4 (integer-exact — the delta update
/// divides a non-negative remainder by 4 in every port).
const EMA_WEIGHT_Q12 = 1024;
const EMA_ONE_Q12 = 4096;

export class CadencePolicy {
  private readonly cfg: CadenceConfig;
  /// Total step() calls — the tick counter (the policy's only clock).
  private ticks = 0;

  // --- shared state ---
  private lastPresentedSeq = 0;

  // --- PACED_INTERPOLATE state (the two-frame window) ---
  private prevSeq = 0;
  private prevObsTick = 0;
  private newestSeq = 0;
  private newestObsTick = 0;
  /// Last PRESENTED (base, target, alpha) triple — the elision key: a
  /// tick presents iff its raster differs from the last presented one.
  private lastBaseSeq = -1;
  private lastTargetSeq = -1;
  private lastAlpha = -1;

  // --- BURST_COALESCE state ---
  /// Q12 EWMA of inter-arrival gaps (ticks). Init 4096 = gap 1 (the
  ///  conservative default: present whenever content is new).
  private ewmaGapQ12 = 4096;
  private haveGap = false;
  private lastArrivalTick = 0;
  private k = 1;
  private ticksSinceAssess = 0;
  private tickInCycle = 0;
  private lastSeenLatest = 0;

  // --- counters (advisory, AXIOM T; exact per PC2) ---
  /// Presents issued (real + interpolated).
  presents = 0;
  /// Frames coalesced BY DECISION (Law 4).
  coalescedByDecision = 0;
  /// Synthesized (interpolated) presents — invention counted, per the
  /// RFC's Law-4-for-synthesis stance.
  interpFrames = 0;
  /// PACED: ticks on which a new seq arrived (PC2's arrivalTicks).
  arrivalTicks = 0;
  /// Ticks with no raster (nothing changed / not on the beat).
  elided = 0;
  /// BURST: present ticks that found nothing newer (content slower than
  /// the target sub-rate — the honest miss, counted, never silent).
  missedPresentTicks = 0;

  /// The identity-stable decision record step() returns.
  readonly act: PresentDecision = {
    present: false,
    interp: false,
    alphaQ12: 0,
    coalesced: 0,
    presentSeq: 0,
    k: 1,
  };

  constructor(config: Partial<CadenceConfig> & { policy: CadencePolicyKind }) {
    this.cfg = { ...CADENCE_DEFAULTS, ...config };
    this.k = CADENCE_K_MIN;
  }

  get config(): Readonly<CadenceConfig> {
    return this.cfg;
  }

  /// One display tick. Pure function of the arrival trace + internal
  /// state; zero allocation (PC4). `latestSeq` is the newest seq observed
  /// at this tick (a fan-out reader's claim.seq, the cursor's latest) —
  /// monotonic by the ring contract; a regressed input is clamped to the
  /// high-water mark (defensive, declared — the transport never regresses).
  step(latestSeq: number): PresentDecision {
    this.ticks++;
    const a = this.act;
    a.present = false;
    a.interp = false;
    a.alphaQ12 = 0;
    a.coalesced = 0;
    a.k = this.k;

    switch (this.cfg.policy) {
      case CadencePolicyKind.LATEST_WINS: {
        if (latestSeq > this.lastPresentedSeq) {
          a.coalesced = latestSeq - this.lastPresentedSeq - 1;
          this.coalescedByDecision += a.coalesced;
          this.lastPresentedSeq = latestSeq;
          this.presents++;
          a.present = true;
          a.presentSeq = latestSeq;
        } else {
          this.elided++;
          a.presentSeq = this.lastPresentedSeq;
        }
        return a;
      }

      case CadencePolicyKind.PACED_INTERPOLATE: {
        if (latestSeq > this.newestSeq) {
          // Arrival: close the window, open the next at alpha=0.
          a.coalesced = latestSeq - this.newestSeq - 1;
          this.coalescedByDecision += a.coalesced;
          this.prevSeq = this.newestSeq;
          this.prevObsTick = this.newestObsTick;
          this.newestSeq = latestSeq;
          this.newestObsTick = this.ticks;
          this.arrivalTicks++;
          a.interp = true;
          a.alphaQ12 = 0;
          a.presentSeq = latestSeq;
        } else {
          // Interpolation window: advance alpha toward the newest frame.
          const period = Math.max(1, this.newestObsTick - this.prevObsTick);
          const dt = this.ticks - this.newestObsTick;
          a.interp = true;
          a.alphaQ12 = Math.min(
            CADENCE_ALPHA_ONE_Q12,
            Math.trunc((dt * CADENCE_ALPHA_ONE_Q12) / period)
          );
          a.presentSeq = this.newestSeq;
        }
        // Present iff the raster triple changed (the elision key).
        if (
          this.lastBaseSeq !== this.prevSeq ||
          this.lastTargetSeq !== this.newestSeq ||
          this.lastAlpha !== a.alphaQ12
        ) {
          this.lastBaseSeq = this.prevSeq;
          this.lastTargetSeq = this.newestSeq;
          this.lastAlpha = a.alphaQ12;
          this.presents++;
          // True blends only: a raster at a blend endpoint (alpha 0 or
          // saturated, or the pre-history window prev==0) shows a REAL
          // frame — synthesis is the strictly-between mixture, and only
          // that is counted as invention (Law 4).
          if (
            this.prevSeq !== 0 &&
            a.alphaQ12 > 0 &&
            a.alphaQ12 < CADENCE_ALPHA_ONE_Q12
          ) {
            this.interpFrames++;
          }
          a.present = true;
        } else {
          this.elided++;
        }
        return a;
      }

      case CadencePolicyKind.BURST_COALESCE: {
        // 1. Arrival detection (high-water clamp — the transport never
        //    regresses; a defensive input is absorbed, not believed).
        const seen = Math.max(this.lastSeenLatest, latestSeq);
        const arrivals = seen - this.lastSeenLatest; // >= 0 by construction
        this.lastSeenLatest = seen;
        // 2. Gap EWMA (Q12, weight 1/4): updated ONLY on arrival ticks —
        //    the inter-arrival gap is the content-cadence estimate, and a
        //    constant gap converges exactly (an arrivals-rate EMA would
        //    oscillate forever on periodic input — the honest estimator
        //    is the gap, not the rate). Non-negative split so every
        //    port's truncating division agrees bit-for-bit.
        if (arrivals > 0) {
          if (this.haveGap) {
            const gap = this.ticks - this.lastArrivalTick;
            const target = gap * EMA_ONE_Q12;
            const delta = target - this.ewmaGapQ12;
            if (delta >= 0) {
              this.ewmaGapQ12 += Math.trunc(delta / 4);
            } else {
              this.ewmaGapQ12 -= Math.trunc(-delta / 4);
            }
          } else {
            this.haveGap = true; // first arrival: no gap observed yet
          }
          this.lastArrivalTick = this.ticks;
        }
        // 3. Periodic reassess — the hysteresis: K changes at most once
        //    per reassessTicks window.
        this.ticksSinceAssess++;
        if (this.ticksSinceAssess >= this.cfg.reassessTicks) {
          this.ticksSinceAssess = 0;
          // round(ewmaGapQ12 / 4096) with integer arithmetic, clamped.
          let kNew = Math.trunc(
            (this.ewmaGapQ12 + EMA_ONE_Q12 / 2) / EMA_ONE_Q12
          );
          if (kNew < CADENCE_K_MIN) kNew = CADENCE_K_MIN;
          if (kNew > CADENCE_K_MAX) kNew = CADENCE_K_MAX;
          this.k = kNew;
          a.k = kNew;
        }
        // 4. The paced present: at most once per K ticks, newest only.
        this.tickInCycle++;
        if (this.tickInCycle >= this.k) {
          this.tickInCycle = 0;
          if (latestSeq > this.lastPresentedSeq) {
            a.coalesced = latestSeq - this.lastPresentedSeq - 1;
            this.coalescedByDecision += a.coalesced;
            this.lastPresentedSeq = latestSeq;
            this.presents++;
            a.present = true;
            a.presentSeq = latestSeq;
          } else {
            this.missedPresentTicks++;
            a.presentSeq = this.lastPresentedSeq;
          }
        } else {
          this.elided++;
          a.presentSeq = this.lastPresentedSeq;
        }
        return a;
      }
    }
  }

  /// Reset to a freshly-constructed state for `policy` (counters and
  /// window included). Used by PC6 switch tests and consumer rebuilds.
  reset(policy: CadencePolicyKind = this.cfg.policy): void {
    this.cfg.policy = policy;
    this.ticks = 0;
    this.lastPresentedSeq = 0;
    this.prevSeq = 0;
    this.prevObsTick = 0;
    this.newestSeq = 0;
    this.newestObsTick = 0;
    this.lastBaseSeq = -1;
    this.lastTargetSeq = -1;
    this.lastAlpha = -1;
    this.ewmaGapQ12 = 4096;
    this.haveGap = false;
    this.lastArrivalTick = 0;
    this.k = CADENCE_K_MIN;
    this.ticksSinceAssess = 0;
    this.tickInCycle = 0;
    this.lastSeenLatest = 0;
    this.presents = 0;
    this.coalescedByDecision = 0;
    this.interpFrames = 0;
    this.arrivalTicks = 0;
    this.elided = 0;
    this.missedPresentTicks = 0;
    this.act.present = false;
    this.act.interp = false;
    this.act.alphaQ12 = 0;
    this.act.coalesced = 0;
    this.act.presentSeq = 0;
    this.act.k = CADENCE_K_MIN;
  }
}
