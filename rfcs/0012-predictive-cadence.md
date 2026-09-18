---
RFC: 0012
Title: Predictive Cadence — clock-drift compensation & VRR alignment (PREDICTIVE_PACED)
Status: Draft
Authors: Engineer 3 (Mobile Runtimes, Cadence Governance & Quality Systems)
Created: 2026-09-18
Supersedes / Superseded-by: None
Requires: 0009 §cadence presentation policies (the three-policy base), 0008 (freshness telemetry)
---

# RFC 0012 — Predictive Cadence: clock-drift compensation & VRR alignment

## Summary

The fourth cadence policy. RFC-0009 §cadence declared its policy set
closed — "a fourth is a new RFC" — because each policy is a distinct
contract with Law 4. This RFC exercises exactly that escape hatch:
**PREDICTIVE_PACED**, a display-rate presenter whose alpha ramp is driven
by a **fractional Q16 phase accumulator** fed by a filtered gap estimate,
instead of PACED_INTERPOLATE's raw integer last-period. It exists to fix
one concrete, visible defect class and one estimator weakness:

1. **The pulldown judder.** PACED_INTERPOLATE's period is INTEGER ticks
   (`max(1, newestObsTick - prevObsTick)`). When the content period is
   not an integer number of display ticks — 30 Hz feed on a 144 Hz
   display (2.4 ticks), 60 Hz on 90 Hz (1.5), 24 fps film on 120 Hz
   ProMotion at a VRR step (4.8) — the integer window saturates early,
   holds, collapses to alpha 0 at the wrong phase, and the real arrival
   lands mid-window. The raster oscillates ramp/hold/collapse: the
   classic 3:2-pulldown judder, now at UI timescale.
2. **The unfiltered clock.** PACED re-times its entire window from the
   RAW last inter-arrival gap — one jittery gap (a GC pause in the
   producer, a radio burst) re-times the next window wholesale. There is
   no drift compensation at all: a stable-but-fractional beat is
   indistinguishable from noise.

PREDICTIVE_PACED replaces both with two integer filters and a phase
accumulator:

```
ticker ──tick + latestSeq──> PREDICTIVE_PACED ──{present? alphaQ12 coalesced}──> draw loop
                                   │
                                   ├─ gapQ16  : Q16 EWMA (gain 1/4) of inter-arrival gaps
                                   ├─ varQ16  : Q16 EWMA (gain 1/4) of |gap − gapQ16| (MAD)
                                   └─ phaseQ16: fractional window position, += 65536²/gapQ16 per tick
```

Everything RFC-0009 promised about cadence policies still holds, verbatim:
deterministic (ticks counted, never read), integer-only with non-negative
division operands (identical truncation in every port), zero allocation
(identity-stable decision record), at most one present per tick, never
extrapolating past the newest frame (saturated alpha = honest holding),
continuous at window boundaries by construction, and Law-4-exact
telescoping counters.

## Motivation

A 120 Hz or 240 Hz VRR/ProMotion display is a *fractional-ratio*
environment almost by definition. Content clocks (camera sensors, video
decoders, remote feeds, simulators) run on their own crystal: 30.00 Hz
content against a 144 Hz panel is 4.8 display ticks per frame, forever.
The three shipped policies all quantize the relationship to integers:

- `LATEST_WINS` — presents arrivals only; between arrivals the panel
  re-shows the newest frame at display rate. Correct, and judder-free,
  but it never synthesizes: motion runs at content rate, not display
  rate.
- `PACED_INTERPOLATE` — synthesizes at display rate (the right idea) but
  paces the ramp by the raw integer last-period. At 4.8 the integer
  window is alternately 5 and 4 ticks wide (or saturates at 4 and holds),
  and the synthesized alpha staircases against the true beat: the
  3:2 judder, precisely.
- `BURST_COALESCE` — intentionally sub-rate; not a display-rate
  presenter at all.

