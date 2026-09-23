# weftc-pillar8-managed — SCORECARD

**Pillar 8 — weft-verify** · Senior Engineer 3 (Managed Runtimes / Developer Tooling / Verification CLI & Scorecard)
**Verdict: DELIVERED — ALL 7 STAGES GREEN (exit 0)** · 2026-09-22

## What ships

| Item | Path | Status |
|------|------|--------|
| @weft/verify package (zero runtime deps, 99 KiB) | `packages/verify/` | ✅ Stage 7 |
| Unified CLI: `--all --lint-alloc --formal --chaos --report` | `packages/verify/src/cli.ts` | ✅ Stage 4 (16/16) |
| Cross-language zero-alloc linter (TS/C/C++/Rust/Swift/Dart) | `packages/verify/src/lint/` | ✅ Stage 2 (P=1.0, R=1.0) |
| Formal engine: 2 exhaustive models + 1e7 wide tally + TLA+ ref | `packages/verify/src/formal/`, `formal/TriadBuffer.tla` | ✅ Stage 5 (9/9 PROVED) |
| Deterministic chaos: thermal / bus / network + resilience | `packages/verify/src/chaos/` | ✅ (90/100) |
| Dark-theme deterministic HTML/JSON scorecards | `packages/verify/src/scorecard/` | ✅ Stage 5 (sha256-equal runs) |
| Git governance hooks (< 50 ms pre-commit) + installer | `tools/verify/hooks/` | ✅ Stage 6 (4 ms) |
| 7-stage fail-closed managed suite | `tools/verify/tests/run_verify_managed_suite.sh` | ✅ exit 0 |
| Poisoned/clean fixtures ×12 + expectation contract (31 rules) | `tests/verify/managed/fixtures/` | ✅ |
| Managed audit report | `docs/reports/D-83-VERIFY-MANAGED-AUDIT.md` | ✅ |

## Hard-gate scoreboard

| Gate | Mandate | Achieved |
|------|---------|----------|
| Linter precision / recall on poisoned fixtures | 100% | **1.00 / 1.00** (31/31) |
| Zero-alloc runtime probe (1,000,000 iters) | ≤ 64 KiB heap growth | **−0.4 KiB** (min-of-3; control bites) |
| State-space exploration tally | ≥ 1e7 states | **10,000,000** in 2.5 s (29.9M transitions) |
| Formal theorems | all PROVED | **9/9** (2 models exhausted: 6,336 + 5,760 states, 0 deadlocks) |
| Scorecard determinism | byte-identical reruns | **sha256-equal** (JSON + HTML) |
| Pre-commit hook latency | < 50 ms | **4 ms** best-of-3 |
| Package payload | < 100 KiB | **101,578 B (99 KiB)** |
| Runtime dependencies | 0 | **0** (imports: `node:` + relative only) |
| Boundary law (`core/c/`) | zero touches | **0 references, 0 payload paths** |
| CLI exit-code discipline | 0/1/2 fail-closed | **16/16 cases** |

## Formal Theorems HUD (all PROVED)

TH-01 single-writer-exclusivity · TH-02 writer-reader-slot-exclusion ·
TH-03 no-deadlock · TH-04 bounded-commit-progress≤10 · TH-05
bounded-read-progress≤10 · TH-06 parity-gated-publication · TH-07
bounded-clean-read≤8 · TH-08 bounded-writer-progress≤3 · TH-09
state-space-tally≥1e7

## How to verify locally

```sh
bash tools/verify/tests/run_verify_managed_suite.sh     # ~9 s, must exit 0
node packages/verify/bin/weft-verify.mjs --all          # full pipeline + scorecards
node packages/verify/bin/weft-verify.mjs --formal       # 9 theorems + 1e7 tally
```

Apply the series on a pristine `origin/main` checkout:
`git am weftc-pillar8-managed/patches/*.patch` (or `series.mbox`), then run the
suite. Full mandate matrix, benchmarks, 15-entry bug ledger and 12-point
honesty ledger: see `reports/D-83-VERIFY-MANAGED-AUDIT.md`.
