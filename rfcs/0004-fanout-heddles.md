---
RFC: 0004
Title: Multi-Consumer Fan-Out Heddles
Status: Accepted
Authors: Weft Core Team
Created: 2026-09-15
Supersedes / Superseded-by: None
---

# RFC 0004 — Multi-Consumer Fan-Out Heddles

## Summary
Proposes an architectural pattern for 1-writer, N-reader fan-out pipelines (e.g., primary canvas, minimap, flight recorder, and network telemetry visualizer simultaneously observing a single stream) without modifying the frozen 1-writer/1-reader kernel. Implemented since 2026-09 as the userland driver layer in `@weft/core` (`WeftFanoutBroadcaster` / `WeftFanoutReader`) with Heddle bindings across the four TS frameworks.

## Motivation
Applications often require multiple independent consumers with varying refresh rates (e.g. 120 Hz recorder, 60 Hz canvas, 30 Hz minimap, 15 Hz network graph). Multiple distinct readers cannot bind directly to a single 1:1 Triad without contention or drops.

## Guide-level explanation
Developers instantiate a `WeftFanoutBroadcaster` (or seqlock driver layer) that distributes frames into consumer slots. Each reader polls its own pre-allocated buffer with zero steady-state allocations (Law 2 preserved).

Concretely, since the 2026-09 implementation (`@weft/core`):

```ts
const b = new WeftFanoutBroadcaster(payloadFloats);   // 4-slot ring by default
const slot = b.begin();      // Float32 view; slot stamp invalidated BEFORE fill
slot[0] = value;             // fill
b.publish();                 // wait-free; frame seq is internal & monotonic

const r = b.createReader();  // N of these, one per consumer
const claim = r.claim();     // { fresh, seq, dropped }, mutated in place
if (claim.fresh) draw(r.view());
```

## Reference-level specification
- **Seqlock Ring**: 4–8 pre-allocated buffer slots (implementation default 4).
- **Writer Sequence**: Monotonically increasing sequence counter (i64 in the implementation; frames numbered from 1).
- **Reader Consumption**: Reads latest completed sequence; computes frame drops per reader independently. The implementation satisfies the exact telescoping identity `sum(dropped) == lastSeq - freshClaims`.
- **Throughput**: Verified 1.27 million publishes/sec across 4 concurrent readers in the D-17 TypeScript simulation (`evidence/D-17/fanout_prototype.log`); the production implementation with validated claims re-measured 1.09 million publishes/sec (`spikes/fanout-heddles/fanout_driver_layer.log`, `node/linux-sandbox`) — the ~14% delta is the cost of tear validation, purchased deliberately.

