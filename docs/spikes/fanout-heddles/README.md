# spikes/fanout-heddles — Multi-Consumer Fan-Out (RFC 0004)

## What is real here

| File | Status | What it is |
|------|--------|------------|
| `fanout.cjs` | **the implementation** | Concurrent 1-writer / N-reader seqlock ring over one SharedArrayBuffer: per-slot even/odd latch, control-block publication point, bounded-retry stale-tolerant claims, exact per-reader drop accounting. Zero allocation on publish and claim paths. |
| `fanout_concurrent_test.cjs` | **the gate** | True-parallelism torture test (worker_threads): 1 writer sprinting at full speed vs 4 verifying readers. Hard gates G1–G6; exits 1 on any violation. Phase 2 is the RFC composition: Triad kernel → broadcaster → ring → 4 readers. |
| `fanout_bench.cjs` | evidence | Concurrent throughput under writer/reader contention (verification off). |
| `fanout_prototype.ts` / `.cjs` | superseded | The original single-threaded sketch (plain fields, no atomics). Kept as the RFC's historical artifact — its 1.27M pub/s claim was never earned under parallelism. |

## Protocol summary

**Layout** — one SAB: 128 B control block (`latestSeq`, `done`, `pubCount`) + N slots
(128 B header: `latch` + `frameSeq`; then the payload, 64 B aligned).

**Writer** (frame F → slot `(F−1) mod S`):

1. `Atomics.store(latch, odd)` — exclude readers
2. plain-write `frameSeq` + payload (readers excluded by the odd latch)
3. `Atomics.store(latch, even)` — content complete (SeqCst store = release)
4. `Atomics.store(latestSeq, F)` — **the publication point**

Because every atomic here is SeqCst, any reader that loads `latestSeq = F`
then necessarily observes the slot's even latch: the published slot can
never look complete-empty.

**Reader** (bounded-retry, stale-tolerant — never blocks):

1. `G = Atomics.load(latestSeq)`; `0` → nothing published
2. aim at slot `(G−1) mod S`; `r1 = Atomics.load(latch)`; odd → retry
3. plain-read `frameSeq` + copy payload
4. `r2 = Atomics.load(latch)`; `r1 ≠ r2` → torn (slot reused mid-copy) → retry
5. stable ⇒ internally consistent frame `F ≥ G` (a slot only ever holds
   frames ≡ slot+1 mod S, strictly increasing — so the snapshot is frame G
   or a NEWER reuse; both are legitimate latest-wins deliveries)
6. freshness by u32 serial-number arithmetic (`after(F, lastSeen)`, wrap-safe
   for gaps < 2³¹); `framesBehind = (F − lastSeenPrev − 1) >>> 0` — the same
   per-reader semantics as FrameCursor (RFC 0008)

Retry bound: 8 attempts, then `ST_EXHAUSTED` — the caller keeps its previous
frame and nothing is ever blocked (the reader-facing Law 4 boundary). Torn
retries are expected under slot-reuse pressure and are counted, not hidden.

**Kernel Freeze** — the fan-out is a DRIVER-layer construct (RFC 0004's
whole point): one kernel reader claims from the frozen 1W/1R Triad and
republishes into the ring. The kernel has no knowledge of consumers; no
kernel file is touched. `fanout_concurrent_test.cjs` phase 2 exercises
exactly this composition through `core/ts/weft.ts`'s public cursor API.

## Evidence (linux-sandbox, Node 24, 2026-09-16)

```
phase 1  sprint : writer 0.726M pub/s over 400000 frames — 4 verifying readers
  reader[0..3] fresh=32..107239 dropped exact, tornRetries=19..182, violations=0
phase 2  daisy  : Triad kernel → broadcaster → ring → 4 readers, 237K frames/s, violations=0
GATE: ALL PASSED (integrity, convergence, exact drop accounting)

bench: writer 0.765M publishes/sec (latch protocol INCLUDED, 4 readers contending)
       aggregate 0.783M fresh deliveries across 4 readers; per-reader drops exact
```

The numbers that matter are the zeros: **integrity violations = 0** across
every claim in every reader, and **drop accounting exact** (telescoping
identity `totalDropped = lastSeq − firstSeq − (freshCount − 1)` holds for
every reader). Rates are environment-relative; the invariants are not.

## Relationship to litmus

L1–L8 remain the frozen kernel conformance suite (24 cells) and are untouched.
The fan-out is driver-layer, so its conformance lives here, not in the litmus
matrix: `fanout_concurrent_test.cjs` is wired as the `fanout-concurrent` CI
shard (extreme-test.yml) with the same artifact conventions
(`ci/run-artifacts/shard-fanout-concurrent.log` + results JSON).
