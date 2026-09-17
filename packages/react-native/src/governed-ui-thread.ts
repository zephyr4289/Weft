// governed-ui-thread.ts — RFC-0009 Series 7: the governor ladder + cadence
// policies as React Native Reanimated WORKLETS on the UI thread.
//
// WHY EXISTS: the work order's "native worklet reader bindings" — on RN
// the display loop is Reanimated's UI-thread frame callback, and a JS-side
// governor means a bridge hop per frame (the jank the mobile hardening
// work exists to kill). The governor + policy are PURE INTEGER state
// machines — exactly what worklets CAN run: this module flattens them to
// plain-object state (no class instances — the 2026-09 honesty fix in
// weft-rn.ts removed a 'worklet' directive that captured a Weft class and
// would have failed at workletize time; Series 7 keeps that honesty and
// does it FOR REAL).
//
// ARCHITECTURE (dependency-injected, testable — the weft-rn.ts pattern):
//
//   JS thread:  the fan-out reader claims once per tick (it is a JS
//               object) and writes TWO SharedValues:
//                 latestSeqSV.value  = rec.seq      (the policy's input)
//                 framesBehindSV.value = rec.fresh ? rec.dropped : 0
//                                                  (the ladder's input)
//               (or a native event source writes them — the worklet does
//               not care where the numbers come from.)
//
//   UI thread:  tick() — the worklet — reads both SharedValues, steps the
//               flattened ladder + policy (arithmetic-identical to
//               @weft/core's FreshnessGovernor/CadencePolicy — pinned by
//               the parity test on the canonical xorshift32 trace), and
//               writes the decision into SharedValues:
//                 actionSV / skipNSV        (the ladder — advisory class)
//                 presentSV / interpSV / alphaQ12SV / coalescedSV /
//                 presentSeqSV / kSV        (the presentation decision)
//               Zero bridge hops per frame; the animated view reads the
//               decision SharedValues directly.
//
//   The caller registers tick() with Reanimated's useFrameCallback (the
//   registerFrameCallback DI parameter — the same seam weft-rn.ts uses).
//
// WORKLET DISTRIBUTION (the standard pattern for worklet libraries): the
// inner step functions carry 'worklet' directives; the consuming app's
// babel config must include this package's sources in Reanimated's
// `workletSources` (node_modules is excluded by default). When Reanimated
// is absent or the app does not opt in, createGovernedUiThread degrades
// HONESTLY to the JS-thread tick (the weft-rn.ts stance — stated, never
// implied).
//
// ARITHMETIC PARITY: the flattened machines mirror cadence.ts/governor.ts
// field-for-field (same PROTOCOL kind values, same Q12 ladder, same
// non-negative division split). governed-ui-thread.test.ts pins the
// packed decision trace on the canonical 04-LITMUS §0.2 xorshift32 trace
// against @weft/core — byte-level (the PC3 discipline, applied to the
// worklet flattening).

import {
  CadencePolicy,
  CadencePolicyKind,
  FreshnessGovernor,
} from '@weft/core';

/** SharedValue-shaped (structural — Reanimated SharedValue or a plain
 *  object in tests): the worklet reads `.value`, never imports reanimated. */
export interface SharedNum {
  value: number;
}

/** The flattened ladder state (plain object — worklet-capturable). */
export interface LadderState {
  fastPathBehind: number;
  skipBehind: number;
  snapshotBehind: number;
  reseedCooldownMs: number;
  lastReseedMs: number; // -1 = never
  decidedDrops: number;
  reseeds: number;
  steps: number;
}

/** The flattened policy state (plain object — worklet-capturable). */
export interface CadenceState {
  policy: number;
  reassessTicks: number;
  ticks: number;
  lastPresentedSeq: number;
  prevSeq: number;
  prevObsTick: number;
  newestSeq: number;
  newestObsTick: number;
  lastBaseSeq: number;
  lastTargetSeq: number;
  lastAlpha: number;
  ewmaGapQ12: number;
  haveGap: boolean;
  lastArrivalTick: number;
  k: number;
  ticksSinceAssess: number;
  tickInCycle: number;
  lastSeenLatest: number;
  presents: number;
  coalescedByDecision: number;
  interpFrames: number;
  arrivalTicks: number;
  elided: number;
  missedPresentTicks: number;
}

