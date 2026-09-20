# Weft CI — Extreme Testing Suite

> **Parallel test matrix + performance gate + report persistence to `ci-report` branch.**
> Adapted from the founding extreme-test strategy for Weft's 3-language kernel surface.

## What this CI does

Every push to `main`/`develop` and every PR to `main` triggers the **Extreme Test Matrix** workflow (`.github/workflows/extreme-test.yml`), which runs **14 parallel shards**:

| Shard | What it tests | Gate |
|---|---|---|
| `build` (gatekeeper) | Compiles C + Rust + TS kernels first | Hard fail = workflow stops |
| `codeql-analysis` | Static security + quality on C + Python | Hard fail |
| `litmus-c` | L1–L8 litmus suite, C kernel | Hard fail (or `EXPOSURE-SHORTFALL`) |
| `litmus-rust` | L1–L8 litmus suite, Rust kernel | Hard fail |
| `litmus-ts` | L1–L8 litmus suite, TS kernel (with EXPOSURE-RETRY) | Hard fail (or `EXPOSURE-SHORTFALL`) |
| `bench-b-c` | B1–B5 benchmarks, C kernel | Hard fail |
| `bench-b-rust` | B1–B5 benchmarks, Rust kernel | Hard fail |
| `bench-b-ts` | B1–B5 benchmarks, TS kernel | Hard fail |
| `wsuite` | W1–W5 × A/B/C/D = 20 cells, P99 + alloc assertions | Hard fail (alloc==0 for C+D) |
| `thermal-proxy` | W2 × 4 backends × 120s sustained, decay curves | Advisory (never blocks) |
| `tools-interop` | C→C, C→Rust, Rust→Rust, Rust→C record/replay | Hard fail |
| `ports-validate` | Kotlin + Swift + Dart + TS heddles structural validator | Hard fail (4/4 targets) |
| `site-determinism` | `make site` double-render, byte-identical sha256 | Hard fail |
| `forensic-scan` | PAST-CROPBOX scan at 611.5pt threshold on all PDFs | Hard fail (0 flags) |
| `canonical-audit` | `bench/results.json` sha256 = `16b5c663` (R1 Path C invariant) | Hard fail |
| `weftc-codegen` | Project weftc Pillar 1 native codegen (C11/Rust/WGSL/GLSL): generator determinism, committed goldens, Law-4 GPU-refusal matrix, C roundtrip x3 build flavors (static ABI asserts, cast refusals, misaligned packed IO, 65536-pattern f16 identity, SIMD batch), Rust no_std compile + C→Rust→C bit-exact stage bins, WGSL/GLSL double-entry vs C `offsetof`, glslang SPIR-V leg | Hard fail |
| **`perf-regression`** | **W-suite P99 vs pinned baseline; >15% drop = FAIL** | **Hard fail** |

After all shards finish, the **aggregation job** downloads every shard's log + results JSON, computes the overall status, and commits the full report to the **`ci-report` branch** under `runs/run-<NNNNN>/`.

## The performance gate (key feature)

The `perf-regression` shard is what makes this CI "extreme": **committed code can never silently degrade performance.**

1. The W-suite (5 workloads × 4 backends = 20 cells) measures P99 FPS for each cell.
2. Each cell's P99 is compared against the pinned baseline in `ci/baselines/wsuite-p99-baseline.json`.
3. If any cell drops **more than 15% below baseline**, the shard FAILS and the workflow blocks the merge.
4. The PR gets a comment with a regression table (workload, backend, baseline, actual, Δ%).
5. To update the baseline (e.g., after an intentional protocol change that legitimately trades P99 for memory): add the **`perf-baseline-update`** label to the PR. When the PR merges, the `.github/workflows/update-perf-baseline.yml` workflow runs the W-suite with longer measure windows (10s vs 5s) and commits the new baseline to `main`.

This means: **every perf change is intentional, attributed, and reviewed.** No silent regressions.

## Nightly deep matrix

