---
RFC: 0009
Title: The Freshness Governor — adaptive consumer strategy from framesBehind
Status: Draft
Authors: Arch Breakthrough Contributor
Created: 2026-09-16
Supersedes / Superseded-by: None
Requires: 0008 (freshness telemetry), 0004 (fan-out, for the fan-out case)
---

# RFC 0009 — The Freshness Governor

## Summary

A driver-layer controller that turns RFC 0008's `framesBehind` telemetry
into **automatic consumer-side decisions**: throttle, upgrade, or
drop-to-snapshot. The governor closes the control loop the freshness work
opened: FrameCursor *measures* staleness; the governor *acts* on it, per
consumer, without touching the kernel.

## Motivation

RFC 0008 answered the §10 question ("how stale is this consumer?") with a
counter. But a measurement nobody acts on is just guilt. Every real
consumer today hand-rolls the same ladder: if I'm behind, skip drawing the
minimap; if I'm very behind, re-snapshot instead of replaying. That ladder
is (a) duplicated per app, (b) tuned by vibes, (c) untestable, because it
lives inside app code where litmus cannot reach it.

The governor makes the ladder a spec'd, testable driver-layer object:

```
consumer ──framesBehind──> GOVERNOR ──{FastPath | Skip | Snapshot | Reseed}──> consumer
```

## Guide-level explanation

```ts
const gov = new FreshnessGovernor({
  fastPathBehind: 1,     // behind <= 1: draw every frame (flagship canvas)
  skipBehind: 4,         // behind <= 4: draw latest, skip intermediates
  snapshotBehind: 16,    // behind <= 16: draw once, then re-sync
  // behind > snapshotBehind: consumer is effectively dead — the governor
  // emits Reseed and the app rebuilds the consumer (e.g. re-open the heddle)
});
// per frame:
const action = gov.step(cursor.framesBehind());
switch (action.kind) { /* FastPath | Skip(n) | Snapshot | Reseed */ }
```

C/Rust map the same ladder onto `weft_frame_cursor` /
`FrameCursor` (RFC 0008 native). The four actions are closed — adding a
fifth is a new RFC, because each action is a different contract with
Law 4 (drops are free, but *decided* drops must be counted as decisions,
not accidents).

## Reference-level specification

- **Inputs**: `framesBehind` (u32, from FrameCursor), monotonic.
- **Outputs** (closed set):
  - `FastPath` — behind ≤ `fastPathBehind`. Consumer draws live.
  - `Skip(n)` — `fastPathBehind < behind ≤ skipBehind`. Draw newest only;
    `n = behind - fastPathBehind` intermediates are dropped **by decision**
    (counted via the governor's own counter, distinct from the ring's).
  - `Snapshot` — `skipBehind < behind ≤ snapshotBehind`. Render one frame
    from a fresh claim, then jump the consumer's expectations to `latest`.
  - `Reseed` — behind > `snapshotBehind`. The consumer cannot recover by
    skipping; rebuild it. The governor rate-limits Reseed (one per
    `reseedCooldownMs`, default 250) to prevent flap.
- **Hysteresis**: transitions use the *same* thresholds on the way down as
  on the way up (the ladder is stateless per step — flap is impossible by
  construction because actions depend only on the current `behind`, and
  Reseed is the only stateful action, gated by cooldown).
- **Zero allocation**: all state is two u32s and a timestamp. Litmus-grade:
  `step()` is pure except the Reseed cooldown.

## V-series-style conformance (proposed gates, G-series)

1. **G1 ladder**: every `behind` in 0..64 maps to the documented action.
2. **G2 monotone**: larger `behind` never yields a *fresher-class* action.
3. **G3 reseed flap**: 10k random `behind` spikes with cooldown — at most
   `ceil(10k / cooldown)` Reseeds emitted.
4. **G4 zero-alloc**: step() allocates nothing (C: no malloc in path;
   TS: no `new` in step).
5. **G5 parity**: identical action sequences for identical `behind` traces
   across C/Rust/TS (shared trace fixture, xlang style).

## Cadence presentation policies (Series 7 extension — LATEST_WINS / PACED_INTERPOLATE / BURST_COALESCE)

The governor decides WHICH CLASS of action fits the consumer's freshness;
the mobile display loop needs one more answer: GIVEN a vsync-paced tick
stream and a volatile input feed, WHICH frame do we present this tick, and
do we synthesize one? Every real app hand-rolls this too — "poll every
rAF, draw if new, else skip", "always draw, lerp between the last two
frames", "throttle to half refresh when the feed is bursty" — duplicated,
tuned by vibes, untestable. The cadence policy makes it a spec'd, pure,
zero-allocation state machine with a CLOSED policy set:

```
ticker ──tick + latestSeq──> CADENCE POLICY ──{present? interp? alphaQ12 coalesced}──> draw loop
```

This section is an extension of RFC-0009's ladder, not a fifth action: the
ladder acts on *staleness* (framesBehind), the policy acts on *presentation*
(latestSeq at display ticks); they compose — the consumer steps the
governor for the rebuild/snapshot classes and the policy for the per-tick
raster decision. The three policies are CLOSED (a fourth is a new RFC, same
discipline as the action set).

