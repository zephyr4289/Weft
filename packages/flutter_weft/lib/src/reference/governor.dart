// governor.dart — RFC-0009: FreshnessGovernor ladder + §cadence
// presentation policies, Dart driver layer (Series 7).
//
// WHY EXISTS: RFC-0009 shipped the governor (the staleness ladder) to C,
// Rust, and TS (PR #9, the nanoseconds work order) — but the VM ports had
// no equivalent: Flutter apps consuming a fan-out ring or a Triad
// hand-rolled the skip/snapshot/rebuild ladder per screen, tuned by
// vibes, untestable. This module brings BOTH halves of RFC-0009 to Dart
// with arithmetic-identical behavior to the TS reference and the
// Kotlin/Swift twins (docs/PORTS.md §9):
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
// internally, no clock reads), all arithmetic is int (64-bit on the VM)
// with non-negative division operands (`~/` truncates identically to
// every port). The same trace MUST produce the identical packed
// action/decision log in TS/Kotlin/Swift/Dart — proven by
// fixtures/xlang-governor/vm (G5) and fixtures/xlang-cadence (PC3), and
// pinned locally by the FNV-1a-64 trace hashes in governor_test.dart
// (ladder 0x3c33156204c7cfdf, cadence 0x6f654c298cbcc9f4).
//
// LAW 2 (G4/PC4): step() allocates NOTHING — state is ints/bools; the
// returned action/decision is the object's OWN identity-stable record,
// mutated in place (the FanoutClaim pattern). Read it synchronously; do
// not retain it across steps. Dart's proof is the identical() audit plus
// the pinned traces (no portable allocation counter — declared, per the
// repo's per-port honesty culture; the JVM allocated-bytes audit is the
// Kotlin leg's proof).
//
// LAW 4: decidedDrops/coalescedByDecision count frames dropped BY DECISION;
// interpFrames counts synthesized (strictly-between blend) presents —
// invention is a decision too. Exact telescoping identities (PC2):
//   LATEST_WINS / BURST_COALESCE: sum(coalesced) == lastPresentedSeq - presents
//   PACED_INTERPOLATE:            sum(coalesced) == newestSeq - arrivalTicks
//
// SINGLE-ISOLATE: like the kernel it reads, the governor is pure logic —
// no isolates, no atomics needed; identical behavior in any isolate.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION (battery green on
// the Dart VM via the plain-dart expect shim; flutter-packages CI is the
// flutter_test cover).

// ---------------------------------------------------------------------------
// The ladder — RFC-0009 core (arithmetic-identical to governor.ts /
// governor.c / governor.rs / Governor.kt / Governor.swift).
// ---------------------------------------------------------------------------

/// The closed action set. Numeric values are PROTOCOL (G5; do not renumber).
abstract final class GovernorActionKind {
  static const int fastPath = 0;
  static const int skip = 1;
  static const int snapshot = 2;
  static const int reseed = 3;
}

/// Identity-stable action record — mutated in place by step(), never
/// allocated per call (G4). skipN is valid only for kind == skip.
class GovernorAction {
  int kind;
  int skipN;
  GovernorAction(this.kind, this.skipN);
}

/// Ladder thresholds + cooldown, with RFC-0009's published defaults.
class GovernorConfig {
  final int fastPathBehind;
  final int skipBehind;
  final int snapshotBehind;
  final int reseedCooldownMs;
  const GovernorConfig({
    this.fastPathBehind = 1,
    this.skipBehind = 4,
    this.snapshotBehind = 16,
    this.reseedCooldownMs = 250,
  });
}

/// RFC-0009 published defaults (the TS GOVERNOR_DEFAULTS / C macros).
const GovernorConfig governorDefaults = GovernorConfig();

const int _neverReseeded = -1;

/// The staleness ladder. One step per consumer observation; the input is
/// ANY monotonic per-consumer staleness count (FrameCursor's framesBehind,
/// or a fan-out reader's claim.dropped — same semantics per PORTS.md §7).
/// Composed by the app; the governor never touches a Weft, a ring, or a
/// Triad (RFC-0009's "advisory only" lean).
class FreshnessGovernor {
  /// The ladder + cooldown. The ladder SHAPE is the spec (RFC-0009).
  final GovernorConfig cfg;
  int _lastReseedMs = _neverReseeded;

