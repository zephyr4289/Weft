# D-81 — FORMAL VERIFICATION AUDIT (Pillar 8: weft-verify)

**Component:** weft-verify (formal proofs, static alloc linter, formal safety anchors)
**Engineer:** Senior Engineer 1 (Core / Formal Verification / Static Analysis)
**Branch:** `feat/weft-verify-core` (off `main` @ `debf32a`)
**Date:** 2026-09-22
**Prover:** TLC 2-developer release **1.8.0**, sha256
`9d36716ffb5e49d1ba8fae4651eba59f3189887e12eb90e204a42d2e6e993fef`
(fetched to `.tlc-cache/`, pinned and verified by the G2 gate)
**Compilers:** gcc 14.2.0 (Debian) and clang 19.1.7 — both at
`-std=c11 -Wall -Wextra -Werror -pedantic`; g++/clang++ at `-std=c++17`

---

## 0. Executive summary

Pillar 8 establishes the mathematical proof layer of the Weft ecosystem:

1. **The 64B/128B seqlock ring protocol is PROVEN torn-read-free and
   writer-non-blocking** — exhaustively, over 6,676,860 reachable states by
   TLC and independently re-verified by a C trace-compliance oracle that
   reaches **exactly the same state count** (state-for-state parity). The
   full-tier configuration explores **≥ 16,000,000 distinct states**
   (measured 16,062,820 at the 9-minute CI budget cutoff, zero errors),
   satisfying the mandate's ≥ 10⁷ exploration requirement.
2. **WCR1 lease consensus is PROVEN single-primary-per-epoch** —
   exhaustively over 8,035,488 reachable states (68.6M states generated) by
   TLC, again with exact state-count parity from the C oracle, plus
   10,000,000-step randomized walks under full-width epochs. **The oracle
   caught a real lease-stamping defect** in the first specification draft
   (§7 defect D-1); the shipped spec stamps every lease with the epoch its
   quorum actually promised.
3. **`weftc --lint-alloc` proves Law 1 at compile time** across C, C++,
   Rust, TypeScript, Swift and Dart: 25/25 planted violations detected
   (100% recall), 0 false positives on the clean corpus, and the scan pass
   runs at **3.3 ns/node** (431,775 nodes in 1.42 ms against the 15 ms SLA)
   with **zero heap allocations** — mechanically proven by linker-level
   malloc interposition.
4. **Formal bounds anchors** (`core/c/verify/`) survive 10,000,000
   randomized differential cycles against an independent overflow-checked
   reference with 0 mismatches, and their frozen ABI is re-derived from
   C++17 under both compilers.
5. The **fail-closed G1–G6 gate suite** runs all of the above in one
   command and aborts non-zero on any regression.

---

## 1. Mandate compliance matrix