- **LATEST_WINS** (kind 0) — the newest-wins raster: present iff
  `latestSeq` advanced since the last present; the jumped-over frames are
  coalesced BY DECISION (`coalesced = latestSeq - lastPresentedSeq - 1`,
  Law 4). Steady by construction: at most one present per tick, never a
  stale re-raster. Best for state canvases (charts, telemetry) where a
  blended stale frame is a lie.
- **PACED_INTERPOLATE** (kind 1) — the display-rate raster with a one-period
  lag: holds the last two observed seqs and presents
  `blend(prev, newest, alpha)` where `alphaQ12 = clamp((tick -
  newestObsTick) * 4096 / period, 0, 4096)` and `period = max(1,
  newestObsTick - prevObsTick)` (the observed inter-arrival period, in
  ticks — integer arithmetic only). At the arrival tick alpha=0 (raster =
  prev = the completed previous blend — continuous by construction); each
  subsequent tick advances alpha; saturation holds the raster (presents
  elide) until the next arrival re-windows. The result is a present on
  (nearly) EVERY display tick while content flows, regardless of input
  cadence — the steady 60/120 Hz contract — with motion smoothness bounded
  by the observed period, never extrapolated past the newest frame (a
  saturated blend is honest holding, not invented future). Interpolated
  presents are counted in `interpFrames` (advisory): synthesis is a
  decision, not an accident (Law 4 applied to invention, not just drops).
- **BURST_COALESCE** (kind 2) — the adaptive sub-rate raster: tracks an
  integer Q12 EWMA of inter-arrival GAPS (ticks between arrival ticks,
  weight 1/4 — constant for regular content, so the estimate converges
  exactly instead of oscillating the way an arrivals-rate EMA does on
  periodic input) and every `REASSESS_TICKS` (default 8) ticks recomputes
  the pacing divisor `K = clamp(round(gapEWMA), 1, 64)` — present at most
  once per K ticks (the newest at each present tick, everything between
  coalesced and counted). When content flows at or above display rate the
  gap is 1 and K stays 1 (degrades to newest-wins — nothing to save); when
  content flows at a steady fraction of display rate (30 Hz on 120 Hz) the
  gap is 4, K locks onto the content beat and the loop presents exactly on
  that beat — the raster sub-rate is STEADY instead of aliasing between 0
  and 1 presents per tick as bursts land; a burst feed (24 frames every
  10 ticks) paces at one present per burst, the newest, fully coalesced.
  Queueing is NOT done (the RFC-0009 alternative rejection stands): bursts
  are absorbed latest-wins, only the PRESENT pace adapts.