  /// Law 4: frames dropped BY DECISION (Skip(n) intermediates), distinct
  /// from any ring-level drop counter.
  int decidedDrops = 0;
  /// Emitted Reseeds (post-cooldown only).
  int reseeds = 0;
  /// Total step() calls (advisory).
  int steps = 0;

  /// The identity-stable action record step() returns.
  final GovernorAction act = GovernorAction(GovernorActionKind.fastPath, 0);

  FreshnessGovernor({GovernorConfig config = governorDefaults})
      : cfg = config;

  /// One decision. Pure except the Reseed cooldown bookkeeping; zero
  /// allocation (G4). `nowMs` is caller-injected monotonic milliseconds —
  /// the same (behind, nowMs) trace yields the same actions in every
  /// port (G5).
  GovernorAction step(int framesBehind, int nowMs) {
    steps++;
    final behind = framesBehind < 0 ? 0 : framesBehind;
    final a = act;
    if (behind <= cfg.fastPathBehind) {
      a.kind = GovernorActionKind.fastPath;
      a.skipN = 0;
    } else if (behind <= cfg.skipBehind) {
      final n = behind - cfg.fastPathBehind;
      decidedDrops += n; // Law 4: decided drops are decisions
      a.kind = GovernorActionKind.skip;
      a.skipN = n;
    } else if (behind <= cfg.snapshotBehind) {
      a.kind = GovernorActionKind.snapshot;
      a.skipN = 0;
    } else {
      // behind > snapshotBehind: Reseed, rate-limited by the cooldown.
      if (_lastReseedMs == _neverReseeded ||
          nowMs - _lastReseedMs >= cfg.reseedCooldownMs) {
        _lastReseedMs = nowMs;
        reseeds++;
        a.kind = GovernorActionKind.reseed;
        a.skipN = 0;
      } else {
        // Rate-limited: degrade to the best non-rebuild action.
        a.kind = GovernorActionKind.snapshot;
        a.skipN = 0;
      }
    }
    return a;
  }

  /// Reset the cooldown state (a rebuilt consumer starts fresh).
  void reset() {
    _lastReseedMs = _neverReseeded;
    decidedDrops = 0;
    reseeds = 0;
    steps = 0;
    act.kind = GovernorActionKind.fastPath;
    act.skipN = 0;
  }
}

// ---------------------------------------------------------------------------
// The cadence policies — RFC-0009 §Cadence presentation policies (Series 7;
// arithmetic-identical to packages/core/src/cadence.ts, Governor.kt,
// Governor.swift).
// ---------------------------------------------------------------------------

/// The closed policy set. Numeric values are PROTOCOL (PC3; do not renumber).
abstract final class CadencePolicyKind {
  static const int latestWins = 0;
  static const int pacedInterpolate = 1;
  static const int burstCoalesce = 2;
  /// RFC-0012: the fractional-ratio presenter (Q16 phase accumulator).
  static const int predictivePaced = 3;
}

/// Identity-stable decision record — mutated in place by step(), never
/// allocated per call (PC4). interp/alphaQ12 describe the CURRENT raster
/// state (meaningful even when present=false); PACED only.
class PresentDecision {
  /// Raster this tick (the app's one draw call).
  bool present;
  /// The current raster state — blend(prev, newest, alphaQ12/4096).
  bool interp;
  /// Blend weight in Q12 (0..4096).
  int alphaQ12;
  /// Frames coalesced BY DECISION this tick (Law 4).
  int coalesced;
  /// Seq the presented raster derives from.
  int presentSeq;
  /// BURST_COALESCE only: the current pacing divisor (advisory HUD).
  int k;
  PresentDecision({
    this.present = false,
    this.interp = false,
    this.alphaQ12 = 0,
    this.coalesced = 0,
    this.presentSeq = 0,
    this.k = 1,
  });
}

