# PR-PILLAR8 — weft-verify: formal proofs, static alloc linter & safety anchors

**Branch:** `feat/weft-verify-core` (5 commits, DCO-signed, off `main` @ `debf32a`)
**Pillar:** 8 — weft-verify (Formal Proofs, Static Alloc Linter & Formal Safety Anchors)
**Territory (respected, 0-diff outside):** `formal/`, `tools/weftc/lint/`, `core/c/verify/`, `tests/verify/core/`, `tools/verify/`, `docs/reports/D-81-FORMAL-VERIFICATION-AUDIT.md`

## What lands

1. **TLA+ proofs, TLC-verified (pinned tla2tools 1.8.0, sha256-checked):**
   - `formal/seqlock_ring.tla` — the 64B/128B seqlock ring protocol at
     micro-step granularity. `NoTornReads` + `SeqParity` safety and the
     `ReaderNeverBlocksWriter` liveness property under **writer-only**
     weak fairness (readers may crash). Exhaustive CI tier: 6,676,860
     distinct states, zero errors; full tier: 16,062,820 distinct states
     explored (mandate: ≥ 10⁷), no explosion, no deadlocks.
   - `formal/wcr1_consensus.tla` — lease renewal, heartbeats, elections
     through the message plane, one-link-flap partitions, gossip-on-heal.
     `SinglePrimary` (per-epoch, quorum intersection), `QuorumPromise`,
     `HealFloor` (monotonic epoch advancement). Exhaustive: 8,035,488
     distinct states (68.6M generated), zero errors, elections
     non-vacuously firing.

2. **`weftc --lint-alloc`** (`tools/weftc/lint/`) — Law 1 proven at
   compile time across C / C++ / Rust / TypeScript / Swift / Dart:
   malloc family, new/delete, mmap, strdup/asprintf, dynamic string
   concatenation & interpolation, unbounded recursion (self + Tarjan
   SCC cycles), Rust std idioms. GCC-format diagnostics with remediation
   notes; JSON mode; fail-closed exit codes. **SLA: 431,775 AST nodes
   scanned in 1.42 ms (3.3 ns/node) against the <15 ms budget; the scan
   pass performs zero heap allocations (proven by G4 malloc
   interposition).**

3. **Formal safety anchors** (`core/c/verify/`, frozen ABI v1.0) —
   `WEFT_STATIC_ASSERT` / `WEFT_BOUNDS_CHECK` / `WEFT_PROVE_ALIGNED`
   compile-time proofs plus zero-overhead runtime twins for ring
   indices, ring spans, variable-length-field offsets and DMA strides
   (overflow-safe by construction). 10,000,000-cycle differential stress
   vs an independent reference: 0 mismatches. Cache-line-aligned atomic
   violation ledger. `WEFT_VERIFY_DISABLE` release mode folds every
   guard to constants.

4. **Fail-closed G1–G6 gate suite** (`tools/verify/tests/`) —
   dual-compiler strict builds, pinned TLC + C trace-compliance oracle
   (state-count parity with TLC on both models), linter
   precision/recall (100% / 0 FP), zero-alloc probes, ASan+UBSan, ABI
   freeze from C++17 under both compilers.

## The interesting part: the oracle caught a real consensus defect

The first WCR1 draft stamped a new primary's lease with the candidate's
*current* promise rather than the epoch its quorum actually promised. A
candidate that votes for a peer mid-campaign raises its own promise; the
unstamped variant then lets two actives share a lease epoch — the C
oracle found the violating states (first at state #80,362) and the fix
(`candEpoch`) is now part of both the TLA spec and the oracle, with the
defect recorded in the D-81 ledger (§7, D-1..D-14). Also documented: a
SANY silent-error-recovery incident that only state-count parity
between two independent provers exposed (D-14).

## Evidence

`tools/verify/evidence/` — per-gate logs + `SUMMARY.txt`
(`ALL-GATES-PASS`). Full analysis: `docs/reports/D-81-FORMAL-VERIFICATION-AUDIT.md`.

## Reviewer shortcuts

- Spec readability: `formal/seqlock_ring.tla` header comment + invariants.
- Linter behavior: `tools/weftc/lint/README.md` (rule table + FP ledger).
- Gate mechanics: `tools/verify/tests/run_verify_core_suite.sh`.
- Everything measured, with numbers: `D-81` §3–§7.