**Inputs**: `latestSeq` (monotonic seq observed at this tick — a fan-out
reader's `claim.seq`, or the triad cursor's latest), one `step()` per
display tick. **Outputs** (closed record): `present`, `interp`,
`alphaQ12` (Q12; valid when `interp`), `coalesced`, `presentSeq`. All
state is integers; `step()` is a pure function of the tick trace — no
clock reads (ticks ARE the clock; the caller's vsync source owns time),
which makes cross-language parity byte-comparable (PC gates below).
**Zero allocation**: the decision record is identity-stable and mutated
in place (the `weft_gov_action_t` pattern).

### Cadence conformance (PC-series gates)

1. **PC1 bounded rate**: no policy ever presents more than once per tick;
   LATEST_WINS never presents an unchanged seq; PACED presents only when
   the (prev, newest, alpha) raster triple changes; BURST only when
   `tickInCycle >= K` AND the seq advanced.
2. **PC2 telescoping (Law 4)**: LATEST_WINS/BURST:
   `sum(coalesced) == lastPresentedSeq - presents`; PACED:
   `sum(coalesced) == newestSeq - arrivalTicks`. Decided drops and only
   decided drops appear in `coalescedByDecision`.
3. **PC3 parity**: identical packed decision traces for identical
   arrival traces across TS/Kotlin/Swift/Dart
   (`fixtures/xlang-cadence/`, the G5 discipline).
4. **PC4 zero allocation**: `step()` allocates nothing (identity-stable
   record; TS: no `new` in step; Kotlin: allocated-bytes audit on the
   JVM; Swift/Dart: identity audits — per-port honesty about what is
   provable where).
5. **PC5 steady cadence under volatility**: crafted deterministic regimes
   — 30 Hz-on-120 Hz (PACED presents every tick; BURST K converges to 4
   and presents on the beat), 240 Hz-on-120 Hz (LATEST/BURST present each
   tick; PACED presents each tick one period behind), 8 kHz bursts
   (LATEST coalesces ~66/120 per present; BURST K->1) — exact counts
   pinned (the traces are deterministic; a range gate would be weaker
   than the truth).
6. **PC6 policy-switch safety**: switching policy kind mid-trace never
   presents a seq older than the last presented (monotone presentSeq),
   and counters survive the switch (advisory state is never lost).

## Alternatives considered

- **Writer-side pacing** (slow the writer when a consumer lags): violates
  the river — the writer must never wait (02-KERNEL §1).
- **Global governor** (one controller per stream, not per consumer): RFC 0008
  explicitly chose per-reader accounting; a shared controller reintroduces
  the cross-consumer coupling it rejected.
- **Queue the lagging frames**: that's a message queue, not a state plane.
  Latest-wins is the contract; the governor decides *within* it.

## Drawbacks

- One more driver-layer object to document. Mitigated: closed action set,
  4 thresholds, no hidden state.
- Apps may disagree with the default ladder. Mitigated: thresholds are
  constructor args with published defaults; the *ladder shape* is the spec.

## Open questions

- Should `Snapshot` auto-trigger consumer-side re-claim (a `claim()`
  inside the governor) or stay advisory? Current lean: advisory only —
  the governor never touches a Triad; it reads FrameCursor.
- Does the fan-out reader (RFC 0004) want a governor built into
  `WeftFanoutReader`, or composed by the app? Current lean: composed —
  the reader's `framesBehind()` (per-consumer, from 0008) is the input;
  composition keeps the reader's API frozen.

## Hardware Deferral List

None — the governor is pure control logic.

## Staff Decision

[EMPTY]

## Implementation record (2026-09-16 — evidence only; Status remains Draft, a staff action)

Implemented in TS + C + Rust per the nanoseconds work order:

- `packages/core/src/governor.ts` (`FreshnessGovernor`, exported; the RFC's
  published defaults), `core/c/governor.{h,c}` (`weft_governor_*`),
  `core/rust/src/governor.rs` (`Governor`). Time is INJECTED
  (`step(behind, nowMs)`) — step is a pure function of the trace, which is
  what makes G5 deterministic; the rate-limited Reseed degrades to Snapshot
  (the documented fallback).
- Conformance: G1/G2/G3/G3b/G4/Law-4/custom pinned in all three languages
  (`packages/core/test/governor.test.ts`, `core/c/governor_test.c`,
  `core/rust/tests/governor_test.rs`); G5 via
  `fixtures/xlang-governor/` — one deterministic xorshift32 trace (§0.2),
  three emitters, byte-identical action logs (20,001 bytes), all four
  action classes exercised. Wired into the native CI shard.
- Wiring (the open questions' "composed by the app" lean):
  `demos/web/src/components/FeedFanoutViews.tsx` — each fan-out view owns
  its governor, `claim.dropped -> gov.step() -> decideDraw()`
  (`demos/web/src/modes/governorPolicy.ts`, the shared pure policy).
- B4 proof (`demos/web/scripts/governor_bench.ts`, evidence committed):
  the B4 display-adversarial matrix, naive vs governor draw policy on a
  real worker-driven fan-out ring. Redundant-poll regime (hold=0):
  100% of draw memcpys elided, 22-43% of claim fences elided, convergence
  gated per row. Paced-reader regime (hold >= 10 ms): savings ~0 — honest
  finding, nothing to elide when the consumer is slower than the writer.

## Implementation record (2026-09-17 — Series 7: cadence policies + VM ports; Status remains Draft, a staff action)

The §Cadence presentation policies extension landed with the VM-port
governor wave (the "deterministic cadence, 0-GC memory pooling & mobile
lifecycle" work order):

- Ladder: `packages/core/src/governor.ts` (unchanged) + the three VM
  ports — `core/kotlin/Governor.kt`, `core/swift/Governor.swift`,
  `core/dart/governor.dart` (android/apple/flutter mirrors) — same
  thresholds, same cooldown, same PROTOCOL kind values; G-series pinned
  per port (`GovernorTest.kt`, `GovernorTests.swift`,
  `governor_test.dart`); G5-VM: the shared xorshift32 ladder trace
  extended to the VM emitters (fixtures/xlang-governor/vm/).
- Policies: `packages/core/src/cadence.ts` (the TS reference),
  `CadencePolicy` co-located in each VM port's Governor module —
  PC-series pinned in the same batteries; PC3 via
  `fixtures/xlang-cadence/` (one xorshift32 arrival trace, four
  emitters, byte-identical packed decision logs, all three policies
  exercised in one stream).
- Composition (the second open question's "composed" lean, realized):
  `GovernedFanoutConsumer` per VM port — reader + governor + policy +
  two-frame history over the Series-7 buffer recyclers; the RN worklet
  (`packages/react-native/src/governed-ui-thread.ts`), the Compose
  DrawScope adapter (`android/weft-compose/.../WeftGovernedDraw.kt`) and
  the Flutter painter (`packages/flutter_weft/.../governed_painter.dart`)
  all drive that one composition. The governor still never touches a
  Triad or a ring; the consumer does, once per tick.
- PC5 evidence: `demos/web/scripts/cadence_bench.ts` (the B4 successor:
  120 Hz ticker vs volatile worker feed, per-policy present-rate
  steadiness, evidence committed) + the pinned exact-count regimes in
  every port's PC battery.