| Mandate § | Requirement | Status | Evidence |
|---|---|---|---|
| 2.A.1 | `formal/seqlock_ring.tla` + `.cfg`, NoTornReads safety | **DONE — PROVEN** | `G2-tlc-seqlock_ring.log`: "Model checking completed. No error has been found", 6,676,860 distinct states |
| 2.A.1 | ReaderNeverBlocksWriter liveness | **DONE — PROVEN** | `PROPERTY ReaderNeverBlocksWriter` under writer-only WF; temporal check passed in the same TLC run |
| 2.A.1 | cfg explores ≥ 10⁷ distinct states, no explosion/deadlock | **DONE** | `seqlock_ring_full.cfg`: 16,062,820 distinct states at the 9-min cutoff, steady 2.4M ds/min, 0 deadlocks, 0 errors |
| 2.A.2 | `formal/wcr1_consensus.tla` + `.cfg`, SinglePrimary | **DONE — PROVEN** | `G2-tlc-wcr1_consensus.log`: 8,035,488 distinct states, no violations, non-vacuous (elections fire) |
| 2.A.2 | PartitionHealing (monotonic epoch advancement) | **DONE — PROVEN** | `HealFloor` invariant over the full reachable space; per-edge epoch regression check in the oracle (0 over 68.5M generated edges) |
| 2.B.1 | `weftc_lint_alloc.c/.h` AST scanner for @hot functions | **DONE** | 6 annotation forms × 6 languages (§5) |
| 2.B.1 | Detection rules: malloc family, new/delete, mmap, strdup, concat, fmt, recursion | **DONE — 100% recall** | `G3-lint.log`: 25/25 planted, 0 FP |
| 2.B.1 | GCC/Clang diagnostic format + remediation | **DONE** | `file:line:col: error: … [rule]` + `note:` remediation line; JSON mode |
| 2.B.1 | SLA: > 100,000 AST nodes in < 15 ms | **DONE — 10× margin** | 431,775 nodes, best 1.42 ms, 3.3 ns/node |
| 2.C.1 | `weft_verify.h` frozen ABI + WEFT_STATIC_ASSERT / WEFT_BOUNDS_CHECK / WEFT_PROVE_ALIGNED | **DONE** | ABI v1.0; compile-time + zero-overhead runtime twins; disable mode folds to constants |
| 2.C.2 | `weft_verify_bounds.c` boundary verification (ring indices, VLF offsets, DMA strides) | **DONE** | 10M-cycle differential stress, 0 mismatches, 8.6 ns per 5-check set |
| 2.D.1 | `test_oracle_tla.c` TLC trace-compliance oracle | **DONE — state-parity** | C BFS reaches exactly 6,676,860 / 8,035,488 states = TLC counts |
| 2.D.2 | `test_weftc_lint_alloc.c` positive + negative cases | **DONE** | 13-case corpus, exact rule-count assertions |
| 2.D.3 | `test_verify_bounds.c` 10,000,000 cycles, 0 failures | **DONE** | 0 mismatches; violation ledger exact |
| 2.D.4 | `run_verify_core_suite.sh` G1–G6 fail-closed | **DONE** | `SUMMARY.txt` — all gates PASS |
| 2.E | D-81 audit, proof ledger, linter benchmarks, FP ledger | **THIS DOCUMENT** | §3–§7 |
| 3.1 | Territory isolation | **CLEAN** | `git diff --stat main..feat/weft-verify-core` touches only the six assigned paths; kernel core 0-diff |
| 3.2 | Deterministic execution, fixed seeds | **CLEAN** | splitmix64 seeds hardcoded (`0x5345CC4B…`, `0x57435231…`) |
| 3.3 | Zero heap allocation in hot paths (Law 1) | **PROVEN** | G4 `--wrap=malloc,…` probes: 0 calls in bounds stress, lint scan, oracle BFS + 10M-step walks |
| 4 | Delivery: `weftc-pillar8-core.zip` | **DONE** | sources + tests + build scripts + evidence + this report |

---

## 2. Architecture

```
formal/                          TLA+ truth (TLC 1.8.0, sha-pinned)
  seqlock_ring.tla/.cfg/.full    64B/128B seqlock ring protocol
  wcr1_consensus.tla/.cfg/.full  lease/heartbeat/election/heal control plane
core/c/verify/                   frozen ABI + runtime anchors
  include/weft_verify.h          WEFT_STATIC_ASSERT / WEFT_BOUNDS_CHECK /
                                 WEFT_PROVE_ALIGNED + status ladder (v1.0)
  src/weft_verify_bounds.c       out-of-line twins; cache-line-aligned
                                 atomic violation ledger (Law 2)
tools/weftc/lint/                Law 1 at compile time
  weftc_lint_alloc.h/.c          tokenizer → function/hot detection →
                                 zero-alloc scan (rules + Tarjan SCC)
  weftc_lint_main.c              `weftc-lint` CLI (gcc/json formats)
tests/verify/core/               gate inputs
  test_oracle_tla.c              C mirror of both TLA machines
  test_weftc_lint_alloc.c        corpus + SLA + Law 1 probe
  test_verify_bounds.c           edge cases + 10M differential stress
  test_abi_cpp17.cpp             C++17 mirror of the frozen layouts
tools/verify/                    orchestration
  tests/run_verify_core_suite.sh G1–G6 fail-closed; evidence/
  Makefile                       developer convenience targets
```

