---
RFC: 0008
Title: FrameCursor — freshness telemetry and adaptive draw budget (no kernel change)
Status: Accepted (design)
Authors: Architectural breakthrough contribution series
Created: 2026-09-16
Supersedes / Superseded-by: None
---

# RFC 0008 — FrameCursor: freshness telemetry and adaptive draw budget

## Summary

Resolves RFC 0001 §10's open question ("whether `claim()` should expose `seq`
staleness (`framesBehind`) to draw code for adaptive detail-level — telemetry
first, API later") **without touching the frozen kernel**: the reader diffs
its own claimed envelope seqs between claims,

```
framesBehind = seq_now − seq_prev − 1
```

and exposes the value through a tiny driver-layer `FrameCursor` available in
all four Heddle-language ports (TS `@weft/core/cursor`, Kotlin, Swift, Dart).
Dropped frames stop being invisible loss and become a **control signal**:
draw code can lower detail level when behind, restore it when caught up, and
telemetry can quantify "now-ness" — the project's entire thesis — per reader.

## Motivation

The kernel drops intermediate frames by design (latest-wins *is* the
semantics of display), and `seq` gaps were already declared "observable for
telemetry" (RFC 0001 §7) — but no port exposed them. Meanwhile every
framework binding claims exactly once per VSYNC, so per-reader drop
accounting is the natural unit of honesty for the draw side: it measures how
far *this reader* is behind the writer, not global writer load.

The obvious alternative — a shared publish counter read at claim time — was
rejected: it adds a second shared atomic to the hot path. Kernel law forbids
a second atomic *participating in ownership decisions*; a telemetry-only
counter is legal but unnecessary: the envelope seq the reader already holds
carries the same information for free. Zero new atomics, zero protocol
version bump, zero frozen-surface breach.

## Guide-level explanation

```ts
// TypeScript
import { FrameCursor } from '@weft/core/cursor';
const cursor = new FrameCursor();
const draw = () => {
  const { seq, framesBehind, payload } = cursor.claim(weft);
  const lod = framesBehind > 2 ? LOD.low : LOD.high;   // adaptive budget
  render(payload, lod);
};
```

The first claim reports `first: true` with `framesBehind: 0` — the null
frame (seq 0) is the baseline, so back-to-back claims against a slow writer
correctly report 0 behind. A decreasing seq (u32 wrap / writer reset) resets
accounting rather than reporting a huge burst.

## Reference-level specification

- `FrameCursor.claim(weft)` performs the ordinary wait-free claim (one
  exchange — RFC 0001 §4.3), reads the claimed envelope's seq, computes the
  delta, and returns `{ seq, framesBehind, first, payload }` where `payload`
  is the zero-allocation live reader view (`rLive()` / `rLiveBuf()` /
  `rLivePtr(16)` / `rReadSlice(16, payloadMax)` per port).
- Accumulators: `totalDropped` (Σ framesBehind) and `claims`. Advisory per
  AXIOM T.
- Cost per claim: one extra envelope read (already cache-resident after the
  claim's acquire) + integer arithmetic. No allocation. Wait-free preserved.
- Ports: TypeScript (`packages/core/src/cursor.ts`, subpath
  `@weft/core/cursor`), Kotlin (`core/kotlin/FrameCursor.kt` + android
  mirror), Swift (`core/swift/FrameCursor.swift`), Dart
  (`core/dart/frame_cursor.dart`). Same shape, port-idiomatic payloads.
- The web demo's Mode C HUD now reports real per-reader drops through the
  cursor (previously it printed the kernel's revocation counter, which is
  identically zero outside L7-style tests — a misleading display).

## Boundary of the claim (Law 4)

`framesBehind` measures frames published between a reader's own claims that
that reader never saw. It is NOT a global drop counter, NOT a latency
measure, and says nothing about other readers. Multi-reader fan-out
accounting remains RFC 0004's domain.

## Alternatives considered

- **Kernel-level `claimDetailed()` with a shared publish counter** — adds a
  second shared atomic read to every claim and a frozen-surface change for
  information the envelope already carries. Rejected.
- **Seqlock ring with per-reader cursors (RFC 0004) for everything** —
  orthogonal: fan-out solves *distribution*; FrameCursor solves *freshness
  visibility* for the existing 1:1 hot path. Composable, not competing.
- **Do nothing** — "drops are free" stays true but unmeasurable; the
  adaptive-LOD opportunity is wasted.

## Staff Decision

[EMPTY]
