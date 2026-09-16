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