/** The per-tick decision record (mutated in place — the act pattern). */
export interface GovernedDecision {
  // ladder (advisory class)
  action: number; // 0 FastPath | 1 Skip | 2 Snapshot | 3 Reseed
  skipN: number;
  actionChanged: boolean;
  // policy (presentation)
  present: boolean;
  interp: boolean;
  alphaQ12: number;
  coalesced: number;
  presentSeq: number;
  k: number;
}

/** Fresh ladder state with RFC-0009's published defaults. */
export function createLadderState(overrides?: Partial<LadderState>): LadderState {
  return {
    fastPathBehind: 1,
    skipBehind: 4,
    snapshotBehind: 16,
    reseedCooldownMs: 250,
    lastReseedMs: -1,
    decidedDrops: 0,
    reseeds: 0,
    steps: 0,
    ...overrides,
  };
}

/** Fresh policy state for a PROTOCOL policy kind. */
export function createCadenceState(
  policy: number,
  reassessTicks = 8
): CadenceState {
  return {
    policy,
    reassessTicks,
    ticks: 0,
    lastPresentedSeq: 0,
    prevSeq: 0,
    prevObsTick: 0,
    newestSeq: 0,
    newestObsTick: 0,
    lastBaseSeq: -1,
    lastTargetSeq: -1,
    lastAlpha: -1,
    ewmaGapQ12: 4096,
    haveGap: false,
    lastArrivalTick: 0,
    k: 1,
    ticksSinceAssess: 0,
    tickInCycle: 0,
    lastSeenLatest: 0,
    presents: 0,
    coalescedByDecision: 0,
    interpFrames: 0,
    arrivalTicks: 0,
    elided: 0,
    missedPresentTicks: 0,
  };
}

/** Fresh decision record (mutated in place). */
export function createDecision(): GovernedDecision {
  return {
    action: 0,
    skipN: 0,
    actionChanged: false,
    present: false,
    interp: false,
    alphaQ12: 0,
    coalesced: 0,
    presentSeq: 0,
    k: 1,
  };
}

// ---------------------------------------------------------------------------
// The flattened machines. The 'worklet' directives below are inert
// comments without the Reanimated babel plugin — with it (and this package
// listed in workletSources), these run ON THE UI THREAD. The bodies are
// arithmetic-identical to @weft/core (the parity test pins it).
// ---------------------------------------------------------------------------

/** The ladder, flattened (arithmetic-identical to FreshnessGovernor.step). */
export function ladderStep(
  s: LadderState,
  framesBehind: number,
  nowMs: number,
  out: GovernedDecision,
  prevAction: number
): void {
  'worklet';
  s.steps++;
  const behind = framesBehind < 0 ? 0 : framesBehind;
  if (behind <= s.fastPathBehind) {
    out.action = 0;
    out.skipN = 0;
  } else if (behind <= s.skipBehind) {
    const n = behind - s.fastPathBehind;
    s.decidedDrops += n; // Law 4
    out.action = 1;
    out.skipN = n;
  } else if (behind <= s.snapshotBehind) {
    out.action = 2;
    out.skipN = 0;
  } else if (
    s.lastReseedMs === -1 ||
    nowMs - s.lastReseedMs >= s.reseedCooldownMs
  ) {
    s.lastReseedMs = nowMs;
    s.reseeds++;
    out.action = 3;
    out.skipN = 0;
  } else {
    out.action = 2; // suppressed Reseed degrades to Snapshot
    out.skipN = 0;
  }
  out.actionChanged = out.action !== prevAction;
}

/** The cadence policies, flattened (arithmetic-identical to
 *  CadencePolicy.step — closed set, PROTOCOL kind values). */