Implementation refinements added 2026-09 (beyond the D-17 spike's sketch, which had an undetectable tear window):

- **Stamp-then-fill bracket**: `begin()` invalidates the target slot's stamp (sets it to 0) BEFORE returning the fill view; `publish()` re-stamps the slot then flips `latestSeq`. A reader mid-copy of frame `f` in a slot that the writer is overwriting for `f + M` always observes the stamp change — the bracket the spike omitted, and the direct analog of the kernel's envelope→canary→exchange bracket.
- **Bounded-retry claim**: the reader validates the slot stamp before and after its copy (≤ 4 attempts, chasing newer completed frames); on exhaustion it keeps its last consistent frame and counts the miss. Never an unbounded spin (Law 1).
- **Per-reader copy**: each reader copies the freshest consistent frame into its own pre-allocated buffer. One Float32 copy per fresh claim is the price of N-reader support at zero kernel surface; the kernel's 1:1 slot swap stays zero-copy.
- **Single writer by contract** — the same discipline as the kernel's writer.
- **Seq-gap accounting**: `dropped` counts sequence gaps; a frame whose `begin()` was abandoned is indistinguishable from a missed publish (declared boundary, not an error).

### Invariants touched
None of the kernel invariants I1–I6 — the ring lives entirely outside the kernel. New driver-layer invariants (pinned by the F-series battery, `packages/core/test/fanout.test.ts`):

- **FI1** — stamp-then-fill bracket: a slot's payload is only readable through a stamp window that brackets every overwrite.
- **FI2** — per-slot stamp monotonicity: stamps on a slot are strictly increasing frame seqs (or 0 while invalidated), so an unchanged stamp proves a consistent copy.
- **FI3** — per-reader telescoping drop accounting (`sum(dropped) == lastSeq - freshClaims`, exact).

### Litmus impact
No L-series change — the kernel litmus suite is untouched and stays canonical for kernel semantics. The driver layer is covered by the package-level F-series battery: 28 tests in `packages/core/test/fanout.test.ts`, including a cross-thread protocol litmus where an independent worker-side writer (plain JS implementing this section's layout, not importing the class) publishes 100k frames while three readers validate every claimed byte — zero torn claims observed (`node-vitest/linux-sandbox`). No existing test fails; the addition is purely userland.

### Envelope impact
None. Fanout frames are not triad envelopes — no magic, version, or payload_len; the slot stamp is the frame id. The ring composes beside the kernel, not inside its wire format.

## Boundary of the claim (Law 4)
Fan-out decouples readers from the writer, but slow readers will observe dropped frames (`t_drop > 0`) if their consumption rate is lower than the writer's publish rate. A mid-overwrite claim skips the tick gracefully (observable in stats, never silent, never a spin). Each reader pays one payload copy per fresh claim. The i64 sequence counter is surfaced as Number — exact below 2^53, unreachable in any real session at 10^6 publishes/s (declared, not assumed). Multi-canvas rendering is approximately synchronized: readers claim independently, so two canvases can momentarily display different (each internally consistent) frames — by design, latest-wins per consumer.

## Alternatives considered
- Kernel-level N-reader atomic CAS array: Violates Kernel Freeze and increases atomic contention.
- Broadcast via message queue / EventEmitter: Breaks Law 2 (creates garbage on every frame).
- N-Triad star (writer publishes into N child Triads): inherits kernel tear-freedom but costs O(N) kernel publishes per frame and N× envelope overhead; the seqlock ring gets N-reader support for one writer-side stamp pair. Rejected on writer-path cost, not correctness.

## Drawbacks
Consumes more memory (N slots × payload size). Each reader pays one Float32 copy per fresh claim (the kernel's 1:1 path stays zero-copy — fan-out is not a zero-copy claim). The accounting is seq-based, so abandoned writer seqs read as drops. Single writer per ring, by contract.

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
- I6 multi-reader lifecycle and GC finalization ordering across heterogeneous runtimes. Partially answered for the TS runtime: a `WeftFanoutReader` is plain views + one buffer over a GC-managed SAB — disposal is GC-native, no handshake required; the ring's latest-wins semantics make a dead reader cost nothing. Remains open for Dart/Kotlin/Swift ports where finalizers are NOT guaranteed timely (see RFC 0006 for the Android-side rehydrate seam).

## Implementation plan
- **Built**: driver layer in `@weft/core` + bindings (`WeftFanoutCanvas`, `useWeftFanout`, `weftFanoutCanvas`, `useWeftFanoutDraw`) + 28-test F-series battery + 23 binding tests + benchmark, delivered on branch `contrib/rfc-0004-fanout-driver-layer` (2026-09).
- **Mechanical acceptance criterion** (flips `Status` to `Implemented`): F-series battery green in CI (`pnpm --filter @weft/core test`), cross-thread litmus with zero torn claims, api-extractor baselines regenerated, and the benchmark log committed with environment tags.
- **Status flip is a staff action**: per round-6 §4 this RFC "stays userland until an RFC-0004 implementation directive is issued"; the implementation above is offered for that directive. Kernel RFCs need two kernel-maintainer approvals; this RFC needs none (no kernel surface), one maintainer approval per the contribution ladder rungs 2–3.

---

## Staff Decision

**ACCEPTED as driver-layer pattern** — round-6 adjudication §4 (2026-09-16): "Log internally consistent (per-reader fresh/drop accounting), honestly tagged; stays userland until an RFC-0004 implementation directive is issued." The 2026-09 implementation series on `contrib/rfc-0004-fanout-driver-layer` delivers the userland implementation and the mechanical acceptance criteria above; the flip to `Implemented` awaits the staff directive.

---

*Process notes: lazy consensus, 7 days — silence is consent; a substantiated objection cites a
Law, an invariant, or a litmus test. Kernel RFCs additionally require both kernel-maintainer
approvals. See [CONTRIBUTING.md](../CONTRIBUTING.md) §3 and [GOVERNANCE.md](../GOVERNANCE.md).*