/// Policy configuration with the RFC-0009 §cadence published defaults.
class CadenceConfig {
  /// The closed policy set member (PROTOCOL kind value).
  final int policy;
  /// BURST_COALESCE: reassess cadence for K, in ticks (the hysteresis —
  /// K changes at most once per window). Default 8.
  final int reassessTicks;
  const CadenceConfig(this.policy, {this.reassessTicks = 8});
}

/// Q12 one (the saturated blend).
const int cadenceAlphaOneQ12 = 4096;

/// BURST_COALESCE bounds for K (present every K ticks; 64 caps stall
/// recovery — with ewma==0 K parks at the cap, presents stop until
/// content resumes, and the reassess window bounds re-lock latency).
const int cadenceKMin = 1;
const int cadenceKMax = 64;

const int _emaOneQ12 = 4096;

// --- PREDICTIVE_PACED (RFC-0012) constants ---
/// Q16 one — the fractional window's saturation point; Q16 -> Q12 is the
/// exact shift >> 4 (65536 >> 4 = 4096).
const int cadenceOneQ16 = 65536;

const int _reactiveNum = 1;
const int _reactiveDen = 4;

/// One cadence policy instance. One step() per DISPLAY TICK; `latestSeq`
/// is the newest seq observed at this tick (a fan-out reader's claim.seq,
/// the cursor's latest) — monotonic by the ring contract; a regressed
/// input is clamped to the high-water mark (defensive, declared).
class CadencePolicy {
  CadenceConfig _cfg;
  /// The active configuration (PC6 switches the policy in place).
  CadenceConfig get cfg => _cfg;
  /// Total step() calls — the tick counter (the policy's only clock).
  int _ticks = 0;

  // --- shared state ---
  int _lastPresentedSeq = 0;

  // --- PACED_INTERPOLATE state (the two-frame window) ---
  int _prevSeq = 0;
  int _prevObsTick = 0;
  int _newestSeq = 0;
  int _newestObsTick = 0;
  /// Last PRESENTED (base, target, alpha) triple — the elision key.
  int _lastBaseSeq = -1;
  int _lastTargetSeq = -1;
  int _lastAlpha = -1;

  // --- BURST_COALESCE state ---
  /// Q12 EWMA of inter-arrival gaps (ticks). Init 4096 = gap 1 (the
  /// conservative default: present whenever content is new).
  int _ewmaGapQ12 = 4096;
  bool _haveGap = false;
  int _lastArrivalTick = 0;
  int _k = cadenceKMin;
  int _ticksSinceAssess = 0;
  int _tickInCycle = 0;
  int _lastSeenLatest = 0;

  // --- PREDICTIVE_PACED state (RFC-0012; independent of BURST's filter
  //     — different scale Q16, different init, different update order) ---
  /// Q16 EWMA of inter-arrival gaps (ticks). 0 = unknown (warmup).
  int _gapQ16 = 0;
  /// Q16 EWMA of |gap<<16 - gapQ16| — the mean absolute deviation.
  int _varQ16 = 0;
  /// True after the SECOND arrival (the first has no gap to observe).
  bool _predHaveGap = false;
  /// Tick of the most recent arrival (the gap's anchor).
  int _predLastArrivalTick = 0;
  /// Fractional window position, 0..cadenceOneQ16 (saturating — the
  /// never-past-newest law). One Q16 unit = 1/65536 of a window.
  int _phaseQ16 = 0;
  /// The phase advance's division-remainder carry (0..gapQ16-1) — full
  /// precision without floats or 128-bit intermediates (RFC-0012 §step).
  int _phaseRem = 0;

  // --- counters (advisory, AXIOM T; exact per PC2) ---
  /// Presents issued (real + interpolated).
  int presents = 0;
  /// Frames coalesced BY DECISION (Law 4).
  int coalescedByDecision = 0;
  /// Synthesized (strictly-between blend) presents — invention counted.
  int interpFrames = 0;
  /// PACED: ticks on which a new seq arrived (PC2's arrivalTicks).
  int arrivalTicks = 0;
  /// Ticks with no raster (nothing changed / not on the beat).
  int elided = 0;
  /// BURST: present ticks that found nothing newer (counted, never silent).
  int missedPresentTicks = 0;
  /// PREDICTIVE: ticks the reactive gate fired (advisory, AXIOM T — the
  /// declared, counted degradation to PACED's integer rule).
  int reactiveTicks = 0;