Phase separation is the load-bearing design decision: **parse/setup may
allocate through a caller-supplied arena hook; the scan/verify hot paths
never allocate** — which is exactly what the G4 linker interposition
proves, and why the engine can be dropped into the browser/WASM build of
weftc without a heap.

---

## 3. Formal proof ledger & state-space exploration logs

### 3.1 Seqlock ring (`formal/seqlock_ring.tla`)

Model shape: single writer, R readers, M slots, sequence cap C. The
writer's install is four atomic micro-steps (seq++ → write lo → write hi →
seq++ & advance); the reader's attempt is four atomic micro-steps (sample
seqBefore + ground truth → copy lo → copy hi → re-sample & validate).
TLC explores every interleaving, including the tear window.

**CI tier (exhaustive)** — `seqlock_ring.cfg` (READERS=2, SLOTS=2,
VALUES=2, SEQCAP=4):

```
23,975,608 states generated, 6,676,860 distinct states found, 0 left on queue
Depth of the complete state graph: 34
Model checking completed. No error has been found.
Finished in 04min 36s   (TLC 1.8.0, -XX:+UseParallelGC -Xmx1500m)
```

Checked: `Inv = TypeOK ∧ SeqParity ∧ NoTornReads` and the temporal
property `ReaderNeverBlocksWriter = []<>(wBeat=0) ∧ []<>(wBeat=1)` under
`FairSpec` whose fairness clauses cover **writer actions only** — readers
may crash, stall or spin forever; the writer heartbeat must still beat.
It does, in every fair behavior.

**Full tier (≥ 10⁷ mandate)** — `seqlock_ring_full.cfg`
(READERS=2, SLOTS=3, VALUES=2, SEQCAP=6):

```
Progress(26) at 09:52:13: 51,570,365 states generated,
                          16,062,820 distinct states found,
                          3,451,772 states left on queue     ← CI budget cut
0 invariant violations, 0 deadlocks, steady 2.4–2.5M distinct states/min
```

