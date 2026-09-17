# PATCHES-WAVE4 — Exhaustive State-Space Proofs, Chaos Injection & Crash Resilience

**Branch:** `contrib/weft-exhaustive-proofs` (the lead's "Branch: Weft" assignment)
**Base:** `ff34e2d` (post-wave-3 main)
**Discipline:** unchanged from waves 1–3 — independent patches, `git format-patch` /
`git am` round-trip verified, every gate green, no silent-green anywhere.

The lead's assignment: *"Exhaustive State-Space Proofs, Chaos Injection & Crash
Resilience"* with three core depth deliverables. Each is delivered below with its
falsifiable evidence.

---

## Patch 1 — feat(chaos): C reference engine (the oracle)

**Files:** `core/c/fanout_chaos.{h,c}`, `core/c/fanout_chaos_runner.c`, `core/c/Makefile`

A deterministic chaos engine for the RFC 0004 ring with two tiers:

- **stepped** — every protocol participant is a micro-step state machine driven
  by a seeded xorshift128 scheduler. Four fault classes inject at a per-mille
  rate: PREEMPT (freeze 1 step), STALL (2, memory-bus analog), THROTTLE (3,
  CPU-throttle analog), REORDER (reverse that thread's payload word order —
  out-of-order analog). Given a config, the run is a pure function: the
  double-run diff is byte-identical.
- **free** — real pthreads over the production `weft_fanout_t` with chaos
  points between begin/fill/publish and around claims; the FAULT SEQUENCE is
  seed-deterministic (per-thread streams), the interleavings are the OS's.
  The 10,000,000-frame nightly tier lives here.

Property ledger **L-C1..L-C6** (torn-accepted=0, future=0, exact telescoping,
publish completion, bounded-and-counted, stamp bracket) adjudicated at drain
end and serialized as the contract JSON.

**Bugs found by building it** (proof the tool has teeth):
1. First reorder-fill used per-word `weft_fanout_fill` calls — which all write
   from the slot start, so words 1..15 carried stale frame data and the ledger
   caught 271,106 "torn" accepts. Fixed via raw-cursor relaxed stores (the
   documented production discipline); 2M frames then pass with 0.
2. The self-test's first PRNG replay check was self-referential — replaced
   with hard-pinned vectors shared by every port.

**Evidence:** selftest PASS; stepped 200k/1M PASS (both regimes); free 2M PASS
(3.59M injections, torn=0, telescoping OK); free 10M PASS (nightly tier);
ASAN leg PASS. `make -C core/c fanout-chaos fanout-chaos-seq fanout-chaos-asan`.

## Patch 2 — feat(chaos): TS stepped engine

**Files:** `core/ts/fanout_chaos.ts`

Bit-exact port (u32 discipline via `>>> 0` + `Math.imul` — tword's product
exceeds 2^53, so imul is load-bearing). Mirrors the C oracle draw-for-draw:
thread pick, fault-rate check, victim, kind. Node's strip-only TS mode
forbids enums, so the SM states are contract-pinned numeric constants.

## Patch 3 — feat(chaos): @weft/core chaos parity

**Files:** `packages/core/src/fanout_chaos.ts`, `packages/core/src/index.ts`,
`packages/core/test/chaos.test.ts`

Mirrors `core/ts/fanout_chaos.ts` (the wave-3 mirror pattern) and adds the
package-level vitest battery: pinned contract vectors, the committed C
golden verdict reproduced BYTE-IDENTICALLY, and L-C1..L-C6 under a 99.9%
fault rate. **3/3 green.**

## Patch 4 — feat(chaos): JVM/Kotlin stepped engine

**Files:** `core/kotlin/FanoutChaos.kt`

Kotlin `UInt` arithmetic maps 1:1 onto the contract (wrapping `times/shl`,
logical `shr`). CLI carries `stepped` + `selftest`. **JVM verdict is
byte-identical to C on both parity configs; selftest PASS.**

## Patch 5 — feat(chaos): Dart stepped engine

**Files:** `core/dart/fanout_chaos.dart`

Standalone (no Flutter dependency) parity oracle with 64-bit-int + `& 0xFFFFFFFF`
u32 discipline and `>>>` logical shifts. **Dart verdict is byte-identical to C
on both parity configs; selftest PASS.**

## Patch 6 — feat(chaos): Swift stepped engine + Apple CI leg

**Files:** `core/swift/FanoutChaos.swift`, `Tests/WeftTests/FanoutChaosTests.swift`

`UInt32` with `&*`/`&<<`/`&>>` maps 1:1. The XCTest leg pins the vectors and
the golden fixture byte-for-byte on macOS runners, plus a 99.9%-chaos property
run. STATUS banner is honest: SOURCE-ONLY in the Linux sandbox, proven in the
Apple CI leg.

## Patch 7 — feat(chaos): pinned fixtures + cross-language parity gate

**Files:** `tools/chaos-fixtures/{pinned-vectors.json,stepped-golden-200k.json,README.md}`,
`ci/scripts/run_chaos_shard.sh`, `ci/scripts/run_chaos_parity.sh`

The parity gate is the flagship: C (oracle, deterministic double-run sanity)
vs TS (always) vs JVM (kotlinc when present) vs Dart (dart when present),
byte-diffed on two configs including a 99.9% fault rate; the committed golden
must be reproduced by the C engine right now. Loud declared skips for absent
toolchains in non-CI environments; RED in CI.

**Evidence (`shard-chaos-parity`):** `[ts] BYTE-IDENTICAL`, `[jvm]
BYTE-IDENTICAL`, `[dart] BYTE-IDENTICAL` (both configs each); golden fixture
reproduced; **PASS**.

## Patch 8 — feat(formal): loom deep-models

**Files:** `core/rust/tests/loom_fanout.rs`

Four extensions to the exhaustive memory-model sweep:
1. `loom_fanout_ring_reorder_fill_exhaustive` — reversed payload fill order;
   NO TORN FRAME ACCEPTED survives every interleaving.
2. `loom_fanout_ring_three_readers` — widens the contract's N.
3. `loom_fanout_reader_rejoin_after_wrap` — the RFC 0006 reattach analog at
   ring level: detach, wrap M=2 three times, rejoin with a fresh session;
   telescoping exact over the rejoin session.
4. `loom_fanout_ring_preemption_bound3_evidence` — the bound-3 sweep
   (`#[ignore]`, minutes by design), run by the nightly leg.

**Evidence:** bound-2 tests 4 passed, 0 failed, 1 ignored (57.97s); bound-3
nightly leg wired (`loom-bound3` job).

## Patch 9 — feat(formal): TLA+ models + TLC shard

**Files:** `formal/fanout/FanoutSeqlock.tla(+.cfg)`,
`formal/reattach/ReattachPolicy.tla(+.cfg)`, `ci/scripts/run_formal_shard.sh`

- **FanoutSeqlock** — the tear window is REAL (one word per copy step; the
  writer can invalidate/refill the slot between them). TLC verdict:
  **"Model checking completed. No error has been found."** — 19,443 distinct
  states, collision probability 4.6e-12: NoTornAccepted, Telescoping,
  StampBracket, NoFuture, LatestMonotonic, TypeOK + WriterFinishes and
  ReaderSeesFinal liveness under weak fairness.
- **ReattachPolicy** — CLEAN_REALLOCATE vs REHYDRATE_PERSISTED with the
  validation gate, epoch-mismatch revalidation (RRevalidate), bounded deaths.
  TLC verdict: **"No error has been found."** — 615 distinct states:
  NoStaleAccess (the SIGSEGV class unrepresentable), NoBlindAttach,
  NoLeakOnRealloc, Telescoping + ProcessesReattach and ReaderCatchesUp.
- The shard fetches tla2tools 1.8.0 with a **pinned sha256** (`9d36716f…`)
  into `.tlc-cache/` — a different prover is a RED. Nightly runs wider bounds
  (3 readers / 4 frames / 3 deaths).

**Modeling lesson baked into the spec history:** the first draft made the
stamp-check + copy atomic, which would have made NoTornAccepted vacuous.
The final model copies one word per step so TLC genuinely explores the tear
window.

## Patch 10 — feat(guardian): the crash & parity telemetry watchdog

**Files:** `tools/guardian/guardian.py`, `tools/guardian/wire-manifest.json`,
`tools/guardian/fixtures/*`, `core/c/wire_probe.c`, `core/c/Makefile`,
`ci/scripts/run_guardian_shard.sh`

Three watches, one verdict:

1. **THROUGHPUT** — ≥ 3% median drop vs `ci/baselines/guardian-throughput-
   baseline.json` = RED; missing baseline/results = RED (Law 1: never silent).
2. **WIRE** — the C probe (`wire-probe`) publishes distinctive frames through
   the REAL kernel and reads raw bytes at the documented offsets (no echoed
   constants; `_Static_assert`s catch compile-time drift); the guardian diffs
   52 fields against the manifest. **A single byte of drift = RED.**
3. **CRASH** — shard artifacts + litmus results audited; any FAILED /
   pass=false / crash signature is a finding.

And the discipline that makes it a guardian: the **selftest poisons it** — a
3.41% drop fixture, a one-byte drift fixture (publishes.offset 8→9), and a
failed-shard fixture must each trigger a bite, while the healthy set passes.
**Guardian selftest PASS.** It already proved itself once: it flagged the
formal shard's own subshell path bug (a relative `tee` target landing inside
`formal/`) before any human noticed.

