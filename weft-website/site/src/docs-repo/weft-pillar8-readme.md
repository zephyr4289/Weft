# weftc-pillar8-core — Weft Pillar 8 delivery

**Pillar 8 — weft-verify: Formal Proofs, Static Alloc Linter & Formal
Safety Anchors.** Engineer 1 (Core / Formal Verification / Static
Analysis). Branch `feat/weft-verify-core`, 5 DCO-signed commits.

## Contents

```
formal/                  TLA+ truth (TLC 1.8.0, sha256-pinned)
  seqlock_ring.tla/.cfg/.full     64B/128B seqlock ring — PROVEN
  wcr1_consensus.tla/.cfg/.full   WCR1 lease consensus — PROVEN
core/c/verify/           frozen ABI v1.0 + zero-overhead bounds anchors
tools/weftc/lint/        weftc --lint-alloc (6 languages, zero-alloc scan)
tests/verify/core/       oracle / corpus / bounds / ABI test suites
tools/verify/            G1–G6 fail-closed runner + evidence/ + Makefile
docs/reports/D-81-FORMAL-VERIFICATION-AUDIT.md   the audit
PR-PILLAR8.md            pull-request narrative
patches/                 git format-patch series (applies clean on main)
```

## Headline numbers

| Proof / gate | Result |
|---|---|
| Seqlock ring, exhaustive (TLC) | 6,676,860 distinct states, 0 errors, liveness holds |
| Seqlock ring, full tier | 16,062,820 distinct states explored (≥ 10⁷ mandate) |
| WCR1 consensus, exhaustive (TLC) | 8,035,488 distinct states (68.6M generated), 0 errors |
| C oracle vs TLC | **state-for-state parity** on both models + 10M-step walks |
| Linter recall / false positives | 25/25 planted caught, 0 FP on clean corpus |
| Linter SLA | 431,775 nodes in 1.42 ms (3.3 ns/node; budget 15 ms) |
| Bounds stress | 10,000,000 cycles, 0 mismatches, 0 heap calls |
| Gate suite | G1–G6 ALL-PASS (`tools/verify/evidence/SUMMARY.txt`) |

## Run it

```bash
bash tools/verify/tests/run_verify_core_suite.sh    # ~15 min, TLC dominates
```

Requires gcc + clang (or `CC2=/path/to/clang`) + g++/clang++ + java 11+
(TLC jar auto-fetched, sha256-pinned).

A real WCR1 lease-stamping defect was caught by the Pillar 8 oracle
during development and is fixed in both provers — see D-81 §7.