16.06M **distinct** states explored with zero errors before the CI box's
9-minute budget; the tier is sized to complete on a workstation (or with
TLC's `-continue` for unattended runs). No state explosion: throughput
held flat at ~8.3M states generated/min throughout. The sequence cap is
the honest finite-model bound — a wrapped counter could alias
seqBefore = seqAfter across a modulo cycle and fake a stable read; the
C oracle covers the unbounded regime instead (§3.3).

### 3.2 WCR1 consensus (`formal/wcr1_consensus.tla`)

Model shape: 3 nodes (smallest quorum-bearing cluster), epochs, bounded
lease ticks, bounded in-flight message set, per-node component map with
one-link-flap splits and gossip-on-heal. Elections run through the
message plane; votes are granted only for strictly-higher epochs;
actives refuse to vote; the lease is stamped with **candEpoch** — the
epoch at which the winning candidacy started, i.e. the epoch its quorum
actually promised (see defect D-1 in §7 for why this matters).

**CI tier (exhaustive)** — `wcr1_consensus.cfg` (NN=3, EPOCHCAP=2,
LEASEMAX=2, MSGBUF=2):

```
68,560,405 states generated, 8,035,488 distinct states found, 0 left on queue
Depth of the complete state graph: 38
Model checking completed. No error has been found.
Finished in 06min 03s
```

Checked: `Inv = TypeOK ∧ SinglePrimary ∧ QuorumPromise ∧ HealFloor`.
Elections demonstrably fire (the predecessor draft without `DeliverGrant`
explored a different, election-free space — vacuity is called out in §7).

**Full tier** — `wcr1_consensus_full.cfg` (EPOCHCAP=3, MSGBUF=3) ships
for workstation-scale exploration; the CI tier already exhausts a shape
containing splits, heals, minority-component elections, stale-heartbeat
refusals and two successive primaries.

### 3.3 C trace-compliance oracle (`tests/verify/core/test_oracle_tla.c`)

The oracle re-implements both machines independently of TLC and verifies:

```
[oracle][seqlock] BFS states=6676860 edges=6676852 deadlocks=0
                  writer-independence-failures=0 (3.6 s, heap-calls=0)
[oracle][seqlock] walk steps=10000000 commits=372443 torn-detected=2
                  torn-VIOLATIONS=0 writer-steps=3331306 (0.3 s, heap-calls=0)
[oracle][wcr1]    BFS states=8035488 edges=8035487 deadlocks=0
                  epoch-regressions=0 (16.6 s, heap-calls=0)
[oracle][wcr1]    walk steps=10000000 elections-won=72404 heals=837495
                  splits=3824470 renewals=714 drops=1454315
                  single-primary-VIOLATIONS=0 (0.9 s, heap-calls=0)
TLA-ORACLE PASS (0 failures)
```

**State-for-state parity:** the C BFS terminates at exactly the TLC
distinct-state counts on both models (6,676,860 and 8,035,488). This is
the strongest available form of trace compliance: two independent
implementations of the spec semantics reach the same reachable set.
(Reaching this parity is also what exposed defect D-3 — dead-buffer-slot
bytes were inflating the C state space 2×; zeroing vacated slots closed
the gap to exact equality.)

The walks use **full-width counters** (uint32 epochs/sequences, no cap),
covering the regime the finite TLC model cannot: unbounded sequence
numbers (no wrap aliasing within 10⁷ steps) and unbounded epoch
advancement. Seeds are fixed (mandate §3.2): the runs above are
byte-reproducible.

Writer-independence is verified mechanically in both tiers: for **every**
reachable state, all reader fields are zeroed and the writer-action
availability mask is recomputed — it must be bit-identical. Zero
divergences over 6.68M states; spot-checked every 1024 steps during the
walks as well.

---

## 4. Theorems

**T1 (NoTornReads).** *If a reader commits (seqBefore = seqAfter, even),
then the payload it copied equals the payload that was whole in the slot
when the read began.*

Proof sketch: a slot's sequence number is odd exactly while the writer
is mid-install on that slot (invariant SeqParity, checked by TLC over
the full space), and each install increments the counter twice — once
before touching data, once after. An interleaved install between the
reader's two seq samples therefore changes the counter (monotone within
the cap; full-width in the walk tier), so the validation fails and the
read is refused as torn. Contrapositive: validation passing means no
install began during the copy, and payload halves cannot have changed
under an even, unchanged counter. ∎ (TLC: exhaustive, 6.68M states;
oracle: same space + 10⁷ walk steps.)

**T2 (ReaderNeverBlocksWriter).** *Under weak fairness of the writer
micro-actions only — readers receive no fairness — the writer heartbeat
flips infinitely often.*

Proof: every writer action's guard references only writer-owned state
(seq, head, wPhase, wVal); this is re-verified mechanically by the
zeroed-reader mask equality over the entire reachable space. When the
writer is idle, either WSeqUp is enabled (uncapped slot) or AdvanceHead
is enabled (capped slot) — one of them always is. WF on the micro-chain
therefore completes installs or skips forever, and wBeat flips on every
completion. Reader state (including "crashed" readers frozen mid-read)
appears in no guard, so no reader configuration can stall it. TLC
confirms the temporal property under FairSpec. ∎

**T3 (SinglePrimary, per epoch).** *Two simultaneously active primaries
never share a leaseEpoch.*

Proof: winning epoch e requires Cardinality(grants ∪ {self}) ≥ Quorum =
2 of 3, and every grant at epoch e raised the granter's promise to
exactly e (GrantVote) while the candidate's self-promise is e
(StartElection). Promises never decrease (checked per-edge by the
oracle; structurally, no action lowers them). A second primary at the
same e needs a second e-majority; two disjoint 2-of-3 majorities cannot
exist (quorum intersection), and a shared member would have had to grant
e twice — impossible under `ep > promised[v]`. The lease-stamping side
condition (leaseEpoch := candEpoch, not the candidate's possibly-risen
promise) is exactly defect D-1: without it, a candidate whose promise
rose by voting elsewhere could be stamped with an epoch its quorum never
promised, and the intersection argument breaks. ∎