export function cadenceStep(
  s: CadenceState,
  latestSeq: number,
  out: GovernedDecision
): void {
  'worklet';
  s.ticks++;
  out.present = false;
  out.interp = false;
  out.alphaQ12 = 0;
  out.coalesced = 0;
  out.k = s.k;

  if (s.policy === CadencePolicyKind.LATEST_WINS) {
    if (latestSeq > s.lastPresentedSeq) {
      out.coalesced = latestSeq - s.lastPresentedSeq - 1;
      s.coalescedByDecision += out.coalesced;
      s.lastPresentedSeq = latestSeq;
      s.presents++;
      out.present = true;
      out.presentSeq = latestSeq;
    } else {
      s.elided++;
      out.presentSeq = s.lastPresentedSeq;
    }
    return;
  }

  if (s.policy === CadencePolicyKind.PACED_INTERPOLATE) {
    if (latestSeq > s.newestSeq) {
      out.coalesced = latestSeq - s.newestSeq - 1;
      s.coalescedByDecision += out.coalesced;
      s.prevSeq = s.newestSeq;
      s.prevObsTick = s.newestObsTick;
      s.newestSeq = latestSeq;
      s.newestObsTick = s.ticks;
      s.arrivalTicks++;
      out.interp = true;
      out.alphaQ12 = 0;
      out.presentSeq = latestSeq;
    } else {
      let period = s.newestObsTick - s.prevObsTick;
      if (period < 1) period = 1;
      const dt = s.ticks - s.newestObsTick;
      out.interp = true;
      let alpha = Math.trunc((dt * 4096) / period);
      if (alpha > 4096) alpha = 4096;
      out.alphaQ12 = alpha;
      out.presentSeq = s.newestSeq;
    }
    if (
      s.lastBaseSeq !== s.prevSeq ||
      s.lastTargetSeq !== s.newestSeq ||
      s.lastAlpha !== out.alphaQ12
    ) {
      s.lastBaseSeq = s.prevSeq;
      s.lastTargetSeq = s.newestSeq;
      s.lastAlpha = out.alphaQ12;
      s.presents++;
      if (
        s.prevSeq !== 0 &&
        out.alphaQ12 > 0 &&
        out.alphaQ12 < 4096
      ) {
        s.interpFrames++;
      }
      out.present = true;
    } else {
      s.elided++;
    }
    return;
  }

  if (s.policy === CadencePolicyKind.BURST_COALESCE) {
    let seen = s.lastSeenLatest;
    if (latestSeq > seen) seen = latestSeq;
    const arrivals = seen - s.lastSeenLatest;
    s.lastSeenLatest = seen;
    if (arrivals > 0) {
      if (s.haveGap) {
        const gap = s.ticks - s.lastArrivalTick;
        const target = gap * 4096;
        const delta = target - s.ewmaGapQ12;
        if (delta >= 0) {
          s.ewmaGapQ12 += Math.trunc(delta / 4);
        } else {
          s.ewmaGapQ12 -= Math.trunc(-delta / 4);
        }
      } else {
        s.haveGap = true;
      }
      s.lastArrivalTick = s.ticks;
    }
    s.ticksSinceAssess++;
    if (s.ticksSinceAssess >= s.reassessTicks) {
      s.ticksSinceAssess = 0;
      let kNew = Math.trunc((s.ewmaGapQ12 + 2048) / 4096);
      if (kNew < 1) kNew = 1;
      if (kNew > 64) kNew = 64;
      s.k = kNew;
      out.k = kNew;
    }
    s.tickInCycle++;
    if (s.tickInCycle >= s.k) {
      s.tickInCycle = 0;
      if (latestSeq > s.lastPresentedSeq) {
        out.coalesced = latestSeq - s.lastPresentedSeq - 1;
        s.coalescedByDecision += out.coalesced;
        s.lastPresentedSeq = latestSeq;
        s.presents++;
        out.present = true;
        out.presentSeq = latestSeq;
      } else {
        s.missedPresentTicks++;
        out.presentSeq = s.lastPresentedSeq;
      }
    } else {
      s.elided++;
      out.presentSeq = s.lastPresentedSeq;
    }
    return;
  }

  throw new Error(`unknown cadence policy kind: ${s.policy}`);
}

