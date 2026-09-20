---
RFC: 0018
Title: weft_sync — Multi-Stream Temporal Synchronization (Sensor Fusion)
Status: Draft
Authors: Engineer 3 (Observability Fabric & Reactive Dataflow)
Created: 2026-09-20
Supersedes / Superseded-by: None
Requires: 0017 (weft_flow views), 0009 §BURST_COALESCE (coalescing precedent), 0002 (monotonic hardware clocks)
---

# RFC 0018 — weft_sync: multi-stream temporal synchronization

## Summary

Real systems do not produce one stream. A robot head produces 60 Hz video,
100 Hz audio chunks, and 200 Hz IMU telemetry — three independent rings,
three producers, three clocks, zero shared state. The consumer (a fusion
pipeline, a recorder, a renderer) needs **coherent tuples**: one video frame
with the audio chunk and the IMU sample that belong to the same instant.

Classic answers all allocate or block: rebuffering queues with condition
variables, or a "sync engine" thread with a heap of pending items.
`weft_sync` does neither. It is a **latest-wins aligner**: each stream holds
exactly ONE unconsumed sample (the newest), and a tuple is emitted the
moment every stream has a sample within a declared tolerance window of the
pivot timestamp. The state is O(N) integers — no queues to grow, no locks
to hold, nothing to GC (Laws 1, 2 by construction).

## Specification

### §1 Streams, offsets, pivot

- **N streams** (≤ 8), each independently fed by `weft_sync_offer`. Samples
  are `weft_flow_view`s (borrowed, RFC-0017) — the synchronizer copies the
  VIEW (24 bytes), never the payload.
- **Clock domains**: each stream has an integer offset `offset_ns[i]`
  (calibration, applied as `τ = t_ns + offset_ns[i]`). Monotonic hardware
  clocks assumed (Weft Law); offsets are static per bind — a drifting
  domain recalibrates by re-binding, not by runtime float filters.
- **Pivot policies** (closed set; a fourth is a new RFC):
  - `MAX_TS` (0): pivot = max τ — wait for the slowest domain (default;
    the tuple is "as fresh as the slowest sensor").
  - `MIN_TS` (1): pivot = min τ — emit as soon as the fastest domain's
    newest sample can cover the slowest's.
  - `ANCHOR` (2): pivot = τ[anchor_stream] — one stream rules the cadence
    (video rules; audio and IMU follow).

### §2 Tuple emission

After each accepted offer: if every stream holds a sample, compute the
pivot τ* and test every stream: `|τ_i − τ*| ≤ tolerance_ns`. On success:
write the tuple (views in stream order, ORIGINAL t_ns preserved — offsets
affect alignment, not the data) into the caller's output array, release all
holds, `t_tuples++`, return 1.

On failure: the **laggard** — the held sample with the smallest τ — is
released and `t_gap_drops++` (return −1). The tuple that sample "belonged
to" never existed; holding it would only deepen the backlog. This is the
same decided-drop honesty as the governor's Skip(n).

### §3 Late and stale samples

- A sample OLDER than the one already held (`τ < held τ`): refused,
  `t_stale_drops++` — out-of-order arrivals never regress a stream
  (latest-wins, the triad's own rule).
- A newer sample replacing an UNCONSUMED held sample: the old one is
  coalesced away, `t_coalesced++` (the RFC-0009 BURST_COALESCE precedent —
  burst discipline, not backlog).

### §4 Bounds

`offer` is O(N) with N ≤ 8: one compare, one store, one spread check. No
allocation, no loop whose bound is not N, no clock read (t_ns injected —
Law 4). The output tuple is caller storage.

## Test plan

`core/c/weft_sync_test.c` (Y-series): aligned emission (60 Hz video +
100 Hz audio chunks + 200 Hz IMU over 10 injected seconds → exactly 600
tuples), pivot policy semantics, laggard drop accounting, late/stale
refusal, coalescing, offset calibration, and a pinned tuple-checksum for
the xlang fixture.