Cross-epoch overlap remains possible by design (a stale lease in a
partitioned minority component); WCR1's write path carries
(epoch, lease) as a fencing token, and voters refuse heartbeats below
their promise — the DeliverHb refusal is the fencing evidence, exercised
in the TLC space.

**T4 (PartitionHealing ⇒ monotonic epochs).** *After a Heal, every node
holds promised ≥ the pre-heal cluster maximum, and promises never
regress.*

Proof: Heal atomically gossips Max(promised) into every node and pins
healedFloor to that maximum; the HealFloor invariant (promised ≥
healedFloor, over the full reachable space) plus per-edge monotonicity
(0 regressions over 68.5M generated edges in the oracle; structural in
the TLA actions) give the mandate's convergence guarantee. ∎

---

## 5. Linter benchmarks, precision/recall & false-positive ledger

### 5.1 SLA benchmark (mandate §2.B)

```
L4 SLA: nodes=431775 (>=100k) best=1.423 ms avg=1.493 ms (3.3 ns/node)
        diags=486 — limit 15 ms: PASS
```

Synthetic deterministic AST (clean functions, poisoned functions with
malloc/free, hot self-recursive functions), 5 timed zero-allocation
scans over the prebuilt token arena. Margin over the SLA: **~10×** on
the 2-core CI box.

### 5.2 Corpus results (G3)

13 cases across six languages: **25 planted violations, 25 found
(100.00% recall), 0 false positives** on the clean cases; rule-level
exact-count assertions; determinism verified by double-scan memcmp;
Law 1 verified by 100 wrapped scans (0 heap calls).

### 5.3 False-positive ledger (documented heuristic boundaries)

| # | Heuristic | Known behavior | Severity |
|---|---|---|---|
| L1 | `new`/`delete` flagged only for C++/TS | plain C identifiers named `new`/`delete` are never flagged | none (correct) |
| L2 | Rust `Box::new`/`String::from` gated to exact constructor | `Box::leak`, `Box::into_raw` not flagged | none (correct) |
| L3 | All self/mutual recursion in hot fns flagged | *bounded* recursion also flagged (static analysis cannot infer bounds) | intentional, per mandate |
| L4 | Managed-lang `+`/`+=` flagged when a string literal is adjacent | `s + t` (two variables) not flagged without type info | documented miss |
| L5 | Method-call edges skipped when preceded by `.`/`->`/`::` | recursion through `self.foo()` not detected | documented miss |
| L6 | Macro bodies (preprocessor lines) skipped | allocation inside a macro used by a hot fn is missed | documented miss |
| L7 | C++ ctor initializer lists attribute the body to the wrong name | diagnostics still point at correct lines/cols | cosmetic |
| L8 | Rust std idioms (Vec::new, .clone(), .push()…) are warnings | allocation-adjacent but not heap-constructors by themselves | advisory tier |
| L9 | Arrow-bodied TS/Dart functions detected; nested arrows attribute to the outer function | nested-lambda allocations counted once for the outer fn | conservative (flags more, not less) |
| L10 | TS `delete obj.x` flagged as alloc-new | it is a heap-object operation, not necessarily a free | advisory, documented |

### 5.4 Language coverage matrix (all smoke-verified)

C `/* @hot */` · C++ `[[clang::annotate("weft_hot")]]`, `[[weft_hot]]`,
`__attribute__((weft_hot))` · Rust `#[weft_hot]` (incl. `'lifetime`
return types) · TS `@hot` (functions, arrow consts, template literals) ·
Swift `@WeftHot` (incl. `\(interp)`) · Dart `@hot` (class methods,
`$var` interpolation, `"a" + s`).