What is missing is the fourth thing every real resampler has: a
**fractional phase accumulator with a filtered rate estimate**. It is the
same mechanism audio resamplers and video pulldown engines have used for
decades, specified in Weft's discipline: integer Q16, no clocks, no
allocation, cross-port byte parity.

## Guide-level explanation

```ts
import { CadencePolicy, CadencePolicyKind } from '@weft/core';

// 30 Hz feed on a 144 Hz display (ticks = display vsyncs).
const p = new CadencePolicy({ policy: CadencePolicyKind.PREDICTIVE_PACED });
// per vsync:
const d = p.step(latestSeqFromFanoutReader());
if (d.present) draw(blend(prevRaster, newestRaster, d.alphaQ12 / 4096));
```

The alpha walk on a stable 2.4-tick beat (warmup done, `gapQ16 ≈ 2.4`):

```
tick:      1      2      3      4      5      6 ...
phase:   0.417  0.833  1.000  1.000  0.417  0.833      (arrival re-seats to 0)
alphaQ12: 1706   3413   4096   4096   1706   3413
```

— a smooth monotone ramp to saturation, a short honest hold on the
newest frame, re-seat at the real arrival. Compare PACED at integer
period 2 (forced): `2048, 4096, [collapse] 2048, 4096 ...` with the
arrival landing half a tick late — the collapse-to-zero at the wrong
phase is the judder. On integer beats (60 on 120 = 2.0), PREDICTIVE's
ladder is *tick-for-tick identical* to PACED's (PC8 below) — the
fractional machinery is a strict generalization, not a replacement.

## Reference-level specification

### State

Beyond the PACED two-frame window (`prevSeq/prevObsTick/newestSeq/
newestObsTick` + elision key), exactly:

| field      | init | meaning                                              |
|------------|------|------------------------------------------------------|
| `gapQ16`   | 0    | Q16.16 EWMA of inter-arrival gaps in display ticks    |
| `varQ16`   | 0    | Q16.16 EWMA of `|gap<<16 − gapQ16|` (mean abs dev)    |
| `haveGap`  | false| true after the 2nd arrival (first has no gap)         |
| `lastArrivalTick` | 0 | tick of the most recent arrival                   |
| `phaseQ16` | 0    | fractional window position, 0..65536 (saturated)      |
| `phaseRem` | 0    | division-remainder carry of the phase advance         |

Constants: `ONE_Q16 = 65536`; EWMA gain 1/4 (the BURST_COALESCE weight,
integer-exact via the non-negative split division); the reactive gate
threshold `varQ16 × 4 > gapQ16` (relative MAD above 1/4 = untrusted
clock); `alphaQ12 = phaseQ16 >> 4` (Q16 → Q12 is exact: 65536 >> 4 = 4096).

### step(latestSeq) — the closed transition