  /// The identity-stable decision record step() returns.
  final PresentDecision act = PresentDecision();

  CadencePolicy(CadenceConfig config) : _cfg = config;

  /// Convenience: default reassess cadence.
  CadencePolicy.kind(int policy) : this(CadenceConfig(policy));

  /// One display tick. Pure function of the arrival trace + internal
  /// state; zero allocation (PC4).
  PresentDecision step(int latestSeq) {
    _ticks++;
    final a = act;
    a.present = false;
    a.interp = false;
    a.alphaQ12 = 0;
    a.coalesced = 0;
    a.k = _k;

    switch (_cfg.policy) {
      case CadencePolicyKind.latestWins:
        if (latestSeq > _lastPresentedSeq) {
          a.coalesced = latestSeq - _lastPresentedSeq - 1;
          coalescedByDecision += a.coalesced;
          _lastPresentedSeq = latestSeq;
          presents++;
          a.present = true;
          a.presentSeq = latestSeq;
        } else {
          elided++;
          a.presentSeq = _lastPresentedSeq;
        }
        return a;

      case CadencePolicyKind.pacedInterpolate:
        if (latestSeq > _newestSeq) {
          // Arrival: close the window, open the next at alpha=0.
          a.coalesced = latestSeq - _newestSeq - 1;
          coalescedByDecision += a.coalesced;
          _prevSeq = _newestSeq;
          _prevObsTick = _newestObsTick;
          _newestSeq = latestSeq;
          _newestObsTick = _ticks;
          arrivalTicks++;
          a.interp = true;
          a.alphaQ12 = 0;
          a.presentSeq = latestSeq;
        } else {
          // Interpolation window: advance alpha toward the newest frame.
          var period = _newestObsTick - _prevObsTick;
          if (period < 1) period = 1;
          final dt = _ticks - _newestObsTick;
          a.interp = true;
          var alpha = (dt * cadenceAlphaOneQ12) ~/ period;
          if (alpha > cadenceAlphaOneQ12) alpha = cadenceAlphaOneQ12;
          a.alphaQ12 = alpha;
          a.presentSeq = _newestSeq;
        }
        // Present iff the raster triple changed (the elision key).
        if (_lastBaseSeq != _prevSeq ||
            _lastTargetSeq != _newestSeq ||
            _lastAlpha != a.alphaQ12) {
          _lastBaseSeq = _prevSeq;
          _lastTargetSeq = _newestSeq;
          _lastAlpha = a.alphaQ12;
          presents++;
          // True blends only: a raster at a blend endpoint (alpha 0 or
          // saturated, or the pre-history window prev==0) shows a REAL
          // frame — synthesis is the strictly-between mixture, and only
          // that is counted as invention (Law 4).
          if (_prevSeq != 0 && a.alphaQ12 > 0 && a.alphaQ12 < cadenceAlphaOneQ12) {
            interpFrames++;
          }
          a.present = true;
        } else {
          elided++;
        }
        return a;

      case CadencePolicyKind.predictivePaced:
        if (latestSeq > _newestSeq) {
          // ARRIVAL — the window boundary. Bookkeeping identical to PACED
          // (coalesced count, window advance, alpha-0 continuity); the
          // filters update in the RFC-0012 declared order: gapQ16 first,
          // then dev against the UPDATED gapQ16, then varQ16 — every
          // operand non-negative, every division split.
          a.coalesced = latestSeq - _newestSeq - 1;
          coalescedByDecision += a.coalesced;
          if (_predHaveGap) {
            final gap = _ticks - _predLastArrivalTick;
            final target = gap * cadenceOneQ16;
            final delta = target - _gapQ16;
            if (delta >= 0) {
              _gapQ16 += delta ~/ _reactiveDen;
            } else {
              _gapQ16 -= (-delta) ~/ _reactiveDen;
            }
            var dev = target - _gapQ16;
            if (dev < 0) dev = -dev;
            final d = dev - _varQ16;
            if (d >= 0) {
              _varQ16 += d ~/ _reactiveDen;
            } else {
              _varQ16 -= (-d) ~/ _reactiveDen;
            }
          } else {
            _predHaveGap = true; // first arrival: no gap observed yet
          }
          _predLastArrivalTick = _ticks;
          arrivalTicks++;
          _prevSeq = _newestSeq;
          _prevObsTick = _newestObsTick;
          _newestSeq = latestSeq;
          _newestObsTick = _ticks;
          _phaseQ16 = 0;
          a.interp = true;
          a.alphaQ12 = 0;
          a.presentSeq = latestSeq;
        } else {
          // NO ARRIVAL — the presentation tick.
          // Reactive gate: relative MAD above 1/4 = untrusted clock —
          // degrade this tick to PACED's integer rule, counted.
          final reactive = _predHaveGap &&
              _varQ16 * _reactiveDen > _gapQ16 * _reactiveNum;
          if (!_predHaveGap || _gapQ16 <= 0 || reactive) {
            // Warmup (no gap KNOWN) or gated: PACED's integer window rule
            // verbatim — the documented path. (The gate may also fire
            // during the filter's own warmup transient — bounded, counted,
            // and it degrades to exactly what PACED would have done
            // anyway.)
            var period = _newestObsTick - _prevObsTick;
            if (period < 1) period = 1;
            final dt = _ticks - _newestObsTick;
            var alpha = (dt * cadenceAlphaOneQ12) ~/ period;
            if (alpha > cadenceAlphaOneQ12) alpha = cadenceAlphaOneQ12;
            a.alphaQ12 = alpha;
            if (reactive) reactiveTicks++;
          } else {
            // The phase accumulator: advance by 1/gapQ16 of a window per
            // tick in Q16, at FULL precision — quotient plus remainder
            // carry (two integers, no allocation, no drift).
            final num = cadenceOneQ16 * cadenceOneQ16;
            final step16 = num ~/ _gapQ16;
            _phaseQ16 += step16;
            if (_phaseQ16 > cadenceOneQ16) _phaseQ16 = cadenceOneQ16;
            _phaseRem += num % _gapQ16;
            if (_phaseRem >= _gapQ16) {
              final carry = _phaseRem ~/ _gapQ16;
              _phaseRem -= carry * _gapQ16;
              _phaseQ16 += carry;
              if (_phaseQ16 > cadenceOneQ16) _phaseQ16 = cadenceOneQ16;
            }
            a.alphaQ12 = _phaseQ16 >> 4; // Q16 -> Q12, exact
          }
          a.interp = true;
          a.presentSeq = _newestSeq;
        }
        // Present iff the raster triple changed (the elision key —
        // identical to PACED).
        if (_lastBaseSeq != _prevSeq ||
            _lastTargetSeq != _newestSeq ||
            _lastAlpha != a.alphaQ12) {
          _lastBaseSeq = _prevSeq;
          _lastTargetSeq = _newestSeq;
          _lastAlpha = a.alphaQ12;
          presents++;
          if (_prevSeq != 0 && a.alphaQ12 > 0 && a.alphaQ12 < cadenceAlphaOneQ12) {
            interpFrames++;
          }
          a.present = true;
        } else {
          elided++;
        }
        return a;

      case CadencePolicyKind.burstCoalesce:
        // 1. Arrival detection (high-water clamp — the transport never
        //    regresses; a defensive input is absorbed, not believed).
        var seen = _lastSeenLatest;
        if (latestSeq > seen) seen = latestSeq;
        final arrivals = seen - _lastSeenLatest; // >= 0 by construction
        _lastSeenLatest = seen;
        // 2. Gap EWMA (Q12, weight 1/4): updated ONLY on arrival ticks —
        //    the inter-arrival gap is the content-cadence estimate, and a
        //    constant gap converges exactly (an arrivals-rate EMA would
        //    oscillate forever on periodic input — the honest estimator
        //    is the gap, not the rate). Non-negative split so every
        //    port's truncating division agrees bit-for-bit.
        if (arrivals > 0) {
          if (_haveGap) {
            final gap = _ticks - _lastArrivalTick;
            final target = gap * _emaOneQ12;
            final delta = target - _ewmaGapQ12;
            if (delta >= 0) {
              _ewmaGapQ12 += delta ~/ 4;
            } else {
              _ewmaGapQ12 -= (-delta) ~/ 4;
            }
          } else {
            _haveGap = true; // first arrival: no gap observed yet
          }
          _lastArrivalTick = _ticks;
        }
        // 3. Periodic reassess — the hysteresis: K changes at most once
        //    per reassessTicks window.
        _ticksSinceAssess++;
        if (_ticksSinceAssess >= _cfg.reassessTicks) {
          _ticksSinceAssess = 0;
          // round(ewmaGapQ12 / 4096), clamped.
          var kNew = (_ewmaGapQ12 + _emaOneQ12 ~/ 2) ~/ _emaOneQ12;
          if (kNew < cadenceKMin) kNew = cadenceKMin;
          if (kNew > cadenceKMax) kNew = cadenceKMax;
          _k = kNew;
          a.k = kNew;
        }
        // 4. The paced present: at most once per K ticks, newest only.
        _tickInCycle++;
        if (_tickInCycle >= _k) {
          _tickInCycle = 0;
          if (latestSeq > _lastPresentedSeq) {
            a.coalesced = latestSeq - _lastPresentedSeq - 1;
            coalescedByDecision += a.coalesced;
            _lastPresentedSeq = latestSeq;
            presents++;
            a.present = true;
            a.presentSeq = latestSeq;
          } else {
            missedPresentTicks++;
            a.presentSeq = _lastPresentedSeq;
          }
        } else {
          elided++;
          a.presentSeq = _lastPresentedSeq;
        }
        return a;

      default:
        // Closed set — an unknown kind is a programming error, not a
        // runtime path (the battery pins the three kinds).
        throw StateError('unknown cadence policy kind: ${_cfg.policy}');
    }
  }