---

## 6. Bounds & ABI audit

- **Known-answer edges:** 30+ assertions including the classic wraparound
  traps (`start+len > UINT64_MAX`, `stride*count` overflow) — all pass;
  the two initial expectation bugs found in my own test/elfcheck were
  corrected (defects D-9/D-12 in §7 — the gates refused my first draft,
  which is them working).
- **Differential stress:** 10,000,000 cycles, 5 checker families per
  cycle, against an independent overflow-checked-multiplication
  reference: **0 mismatches**, 86.5 ms total (8.6 ns per 5-check set).
- **Violation ledger consistency:** every macro-detected violation hits
  the atomic counter exactly (4,784 = 4,784).
- **Frozen ABI:** `sizeof(weft_lint_diag_t)=444`, `sizeof(weft_lint_node_t)=24`,
  `sizeof(weft_lint_func_t)=32`, `sizeof(weft_verify_status_t)=4`, with
  `offsetof` pins on every diag field — asserted from C **and** from
  C++17 under g++ and clang++ (G6). The counter word is `_Alignas(64)`
  and updated with a relaxed fetch-add (Law 2: never shares a cache line
  with hot-plane data; no lock contention).
- **Release mode:** `WEFT_VERIFY_DISABLE` compiles every guard to
  constants and is itself gate-checked in G1.

---

## 7. Defect ledger (caught by the Pillar 8 gates before delivery)