The `.github/workflows/nightly-deep.yml` workflow runs at 00:00 UTC daily and includes the expensive, flakiness-exposing tests that don't make sense on every push:

| Job | What it does |
|---|---|
| `litmus-stability` | Litmus × 5 iterations per language (catches flaky `EXPOSURE-SHORTFALL`s) — PASS iff ≥80% pass rate |
| `thermal-long` | 10 min per backend × 4 backends (vs 2 min on push) — advisory |
| `tsan-deep` | C kernel with `-fsanitize=thread`, all 8 litmus tests × 5 iterations = 40 runs |
| `clean-tree-deep` | Build tarball from current commit, unpack into empty dir, run all 5 make targets + determinism + canonical hash check |

Nightly reports commit to `ci-report` branch under `runs/nightly-<YYYY-MM-DD>-<NNNNN>/`.

## How to pull and analyze reports

The `ci-report` branch is a standalone audit trail — it contains only CI run artifacts, not source code. To pull and analyze:

```sh
# Fetch the ci-report branch
git fetch origin ci-report:ci-report

# Checkout into a separate worktree (don't pollute your source tree)
git worktree add ../weft-ci-reports ci-report
cd ../weft-ci-reports

# List all runs (newest at the bottom)
ls runs/

# Read the latest run's combined log
cat latest.log | less

# Read the latest run's structured summary
python3 -c "import json; print(json.dumps(json.load(open('latest-summary.json')), indent=2))"

# Find all FAILED runs
grep -l '"overall_status": "FAILED"' runs/*/summary.json

# Compare a specific shard across runs
for d in runs/run-*/shards/shard-perf-regression.log; do
  echo "=== $d ==="
  grep -A 1 "Perf-regression status" "$d" | head -2
done

# Pull just the latest nightly
ls runs/nightly-*/
cat nightly-latest.log | less
```

### Report directory structure (per run)

```
runs/run-00123/
├── combined.log              ← all shard logs concatenated, with headers
├── summary.json              ← structured overall status + per-shard results
├── build.log                 ← gatekeeper build log
├── shards/
│   ├── shard-litmus-c.log
│   ├── shard-litmus-rust.log
│   ├── shard-litmus-ts.log
│   ├── shard-bench-b-c.log
│   ├── shard-bench-b-rust.log
│   ├── shard-bench-b-ts.log
│   ├── shard-wsuite.log
│   ├── shard-thermal-proxy.log
│   ├── shard-tools-interop.log
│   ├── shard-ports-validate.log
│   ├── shard-site-determinism.log
│   ├── shard-forensic-scan.log
│   ├── shard-canonical-audit.log
│   └── shard-perf-regression.log
└── results/
    ├── shard-litmus-c-results.json
    ├── shard-litmus-rust-results.json
    ├── shard-litmus-ts-results.json
    ├── shard-bench-b-c-results.json
    ├── shard-bench-b-rust-results.json
    ├── shard-bench-b-ts-results.json
    ├── shard-wsuite-results.json
    ├── shard-thermal-proxy-results.json
    ├── shard-tools-interop-results.json
    ├── shard-ports-validate-results.json
    ├── shard-site-determinism-results.json
    ├── shard-forensic-scan-results.json
    ├── shard-canonical-audit-results.json
    └── shard-perf-regression-results.json
```

The root of the `ci-report` branch also has:
- `latest.log` — copy of the most recent run's combined log
- `latest-summary.json` — copy of the most recent run's structured summary
- `nightly-latest.log` — copy of the most recent nightly run's combined log
- `nightly-latest-summary.json` — copy of the most recent nightly run's structured summary

## CI scripts reference

All scripts live in `ci/scripts/`:

| Script | Used by | What it does |
|---|---|---|
| `run_litmus_shard.sh <lang>` | `extreme-test.yml` (litmus-c/rust/ts shards) | Run litmus for one language; emit results JSON |
| `run_bench_shard.sh <lang>` | `extreme-test.yml` (bench-b-c/rust/ts shards) | Run B-suite for one language; canonical bundle isolation |
| `run_wsuite_shard.sh` | `extreme-test.yml` (wsuite shard) | Run all 20 W-suite cells; emit per-cell P99 + alloc |
| `run_thermal_shard.sh <secs>` | `extreme-test.yml` (thermal-proxy shard) | Run thermal proxy for N seconds × 4 backends |
| `thermal_proxy_inline.py` | `run_thermal_shard.sh` | Inline thermal proxy (no external dep) |
| `run_tools_interop_shard.sh` | `extreme-test.yml` (tools-interop shard) | 4 record/replay interop combos |
| `run_ports_validate_shard.sh` | `extreme-test.yml` (ports-validate shard) | 4/4 port structural validator |
| `run_site_determinism_shard.sh` | `extreme-test.yml` (site-determinism shard) | Double-render + sha256 compare |
| `run_forensic_scan_shard.sh` | `extreme-test.yml` (forensic-scan shard) | PAST-CROPBOX scan at 611.5pt on all PDFs |
| `run_canonical_audit_shard.sh` | `extreme-test.yml` (canonical-audit shard) | Verify `bench/results.json` sha256 = `16b5c663` |
| `run_perf_regression_shard.sh` | `extreme-test.yml` (perf-regression shard) | W-suite P99 vs baseline; >15% drop = FAIL |
| `aggregate_reports.py` | both workflows | Aggregate shard logs + compute overall status |
| `run_litmus_stability.sh <lang> <N>` | `nightly-deep.yml` | Litmus × N iterations; PASS iff ≥80% |
| `run_tsan_deep.sh <N>` | `nightly-deep.yml` | TSAN × N iterations × 8 tests |
| `run_clean_tree_deep.sh <tarball>` | `nightly-deep.yml` | Full clean-tree validation |

## Baseline file

`ci/baselines/wsuite-p99-baseline.json` pins the W-suite P99 values that the `perf-regression` shard compares against. The file is created automatically on the first CI run (no baseline exists yet → use this run's values). To update it intentionally, add the `perf-baseline-update` label to a PR — when the PR merges, the `update-perf-baseline.yml` workflow runs the W-suite with longer measure windows (10s) and commits the new baseline.

The baseline file format:

```json
{
  "cells": [
    {"workload": "W1", "backend": "A", "p99": 120.2},
    {"workload": "W1", "backend": "B", "p99": 140.5},
    {"workload": "W1", "backend": "C", "p99": 140.9},
    {"workload": "W1", "backend": "D", "p99": 87.5},
    ...
  ],
  "note": "Updated via PR #N (label: perf-baseline-update). Measure-s 10, warmup-s 2.",
  "updated_at": "2026-09-15T12:34:56Z",
  "updated_by": "zephyr4289",
  "pr_sha": "abc123..."
}
```

## First-run setup

After pushing these CI files to your repo:

1. **First push to main/develop** — the `extreme-test.yml` workflow runs. The `perf-regression` shard finds no baseline file → creates one from this run's W-suite output (status: PASSED, reason: baseline-initialized). The `ci-report` branch is created on the aggregation step.

2. **First nightly** — runs at 00:00 UTC. The `litmus-stability` and `tsan-deep` jobs may flag flaky cells; investigate and either fix or label `perf-baseline-update` if the new behavior is intentional.

3. **Pull the `ci-report` branch** to inspect reports:
   ```sh
   git fetch origin ci-report:ci-report
   git worktree add ../weft-ci-reports ci-report
   ```

## Honesty

- All measured numbers carry `[MEASURED x86_64-sandbox]` (GitHub Actions runs on `ubuntu-latest` = x86_64)
- Structural gates are normative; thermal is advisory; perf-regression is hard-fail
- Verification claims name scan scope + numeric threshold (per WO-P4-CLOSURE P4-W1 standing rule addendum)
- The CI cannot prove real-device performance — that's Phase 6+