## Patch 11 — feat(ci): shard wiring + the nightly 10M tier

**Files:** `.github/workflows/extreme-test.yml`, `.github/workflows/nightly-deep.yml`

Four new PR shards (`chaos`, `chaos-parity`, `formal`, `guardian`) join the
extreme-test matrix; nightly gains `chaos-10m` (three 10,000,000-frame free
runs: fenced, seq_cst, wide-ring), `loom-bound3` (the preemption-bound-3
sweep), and `formal-deep` (TLC at 3 readers / 4 frames / 3 deaths). All new
scripts carry `set -euo pipefail`; the silent-green audit passes.

## Patch 12 — docs(rfc): RFC-0011 + this document

**Files:** `rfcs/0011-exhaustive-state-space-proofs.md`, `patches/PATCHES-WAVE4.md`

## Gate summary (all green at packaging time)

| Gate | Result |
|---|---|
| chaos selftest (pinned vectors) | PASS |
| chaos stepped, both regimes, 3 shapes + ASAN | PASS |
| chaos free 2M (PR) / 10M × 3 (nightly) | PASS |
| chaos parity C==TS==JVM==Dart (byte-identical, 2 configs) | PASS |
| golden fixture reproduction | PASS |
| loom deep-models bound-2 (4 tests) | PASS (57.97s) |
| TLC FanoutSeqlock (19,443 states) | PROVEN — no error |
| TLC ReattachPolicy (615 states) | PROVEN — no error |
| guardian selftest (bites + healthy) | PASS |
| guardian wire watch (live probe, 52 fields) | PASS |
| litmus C/TS/Rust | PASS (24/24) |
| fanout-native (incl. F10 parity) | PASS |
| silent-green audit | PASS |
| `git am` round-trip | 12/12 clean on pristine base |

## Honest boundaries (Law 4)

- Swift engine: compiled + executed only in the Apple CI leg (no swiftc in
  the Linux sandbox) — the banner says exactly that, and the XCTest pins it
  to the golden fixture.
- TLC proofs are at the checked bounds (2–3 readers, 3–4 frames, 2 slots,
  2 words) — exhaustive AT those bounds, which is the mathematical claim;
  production scales remain the torture/chaos tiers' job.
- The free-running tier's fault SEQUENCE is deterministic; which wall-clock
  interleavings those faults land on is not — that is the tier's purpose.
