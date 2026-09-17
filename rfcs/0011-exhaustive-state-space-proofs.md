---
RFC: 0011
Title: Exhaustive State-Space Proofs, Chaos Injection & Crash Resilience
Status: Implemented (this branch; evidence inline)
Authors: Weft Core Team
Created: 2026-09-17
Supersedes / Superseded-by: None
---

# RFC 0011 — Exhaustive State-Space Proofs, Chaos Injection & Crash Resilience

## Summary

Three new proof tiers for the RFC 0004 fan-out ring and the RFC 0006
reattach discipline, answering one question at three depths: **can any
schedule — however adversarial — violate the protocol's contract?**

1. **Deterministic Chaos Engine** — a seeded chaos runner that injects
   thread preemption, memory-bus stalls, CPU throttling, and out-of-order
   execution anomalies into concurrent reader/writer pairs, at 10,000,000+
   iterations per nightly tier, with a BIT-EXACT cross-language contract:
   C, TS, JVM, Dart (and Swift via the Apple CI leg) execute the identical
   schedule and emit byte-identical verdicts. A red verdict replays with
   the same seed, everywhere.
2. **Formal Protocol Verification** — two TLA+ models checked exhaustively
   by TLC: `FanoutSeqlock` (the multi-reader ring: no torn accepted frame,
   exact telescoping, stamp bracket, no future, and no-starvation liveness
   under weak fairness) and `ReattachPolicy` (process death: no stale
   access, no blind attach, no leak on realloc, catch-up liveness). Plus
   four new loom deep-models (reordered fill, three readers, reader rejoin
   after wrap, preemption-bound-3 sweep).