/**
 * The UI-thread governed tick: ladder + policy over SharedValues, decision
 * into SharedValues. Register the returned tick with Reanimated's
 * useFrameCallback (or any frame source); it is the worklet body.
 *
 * @param latestSeq SharedValue holding the newest observed seq (the JS
 *   reader or a native event source writes it).
 * @param framesBehind SharedValue holding this consumer's per-reader
 *   staleness (rec.fresh ? rec.dropped : 0 — RFC-0008's framesBehind).
 * @param nowMs SharedValue holding the monotonic clock the ladder sees
 *   (injected — determinism; the JS side or a native clock writes it).
 * @param out SharedValues to publish the decision into (all optional —
 *   provide only what the animated view consumes).
 */
export function createGovernedUiThread(opts: {
  policy: number;
  latestSeq: SharedNum;
  framesBehind: SharedNum;
  nowMs: SharedNum;
  reassessTicks?: number;
  ladder?: Partial<LadderState>;
  out?: {
    action?: SharedNum;
    skipN?: SharedNum;
    present?: SharedNum;
    interp?: SharedNum;
    alphaQ12?: SharedNum;
    coalesced?: SharedNum;
    presentSeq?: SharedNum;
    k?: SharedNum;
  };
}): () => void {
  const ladder = createLadderState(opts.ladder);
  const cadence = createCadenceState(opts.policy, opts.reassessTicks ?? 8);
  const decision = createDecision();
  let prevAction = 0;

  const tick = () => {
    'worklet';
    ladderStep(ladder, opts.framesBehind.value, opts.nowMs.value, decision, prevAction);
    prevAction = decision.action;
    cadenceStep(cadence, opts.latestSeq.value, decision);
    const o = opts.out;
    if (o) {
      if (o.action) o.action.value = decision.action;
      if (o.skipN) o.skipN.value = decision.skipN;
      if (o.present) o.present.value = decision.present ? 1 : 0;
      if (o.interp) o.interp.value = decision.interp ? 1 : 0;
      if (o.alphaQ12) o.alphaQ12.value = decision.alphaQ12;
      if (o.coalesced) o.coalesced.value = decision.coalesced;
      if (o.presentSeq) o.presentSeq.value = decision.presentSeq;
      if (o.k) o.k.value = decision.k;
    }
  };
  return tick;
}

// ---------------------------------------------------------------------------
// The parity oracle (test-facing): the same trace through @weft/core's
// reference classes — governed-ui-thread.test.ts byte-compares the worklet
// flattening against THIS. Also exported for anyone wanting the JS-thread
// fallback (the weft-rn.ts stance when Reanimated is absent).
// ---------------------------------------------------------------------------

export function referenceTickTrace(
  policy: number,
  seqs: number[],
  behinds: number[],
  nowMs: number[]
): number[] {
  const gov = new FreshnessGovernor();
  // The cast is deliberate: this oracle accepts ANY kind value so the
  // closed-set guard stays testable; CadencePolicy throws on unknowns.
  const pol = new CadencePolicy({ policy: policy as CadencePolicyKind });
  const packed: number[] = [];
  for (let i = 0; i < seqs.length; i++) {
    const a = gov.step(behinds[i], nowMs[i]);
    const d = pol.step(seqs[i]);
    packed.push(
      ((a.kind << 6) | Math.min(a.skipN, 63)) & 0xff,
      ((d.present ? 1 : 0) << 7) |
        ((d.interp ? 1 : 0) << 6) |
        (d.alphaQ12 >> 7),
      Math.min(d.coalesced, 255)
    );
  }
  return packed;
}