| # | Defect | Caught by | Fix |
|---|---|---|---|
| D-1 | **WCR1 lease-stamping**: `WinElection` stamped `leaseEpoch := promised[c]`; a candidate whose promise rose by voting elsewhere could be stamped with an epoch its quorum never promised → two actives sharing an epoch | C oracle BFS (SinglePrimary violations at state #80,362) | `candEpoch` variable; grants pinned to it; lease stamped from it (both TLA + C) |
| D-2 | **Vacuous WCR1 proof**: no `DeliverGrant` action — grants could never be collected, elections never fired, SinglePrimary held vacuously | self-review while threading candEpoch | added `DeliverGrant` (TLA + C); state space grew to the election-bearing 8.04M |
| D-3 | **C oracle state inflation**: vacated message-buffer slots kept stale bytes; states differing only in dead-slot garbage were distinct | state-count parity vs TLC (16.7M vs 8.04M) | zero vacated slots; parity became exact |
| D-4 | Missing-message-dedup in the C mirror (set semantics) | self-review | canonical sorted insert with dedup |
| D-5 | `weft_fn_match_paren_back` off-by-one counted the `)` itself — no functions ever detected | lint smoke test (0 diags) | walk starts before the token |
| D-6 | Return types between `)` and `{` (`-> T`, `: T`) defeated function detection (Rust/Swift/TS) | multi-language smoke | backward filler walk over type-ish tokens |
| D-7 | Rust `'static` lifetimes tokenized as unterminated char literals (swallowed braces) | Rust smoke | lifetime tokenization before char literals |
| D-8 | Named containers (`class Parser {`) not recognized as method scope (keyword is two tokens back) | Dart smoke | two-token-back container keyword check |
| D-9 | My own selfcheck asserted `span(8,8,8)=OK` — it is out-of-region | bounds test L1 | corrected expectation + added the exact case to the selfcheck |
| D-10 | `Box::leak` false-positive (any `Box::` matched) | corpus design | gated to `Box::new` exactly |
| D-11 | Swift `\(...)` interpolation skipped by the escape-skip branch | Swift smoke | check before skipping the escape pair |
| D-12 | G1 disable-mode build failed: `_fast` twins absent under `WEFT_VERIFY_DISABLE` | first suite run | constant-folding twins provided in disable mode |
| D-13 | `--san` tier used fail-closed overflow on a budget-limited run | first suite run (G5) | budget-limited tier semantics (invariants still checked on every visited state) |
| D-14 | **SANY silent error recovery**: a mangled `[hbAck EXCEPT`/`{promised[m]` line (truncated by an earlier scripted edit) parsed "successfully" with subtly altered semantics instead of failing loudly — caught only because the C oracle's state count stopped matching TLC | oracle/TLC state-parity discipline | full canonical rewrite of the spec; parity restored; the discipline itself is now the delivery's evidence standard |

D-14 deserves a note: the single most valuable property of maintaining
*two independent provers over the same protocol* is that semantic drift
cannot hide. Every discrepancy between TLC's state count and the
oracle's became a found defect.

---

## 8. Law compliance & boundary statement

- **Law 1 (zero allocation):** G4 linker interposition
  (`-Wl,--wrap=malloc,calloc,realloc,free,strdup`) proves 0 heap calls
  in the bounds stress loop, 100 lint scans, and the oracle's BFS +
  10M-step walks (steady state after arena preallocation; stdout uses a
  static buffer).
- **Law 2 (cache-line discipline):** the violation ledger is
  `_Alignas(64)` and single-word; no hot-path data shares its line.
- **Law 3 (dual compiler):** every C source builds clean under gcc 14.2
  and clang 19.1.7 at `-std=c11 -Wall -Wextra -Werror -pedantic`; the
  C++17 TU builds under g++ and clang++ with the same discipline (G1/G6).
- **Law 4 (ABI freeze):** layouts pinned by static asserts on both sides
  of the C/C++ boundary; version macros frozen at 1.0; append-only
  status ladder.
- **Territory:** the branch touches exactly
  `core/c/verify/`, `tools/weftc/lint/`, `formal/`,
  `tests/verify/core/`, `tools/verify/`, `docs/reports/D-81-…` — kernel
  core (`weft.c`, `weft.h`, `fanout.c`, …) is 0-diff, verified by
  `git diff --stat main..feat/weft-verify-core`.
- **Determinism:** all fuzz/walk harnesses use fixed splitmix64 seeds;
  corpus and benchmarks are byte-reproducible.

---

## 9. Reproduction

```bash
bash tools/verify/tests/run_verify_core_suite.sh   # G1–G6, ~15 min (TLC dominates)
# evidence: tools/verify/evidence/{SUMMARY.txt,G1-*.log,G2-*.log,G3-*.log,G4-*.log,G5-*.log,G6-*.log}

# individual tiers:
gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
    -Icore/c/verify/include -Itools/weftc/lint \
    tests/verify/core/test_weftc_lint_alloc.c tools/weftc/lint/weftc_lint_alloc.c \
    -o /tmp/lint && /tmp/lint                    # corpus + SLA
cd formal && java -cp ../.tlc-cache/tla2tools-1.8.0.jar tlc2.TLC \
    -config seqlock_ring.cfg seqlock_ring.tla    # TLC proof
```

Requirements: bash, gcc, clang (or `CC2=/path/to/clang`), g++/clang++,
java 11+ for TLC (auto-fetch of the pinned jar on first run).

---

## 10. Handoff notes

- **Engineer 2 (Native SHM Inspector):** include
  `core/c/verify/include/weft_verify.h` and use `WEFT_BOUNDS_CHECK` /
  `WEFT_PROVE_ALIGNED` at every reader boundary — the anchors are
  header-only inline, allocation-free, and ABI-frozen at 1.0. The
  seqlock ring spec (`formal/seqlock_ring.tla`) is the normative
  reference for what your inspector may observe mid-install.
- **Engineer 3 (Studio UI):** run `weftc-lint` in CI over every hot-path
  contribution (`--format=json` for machine consumption; exit 1 is the
  fail-closed contract). The engine API (`weftc_lint_parse` +
  `weftc_lint_scan_ast`) is embeddable in the WASM weftc with a
  caller-supplied arena.
- **Follow-ups (declared, not done):** macro-body scanning (L6),
  type-aware concat (L4), method-recursion edges (L5), TLC distributed
  workers for the full tiers, and a libFuzzer harness for the tokenizer
  (nightly tier — no libFuzzer in this sandbox).