3. **Automated Crash & Parity Telemetry Guardian** — a CI watchdog that
   flags a **≥ 3%** throughput drop against a pinned baseline, a drift of
   **a single byte** in the cross-language wire layout (observed at runtime
   by a C probe that reads the REAL kernel's bytes, not echoed constants),
   and any crash/failure signature in the result artifacts. The guardian
   must *bite* on poisoned fixtures in its own selftest — a watchdog that
   stays green on a bad fixture is itself a red gate.

## Motivation

The torture gates (F10, ASAN/TSAN builds) explore the interleavings the OS
happens to produce. That is exactly the distribution production draws from
— but it is neither adversarial nor reproducible: a tear that fires once in
ten million frames cannot be re-run, because the schedule that triggered it
is gone. And nothing upstream proves the PROTOCOL (as opposed to the
implementations) correct: a counterexample would still be discovered by
sampling, never by exhaustion. The loom model closed part of that gap at
the memory-model level; this branch closes the rest.

## Guide-level explanation

Reproduce a chaos failure:

```bash
make -C core/c fanout-chaos
./core/c/fanout-chaos stepped 200000 4 4 2 200 200 1337   # deterministic
./core/c/fanout-chaos free 10000000 4 16 3 30 31415926    # 10M-frame tier
```

The stepped verdict is byte-identical in TS, JVM, and Dart (same config):

```bash
bash ci/scripts/run_chaos_parity.sh     # C==TS==JVM==Dart byte-diff + golden fixture
```

Run the proofs:

```bash
bash ci/scripts/run_formal_shard.sh     # TLC: FanoutSeqlock + ReattachPolicy
cargo test --release --test loom_fanout -- --ignored   # the bound-3 sweep
```

Run the guardian:

```bash
python3 tools/guardian/guardian.py selftest   # it must bite on poisoned fixtures
bash ci/scripts/run_guardian_shard.sh         # the CI wiring
```

## Reference-level specification

### The chaos contract (normative; mirrors in every port)

- PRNG: Marsaglia xorshift128 over four u32 words; per-step draws in the
  fixed order: thread pick (`next() % (R+1)`), fault-rate check
  (`next() % 1000 < chaosRate`), then fault victim and kind.
- Fault classes: PREEMPT (freeze 1 step), STALL (2 steps), THROTTLE
  (3 steps), REORDER (reverse that thread's payload word order on its next
  fill/copy). A frozen thread burns its freeze only when the scheduler
  chooses it.
- Seed derivation: `a = mix32(seed ^ 0xA341316C)`, `b = mix32(seed ^
  0xC8013EA4)`, `c = a ^ 0x9E3779B9`, `d = b ^ 0x85EBCA6B`; free mode
  folds the thread id into the same constants.
- Property ledger L-C1..L-C6 (no torn accepted frame, no future, exact
  telescoping, publish completion, bounded-and-counted resolutions, stamp
  bracket), adjudicated once at drain end and serialized as compact JSON
  with fixed field order.
- The ledger's crossed-language identity is enforced mechanically:
  `run_chaos_parity.sh` byte-diffs C vs TS vs JVM vs Dart on two configs,
  and every port's CI test pins the committed golden fixture
  (`tools/chaos-fixtures/stepped-golden-200k.json`).

### Formal models

- `formal/fanout/FanoutSeqlock.tla` — the tear window is REAL in the model:
  the reader copies one word per step and the writer can invalidate and
  refill the same slot between them. TLC proves over the complete state
  space (19,443 states at the checked bounds; collision probability
  4.6e-12) that no schedule accepts a torn frame, telescoping is exact,
  the stamp bracket never splits, and (under weak fairness) the writer
  finishes and every reader accepts the final frame. Nightly bounds:
  3 readers, 4 frames.
- `formal/reattach/ReattachPolicy.tla` — process death with both RFC 0006
  policies; proves the SIGSEGV class unrepresentable (no publish through a
  stale handle; claims only through a live-generation attach, with the
  epoch-mismatch revalidation path modeled), no blind attach (validation
  gate on every recreate), no leak on realloc, per-session telescoping,
  and reattach/catch-up liveness.
- Loom deep-models extend the exhaustive memory-model sweep: reordered
  fill, three readers, reader rejoin after multiple ring wraps, and the
  preemption-bound-3 sweep (`#[ignore]`, run nightly).

### The guardian

- Wire manifest: `tools/guardian/wire-manifest.json` (ring ctrl block,
  envelope, canary, mixer vectors). The C probe observes the layout by
  publishing distinctive frames through the real API and reading raw bytes
  at the documented offsets; the guardian diffs 52 fields — any single
  byte of drift is RED.
- Throughput: median-vs-median against
  `ci/baselines/guardian-throughput-baseline.json`; ≥ 3% drop = RED;
  missing baselines/results are RED (a guardian that finds nothing to
  look at is not green).
- Crash telemetry: shard result artifacts + litmus results audited; any
  `FAILED`/`pass=false`/crash signature is a finding.
- Selftest: poisoned fixtures (drop, one-byte drift, failed shard) must
  each trigger a bite; the healthy set must pass.

## Rationale and alternatives

Wall-clock fuzzing alone cannot replay; model checking alone cannot scale;
static manifests alone cannot see a runtime drift. The three tiers cover
each other's blind spots, and the cross-language chaos contract makes a
failure in ANY port reproducible in ALL of them.

## Drawbacks

- The stepped contract is now a frozen surface: changing a draw order, a
  freeze rule, or a JSON field is a breaking change requiring a version
  bump and simultaneous port updates.
- TLC runs add a Java dependency to the formal shard (jar fetched with a
  pinned sha256 into `.tlc-cache/`).
- The guardian's 3% gate will bite on legitimate slowdowns (shared
  runners); the baseline update protocol is the escape hatch and the
  threshold is configurable.

## Prior art

loom (Rust), Jepsen's deterministic schedulers, TLA+/TLC (Lamport),
Jepsen's "nemesis" fault taxonomy, and the repo's own F10/litmus discipline.

## Status

Implemented on this branch with evidence: chaos engine + four port engines
+ parity gate (byte-identical C==TS==JVM==Dart on two configs), two TLC
proofs (no error found), four loom deep-models (bound-2 green; bound-3
nightly), guardian selftest green, and the four new CI shards + the
nightly 10M-frame tier.