1. **Arrival** (`latestSeq > newestSeq`) — the window boundary:
   - If `haveGap`: `gap = ticks − lastArrivalTick`;
     `target = gap << 16`;
     `delta = target − gapQ16`;
     `gapQ16 += (delta >= 0) ? delta/4 : −((−delta)/4)` (split — every
     port truncates identically);
     `dev = |target − gapQ16|` (post-update);
     `d = dev − varQ16`;
     `varQ16 += (d >= 0) ? d/4 : −((−d)/4)`.
   - Else: `haveGap = true` (first arrival — no gap yet).
   - `lastArrivalTick = ticks`; `arrivalTicks++`; Law-4 coalesced count
     identical to PACED (`latestSeq − newestSeq − 1`).
   - Window advance: `prevSeq := newestSeq; prevObsTick := newestObsTick;
     newestSeq := latestSeq; newestObsTick := ticks`.
   - **Re-seat**: `phaseQ16 = 0`. The arrival tick presents the blend at
     `alphaQ12 = 0` — i.e. the COMPLETED previous target, the same
     continuity construction as PACED (the closed window's saturated
     raster equals the new window's alpha-0 raster).
2. **No arrival** — the presentation tick:
   - **Warmup** (no gap yet): behave exactly as PACED —
     `alphaQ12 = min(4096, (ticks − newestObsTick) × 4096 / max(1,
     newestObsTick − prevObsTick))`. Deterministic, documented, and it
     makes the first two arrivals of any stream port-identical to PACED.
   - **Predicted** (gap known):
     - `step16 = (ONE_Q16 × ONE_Q16) / gapQ16` (integer; numerator 2³²,
       Long-safe in every port);
     - `phaseQ16 = min(ONE_Q16, phaseQ16 + step16)` (saturating — the
       accumulator never extrapolates past the window, matching the
       never-past-newest law);
     - **Full-precision carry**: `phaseRem += (ONE_Q16 × ONE_Q16) %
       gapQ16`; while `phaseRem ≥ gapQ16`: `phaseQ16 = min(ONE_Q16,
       phaseQ16 + phaseRem / gapQ16)` and `phaseRem %= gapQ16`.
       Truncating the per-tick increment alone loses fractional bits and
       breaks PC8's integer equivalence (g=6, j=3 gives 2047 vs PACED's
       2048 — the carry is the fix); with it the accumulator tracks
       `j × ONE_Q16² / gapQ16` exactly — no floats, no 128-bit
       intermediates, two extra integers per policy.
     - **Reactive gate**: if `varQ16 × 4 > gapQ16`, this tick falls back
       to the PACED integer rule (above) and `reactiveTicks++` — a
       counted, declared degradation when the jitter filter says the
       clock is untrustworthy (bursts, hot-path GC in the producer,
       radio stalls). The gate is a pure per-tick function of filter
       state; it flips nothing. (It may also fire during the filter's
       own warmup transient — the first ~16 arrivals, while `varQ16`
       still carries the initial deviation — bounded, counted, and it
       degrades to exactly what PACED would have done anyway; on stable
       feeds it clears and stays clear, which PC7 asserts.)
     - Otherwise `alphaQ12 = phaseQ16 >> 4`.
   - Elision key identical to PACED: present iff `(prevSeq, newestSeq,
     alphaQ12)` changed from the last presented triple.

`interp` is true whenever the policy describes a blend (both branches);
`presentSeq` is `newestSeq` on non-arrival ticks, `latestSeq` on arrival
ticks; `k` is carried as 1 (no sub-rate divisor). Counters: `presents`,
`coalescedByDecision`, `interpFrames` (strictly-between blends only,
Law 4), `arrivalTicks`, `elided` — same telescoping identity as PACED
(`sum(coalesced) == newestSeq − arrivalTicks`), plus the new advisory
`reactiveTicks`.

### What is deliberately NOT here

- **No extrapolation.** Saturated phase HOLDS the newest frame; it never
  synthesizes beyond it (RFC-0009's honesty law — the hold is the honest
  statement "content is late").
- **No Kalman yet.** The 1/4-gain EWMA pair (gap + MAD) is the v1
  estimator: integer-exact, converges in ~16 arrivals, and its
  non-negative split division ports bit-identically. A full 1-D Kalman
  with integer Q16 covariance arithmetic is the natural v2 (the state
  layout leaves room; the reactive gate would consult predicted
  covariance instead of MAD) — deliberately out of scope here.
- **No display-clock re-write.** The policy counts the caller's ticks;
  VRR rate changes are absorbed by the EWMA within ~16 arrivals and
  masked meanwhile by the reactive gate.

### VRR / ProMotion alignment

The policy is refresh-agnostic BY CONSTRUCTION: the tick is whatever the
display gives (CADisplayLink / Choreographer / rAF). Locking to a beat of
`g` ticks requires no integer `g`: the accumulator's per-tick increment
is `1/g` in Q16, so any rational beat is tracked with zero cumulative
drift (PC7), and a VRR switch is just a new `g` entering the EWMA —
bounded re-lock latency `≤ reassessTicks × 16 arrivals` worst case,
typically ≤ 8 ticks after the gate clears.

