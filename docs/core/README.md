# Weft Complete Repository — Sandbox v0.1 + Extreme Testing CI

> **Single-zip extract-and-use repository** · 2026-09-15 · `x86_64-sandbox`
> All source, all reports, all directive packages, all build scripts, all audit logs,
> **plus the full extreme-testing CI suite**.

## What this is

The complete Weft sandbox-buildable project, packaged as a single zip you can extract anywhere. After extraction, you have:

- The full Weft source tree (3-language kernel, litmus suite, benchmark suite, W-suite, tools, ports, docs)
- **The extreme-testing CI suite** (3 GitHub Actions workflows + 15 parallel test shards + perf-regression gate)
- Every PDF report ever produced (whitepaper v1.0.0 through v1.0.4, all phase reports, all errata, all closure-batch evidence)
- Every directive package from staff (Phase 0 through Phase 5, including the round-5 verification record)
- Every build/validation script used to produce the deliverables
- The multi-agent worklog (the full audit trail across all phases)
- The release tarball itself (`weft-sandbox-v0.1.tar.gz`)

## Directory layout

```
weft-complete-repo/
├── README.md                          ← this file
├── weft-sandbox-v0.1.tar.gz           ← the release tarball
├── worklog.md                         ← multi-agent audit trail
│
├── weft/                              ← the Weft source tree (with R1+R2 fixes)
│   ├── .github/workflows/             ← **CI workflows (NEW)**
│   │   ├── extreme-test.yml           ← 14 parallel shards, runs on every push/PR
│   │   ├── nightly-deep.yml           ← nightly: 5× litmus, 10min thermal, TSAN, clean-tree
│   │   └── update-perf-baseline.yml   ← safety valve for perf-regression gate
│   ├── ci/                            ← **CI scripts + baselines (NEW)**
│   │   ├── README.md                  ← full CI documentation
│   │   ├── scripts/                   ← 15 shard runners + aggregator
│   │   └── baselines/wsuite-p99-baseline.json  ← pinned P99 baseline
│   ├── core/                          ← kernel (C, Rust, TS, Kotlin, Swift, Dart)
│   ├── heddles/                       ← TS Heddle bindings
│   ├── litmus/                        ← L1-L8 suite
│   ├── bench/                         ← B-suite + W-suite + thermal + site
│   ├── tools/                         ← drivers, validators, weft-probe, weft-record
│   ├── docs/                          ← WHITEPAPER.md (v1.0.4 canonical), ERRATA, PORTS
│   ├── reports/                       ← all PDFs (also at top-level reports/)
│   ├── Makefile + INSTALL.md + RELEASE-NOTES.md + SHA256SUMS + .gitignore
│   └── README.md
│
├── reports/                           ← all PDFs + markdown sources (top-level copy)
├── directives/                        ← all staff directive packages (audit trail)
└── scripts/                           ← all build/validation scripts
```

## Quick start

After extracting this zip:

```sh
cd weft-complete-repo/weft

# Verify the in-sandbox toolchain (per INSTALL.md):
#   gcc 14+, rustc 1.7x+, node 24+, python3 3.12+

# Run all five make targets locally:
make all       # litmus + bench + site + validate + build
```

## Push to GitHub + enable CI

```sh
cd weft-complete-repo/weft
git init
git remote add origin https://github.com/zephyr4289/Weft.git
git add .
git commit -m "Initial commit: Weft sandbox v0.1 + extreme testing CI"
git push -u origin main
```

On the first push, GitHub Actions will trigger the **Extreme Test Matrix** workflow (`.github/workflows/extreme-test.yml`). It runs 14 parallel shards:

| Shard | What it gates |
|---|---|
| `build` (gatekeeper) | Compiles all 3 kernels first |
| `codeql-analysis` | Static security + quality on C + Python |
| `litmus-c/rust/ts` | 8-test litmus suite per language (with EXPOSURE-RETRY) |
| `bench-b-c/rust/ts` | B1-B5 benchmarks per language (canonical bundle isolation) |
| `wsuite` | 20 W-suite cells; alloc==0 assertion for C+D |
| `thermal-proxy` | 4 backends × sustained FPS decay (advisory) |
| `tools-interop` | 4 record/replay combos (C→C, C→Rust, Rust→Rust, Rust→C) |
| `ports-validate` | 4/4 port structural validator |
| `site-determinism` | `make site` double-render byte-identical |
| `forensic-scan` | PAST-CROPBOX scan at 611.5pt on all PDFs |
| `canonical-audit` | `bench/results.json` sha256 = `16b5c663` |
| **`perf-regression`** | **W-suite P99 vs baseline; >15% drop = FAIL** |

After all shards finish, the aggregation job commits the full report to the **`ci-report` branch** under `runs/run-<NNNNN>/`.

## Pull CI reports for analysis

```sh
# Fetch the ci-report branch (created automatically on first CI run)
git fetch origin ci-report:ci-report

# Inspect in a separate worktree
git worktree add ../weft-ci-reports ci-report
cd ../weft-ci-reports

# Latest run's combined log
cat latest.log | less

# Latest run's structured summary
python3 -c "import json; print(json.dumps(json.load(open('latest-summary.json')), indent=2))"

# Find all FAILED runs
grep -l '"overall_status": "FAILED"' runs/*/summary.json
```

See `weft/ci/README.md` for the full CI documentation (every shard, every script, baseline update protocol).

## The performance gate (key feature)

The `perf-regression` shard is what makes this CI "extreme": **committed code can never silently degrade performance.**

1. W-suite (5 workloads × 4 backends = 20 cells) measures P99 FPS for each cell
2. Each cell's P99 is compared against the pinned baseline in `ci/baselines/wsuite-p99-baseline.json`
3. >15% drop = FAIL → workflow blocks the merge → PR gets a regression-table comment
4. To update the baseline intentionally (e.g., after a protocol change): add the **`perf-baseline-update`** label to the PR. When the PR merges, `update-perf-baseline.yml` runs the W-suite with longer measure windows and commits the new baseline to `main`.

**Every perf change is intentional, attributed, and reviewed. No silent regressions.**

## Nightly deep matrix

`.github/workflows/nightly-deep.yml` runs at 00:00 UTC daily with the expensive tests:

- `litmus-stability`: 5× iterations per language (catches flaky `EXPOSURE-SHORTFALL`s)
- `thermal-long`: 10 min per backend × 4 backends
- `tsan-deep`: C kernel with `-fsanitize=thread` × 5 iterations × 8 tests = 40 runs
- `clean-tree-deep`: build tarball, unpack into empty dir, run all 5 make targets

Nightly reports commit to `ci-report` branch under `runs/nightly-<YYYY-MM-DD>-<NNNNN>/`.

## Canonical artifacts

| Artifact | Path | sha256 |
|---|---|---|
| Final tarball | `weft-sandbox-v0.1.tar.gz` | `62e8c5c2...` |
| Canonical whitepaper | `reports/Weft-Whitepaper-v1.0.4.pdf` | `6f3a3211...` |
| Canonical B-suite bundle | `weft/bench/results.json` | `16b5c663...` |
| Latest release report | `reports/Weft-Phase5-Release-Report-v1.0.1.pdf` | `54eb4499...` |
| W-suite P99 baseline | `weft/ci/baselines/wsuite-p99-baseline.json` | (pinned from R3 clean-tree run) |

## Honesty

- All measured numbers carry `[MEASURED x86_64-sandbox]`
- Structural gates are normative; telemetry is advisory (AXIOM T)
- Verification claims name scan scope + numeric threshold
- The CI cannot prove real-device performance — that's Phase 6+