  /// Test-only observability for the PC2 telescoping identities: the
  /// public counters plus these two endpoints close the equations.
  /// (@visibleForTesting semantics without the flutter import.)
  int lastPresentedSeqForTest() => _lastPresentedSeq;
  int newestSeqForTest() => _newestSeq;
  // RFC-0012 PC7/PC9 observability (the ports mirror the accessors).
  int gapQ16ForTest() => _gapQ16;
  int varQ16ForTest() => _varQ16;
  int phaseQ16ForTest() => _phaseQ16;
  int phaseRemForTest() => _phaseRem;

  /// Reset to a freshly-constructed state for `policy` (counters and
  /// window included). Used by PC6 switch tests and consumer rebuilds.
  void reset([int? policy]) {
    _cfg = CadenceConfig(policy ?? _cfg.policy,
        reassessTicks: _cfg.reassessTicks);
    _ticks = 0;
    _lastPresentedSeq = 0;
    _prevSeq = 0;
    _prevObsTick = 0;
    _newestSeq = 0;
    _newestObsTick = 0;
    _lastBaseSeq = -1;
    _lastTargetSeq = -1;
    _lastAlpha = -1;
    _ewmaGapQ12 = 4096;
    _haveGap = false;
    _lastArrivalTick = 0;
    _k = cadenceKMin;
    _ticksSinceAssess = 0;
    _tickInCycle = 0;
    _lastSeenLatest = 0;
    _gapQ16 = 0;
    _varQ16 = 0;
    _predHaveGap = false;
    _predLastArrivalTick = 0;
    _phaseQ16 = 0;
    _phaseRem = 0;
    reactiveTicks = 0;
    presents = 0;
    coalescedByDecision = 0;
    interpFrames = 0;
    arrivalTicks = 0;
    elided = 0;
    missedPresentTicks = 0;
    act.present = false;
    act.interp = false;
    act.alphaQ12 = 0;
    act.coalesced = 0;
    act.presentSeq = 0;
    act.k = cadenceKMin;
  }
}