### Conformance gates (PC7–PC11, extending RFC-0009 §cadence PC1–PC6)

7. **PC7 drift-freedom**: on a stable `12/5` beat (2.4 ticks), after
   warmup (a) the gap EWMA converges into a ±0.125-tick band around
   2.4×65536 and stays bounded (no walk — 10k ticks; the band is the
   filter's measured steady-state orbit, gain 1/4 over the {2,3} gap
   cycle), and (b) the accumulator predicts the beat within the orbit:
   **≤ 10 saturated-hold presents per 10k ticks** (≤ 0.25% of windows —
   the orbit wobble crossing a tick early; on exact-integer beats the
   count is zero), so the raster holds stale content at most for a
   rounding sliver, never systematically. With crafted late arrivals
   (gap 4 into a 2-tick beat, every 25th), **every late window contains
   exactly one saturated hold — and no non-late window does** (the
   honest hold, shown exactly when content is late, never spuriously;
   whichever rule serves the tick — the accumulator or the gated
   integer rule — saturates exactly once inside a late window, and the
   elision key collapses the rest). (10k-tick battery, all ports.)
8. **PC8 integer equivalence**: on any stable integer beat `g ≥ 1` with
   zero jitter, the post-warmup `(present, interp, alphaQ12)` ladder is
   tick-for-tick identical to PACED_INTERPOLATE fed the same trace.
9. **PC9 monotone window (the judder gate)**: on any stable
   non-integer beat, within each inter-arrival window the presented
   alpha sequence is non-decreasing (no saturate→collapse sawtooth
   except at the true window boundary — the arrival re-seat).
10. **PC10 reactive fallback**: on a crafted high-MAD feed (alternating
    gaps 1, 5, 1, 5, ...), the gate fires (`reactiveTicks > 0`) and
    during gated ticks the alpha equals PACED's integer rule for the
    same window.
11. **PC11 parity**: identical packed decision traces for identical
    arrival traces across TS/Kotlin/Swift/Dart — the PC3 protocol
    extended in place (policy stream order 0,1,2,3; the v2 fixture
    `fixtures/xlang-cadence/` byte-compares all four, and
    `scripts/gen_trace_refs.mjs` pins the FNV-1a-64 references).

PC4 (zero allocation) re-pins over the fourth policy: the JVM battery's
ThreadMXBean audit covers `PREDICTIVE_PACED` step loops at zero bytes.

## Rationale

- **Why a phase accumulator and not a predicted-arrival timestamp?**
  A timestamp invites clock reads and extrapolation past newest. The
  accumulator is a pure integer state machine: it cannot read a clock,
  cannot allocate, and its saturation IS the hold-at-newest law.
- **Why MAD and not variance?** Variance needs squaring (Q32 intermediates,
  overflow discipline per port) and its square root for the band. MAD is
  one absolute value, linear-time, same EWMA gain, and its relative form
  (`MAD/gap`) is exactly the "is this clock trustworthy" quantity.
- **Why keep the PACED warmup and fallback instead of a separate
  policy object?** One policy kind means consumers switch presentations
  without new plumbing (PC6 switch-safety covers it), and the fallback
  is the documented degradation path for pathological clocks — measured
  via `reactiveTicks`, never silent.
- **Why extend the PC3 packing in place?** The v1 packing is
  deliberately lossy-but-sufficient; adding policy kind 3 to the stream
  order extends every log deterministically. The FNV reference pins are
  regenerated once, in the same commit as the four emitters, and the
  byte-compare is the arbiter — no version negotiation anywhere.

## Prior art

Audio resamplers (fractional phase + rate EWMA), 3:2 pulldown cadence
engines, ProMotion's own scheduler, and every network jitter buffer
(MAD/ EWMA-based playout delay). The novelty here is only the discipline:
integer-only, zero-alloc, cross-port byte parity, Law-4-exact counters —
Weft's Four Laws applied to a 40-year-old idea.
