---
RFC: 0004
Title: Multi-Consumer Fan-Out Heddles
Status: Draft
Authors: Weft Core Team
Created: 2026-09-15
Supersedes / Superseded-by: None
---

# RFC 0004 — Multi-Consumer Fan-Out Heddles

## Summary
Proposes an architectural pattern for 1-writer, N-reader fan-out pipelines (e.g., primary canvas, minimap, flight recorder, and network telemetry visualizer simultaneously observing a single stream) without modifying the frozen 1-writer/1-reader kernel.

## Motivation
Applications often require multiple independent consumers with varying refresh rates (e.g. 120 Hz recorder, 60 Hz canvas, 30 Hz minimap, 15 Hz network graph). Multiple distinct readers cannot bind directly to a single 1:1 Triad without contention or drops.

## Guide-level explanation
Developers instantiate a `WeftFanoutBroadcaster` (or seqlock driver layer) that distributes frames into consumer slots. Each reader polls its own pre-allocated buffer with zero steady-state allocations (Law 2 preserved).

## Reference-level specification
- **Seqlock Ring**: 4–8 pre-allocated buffer slots.
- **Writer Sequence**: Monotonically increasing 64-bit sequence counter.
- **Reader Consumption**: Reads latest completed sequence; computes frame drops per reader independently.
- **Throughput**: Verified 1.27 million publishes/sec across 4 concurrent readers in TypeScript simulation.

## Boundary of the claim (Law 4)
Fan-out decouples readers from the writer, but slow readers will observe dropped frames (`t_drop > 0`) if their consumption rate is lower than the writer's publish rate.

## Alternatives considered
- Kernel-level N-reader atomic CAS array: Violates Kernel Freeze and increases atomic contention.
- Broadcast via message queue / EventEmitter: Breaks Law 2 (creates garbage on every frame).

## Drawbacks
Consumes more memory (N slots × payload size).

## Implementation evidence (2026-09-16)

The original prototype (`spikes/fanout-heddles/fanout_prototype.ts`) validated
the shape but was single-threaded — plain fields, no atomics — so its
"1.27 million publishes/sec" number was never earned under real parallelism
and its `claimLatest` had a latent tear window it could not even express.

The pattern is now REAL (`spikes/fanout-heddles/fanout.cjs`): SharedArrayBuffer
+ Atomics (all SeqCst — the TS port's documented ordering regime), per-slot
even/odd latches, a control-block publication point, bounded-retry
stale-tolerant claims, and exact per-reader drop accounting (RFC 0008
semantics per consumer). Conformance lives in `fanout_concurrent_test.cjs`
(wired as the `fanout-concurrent` CI shard in extreme-test.yml) with hard
gates: zero payload integrity violations under writer/reader contention,
convergence to the final frame, and the telescoping drop-accounting identity.
Phase 2 of the test exercises the composition this RFC proposes: a Triad
kernel reader (public cursor API, kernel untouched) republishing into the
ring for four consumer workers. Honest concurrent rates are in
`fanout_bench.cjs` — environment-relative, invariants are not. Details:
`spikes/fanout-heddles/README.md`.

The Open question below (I6 multi-reader lifecycle) remains open: the ring
is driver-layer, so slot ownership follows each runtime's GC rules; the
kernel's I6 handshake governs only the broadcaster's claim of the source.

## Open questions
- I6 multi-reader lifecycle and GC finalization ordering across heterogeneous runtimes.

## Hardware Deferral List
- None (pure userland / driver layer pattern).

## Staff Decision
[EMPTY]
