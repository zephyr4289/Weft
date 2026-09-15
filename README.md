# Weft Complete Repository — Sandbox v0.1

> **Single-zip extract-and-use repository** · 2026-09-12 · `x86_64-sandbox`
> All source, all reports, all directive packages, all build scripts, all audit logs.

## What this is

The complete Weft sandbox-buildable project, packaged as a single zip you can extract anywhere. After extraction, you have:

- The full Weft source tree (3-language kernel, litmus suite, benchmark suite, W-suite, tools, ports, docs)
- Every PDF report ever produced (whitepaper v1.0.0 through v1.0.4, all phase reports, all errata, all closure-batch evidence)
- Every directive package from staff (Phase 0 through Phase 5, including the round-5 verification record `WO-P5-VERIFICATION.md`)
- Every build/validation script used to produce the deliverables
- The multi-agent worklog (the full audit trail across all phases)
- The release tarball itself (`weft-sandbox-v0.1.tar.gz`)

## Directory layout

```
weft-complete-repo/
├── README.md                          ← this file
├── weft-sandbox-v0.1.tar.gz           ← the release tarball (also unpacked at weft/)
├── worklog.md                         ← multi-agent audit trail (all phases)
│
├── weft/                              ← the Weft source tree (extracted from tarball, with R1+R2 fixes)
│   ├── README.md                      ← project README
│   ├── INSTALL.md                     ← in-sandbox vs real-device toolchain split
│   ├── RELEASE-NOTES.md               ← release notes (the one kernel delta)
│   ├── SHA256SUMS                     ← sha256 of all reports/ PDFs
│   ├── Makefile                       ← 5 targets: litmus, bench, site, validate, build
│   ├── 05-CONTRACTS.md                ← Phase 0 contracts (preserved from directive)
│   │
│   ├── core/                          ← kernel implementations (FROZEN)
│   │   ├── c/                         ← C kernel + litmus runner + bench runner
│   │   ├── rust/                      ← Rust kernel + litmus + bench + probe + record
│   │   ├── ts/                        ← TypeScript kernel + litmus + bench
│   │   ├── kotlin/                    ← Kotlin/Android port (source-only, structurally validated)
│   │   ├── swift/                     ← Swift/iOS port (source-only)
│   │   └── dart/                      ← Dart/Flutter port (source-only)
│   │
│   ├── heddles/                       ← TypeScript Heddle bindings (react, svelte, vue, react-native)
│   ├── litmus/                        ← L1–L8 litmus suite + REPORT.md + evidence/
│   ├── bench/                         ← B-suite + W-suite + thermal + site + workloads/
│   ├── tools/                         ← litmus_driver, bench_driver, port_validator, make_site,
│   │                                      weft-probe, weft-record, FORMATS.md
│   ├── docs/                          ← WHITEPAPER.md (v1.0.4 canonical), ERRATA, PORTS, etc.
│   └── reports/                       ← all PDFs (also at top-level reports/)
│
├── reports/                           ← all PDFs + markdown sources (top-level copy for convenience)
│   ├── Weft-Whitepaper-v1.0.4.pdf     ← CANONICAL whitepaper (16pp, sha256 6f3a3211)
│   ├── Weft-Whitepaper-v1.0.3.pdf     ← historical (rebuilt impostor; P5-W1 deviation)
│   ├── Weft-Whitepaper-v1.0.2.pdf     ← historical (B1 first repair)
│   ├── Weft-Whitepaper-v1.0.1.pdf     ← historical (A1–A3 amendments)
│   ├── Weft-Phase5-Release-Report-v1.0.1.pdf  ← LATEST release report (R5 re-typeset)
│   ├── Weft-Phase5-Release-Report.pdf ← original v1.0 (superseded by v1.0.1)
│   ├── Weft-Phase5-C2-Soak-Evidence.pdf       ← B2 soak evidence (World A confirmed)
│   ├── Weft-Phase4-Errata.pdf                 ← original Phase 4 errata
│   ├── Weft-Phase4-Errata-R4-Correction-Slip.pdf  ← R4 slip (B3 misattribution fix)
│   ├── Weft-Phase4-Ports-Report.pdf           ← Phase 4 ports report
│   ├── Weft-Phase2-Tools-Report.pdf           ← Phase 2 tools report
│   ├── Weft-Phase1-Implementation-Report.pdf  ← Phase 1 vertical slice
│   ├── Weft-Triad-Spike-Report.pdf            ← Phase 0 spike report
│   ├── Weft-Sandbox-Roadmap.pdf               ← sandbox-constrained roadmap
│   └── Weft-Specification-v0.1.pdf            ← founding spec (superseded by WHITEPAPER.md)
│
├── directives/                        ← all staff directive packages (the audit trail)
│   ├── weft-docs-founding/            ← founding architecture docs (ROADMAP, ARCHITECTURE, etc.)
│   ├── weft-phase0-directive/         ← Phase 0 (kernel + litmus)
│   ├── weft-phase0-directive-v1.1/    ← Phase 0 v1.1 (catalog min_claims floor added)
│   ├── weft-phase0-directive-v1.2/    ← Phase 0 v1.2
│   ├── weft-phase2-directive/         ← Phase 2 (tools)
│   ├── weft-phase3-directive/         ← Phase 3 (whitepaper)
│   ├── weft-phase4-directive/         ← Phase 4 (ports)
│   └── weft-phase5-directive-r5/      ← Phase 5 (release) + all verification records:
│                                          WO-P4-C1-VERIFICATION.md (round 3 FAIL)
│                                          WO-P4-C1R-VERIFICATION.md (round 4 ACCEPT)
│                                          WO-P5-VERIFICATION.md     (round 5 CONDITIONAL)
│
└── scripts/                           ← all build/validation scripts
    ├── build_whitepaper.py            ← typeset WHITEPAPER.md → PDF (XeTeX-native)
    ├── scan_whitepaper.py             ← forensic overflow scan at 611.5pt threshold
    ├── soak_b2.py                     ← B2 soak evidence collector
    ├── package_c2_evidence.py         ← C2 delivery PDF builder
    ├── build_p4_errata.py             ← Phase 4 errata PDF builder
    ├── build_phase5_release_report.py ← original release report builder
    ├── build_phase5_release_report_r5.py  ← R5 re-typeset (v1.0.1) builder
    ├── clean_tree_validation.py       ← original clean-tree validator
    ├── clean_tree_validation_r3.py    ← R3 full-logs clean-tree validator
    ├── thermal_proxy.py               ← T3 thermal-proxy runner
    ├── weft-spike/                    ← Phase 0 C spike source
    ├── weft-android/                  ← Phase 1 Android vertical slice (Rust + Kotlin)
    ├── weft_body.py, weft_merge.py    ← Phase 0 founding-spec PDF builders
    ├── weft_phase1_report.py          ← Phase 1 report builder
    ├── weft_sandbox_roadmap.py        ← sandbox roadmap PDF builder
    └── weft_spike_report.py           ← spike report PDF builder
```

## Quick start

After extracting this zip:

```sh
cd weft-complete-repo/weft

# Verify the in-sandbox toolchain (per INSTALL.md):
#   gcc 14+, rustc 1.7x+, node 24+, python3 3.12+
# (Rust install: curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh)

# Run all five make targets:
make all       # litmus + bench + site + validate + build

# Or individually:
make litmus    # 24-cell litmus matrix (or EXPOSURE-SHORTFALL RED per R2)
make bench     # B-cells (canonical 16b5c663) + W-cells (separate bundle)
make site      # regenerate static HTML site (deterministic)
make validate  # 4/4 port targets exit 0
make build     # C + Rust + TS builds
```

Expected outcomes (per R3 clean-tree re-validation):

- `make litmus`: 23/24 cells PASS; ts/L1-tear EXPOSURE-SHORTFALL RED (ratified honest outcome per R-B; the catalog-amendment evidence path is now open via the R2 telemetry fix)
- `make bench`: PASS — `bench/results.json` sha256 = `16b5c663` exactly (R1 Path C verified)
- `make site`: PASS — deterministic, sha256 `88f02947...` byte-identical on re-render
- `make validate`: PASS — 4/4 targets green (61 structural checks)
- `make build`: PASS — C, Rust, TS kernels compile clean

## Canonical artifacts

| Artifact | Path | sha256 |
|---|---|---|
| Final tarball | `weft-sandbox-v0.1.tar.gz` | `62e8c5c2...` (full: `62e8c5c2953fc03a74c69064a7eb084024a56e5f1ce955060dea4c9dce75ba7b`) |
| Canonical whitepaper | `reports/Weft-Whitepaper-v1.0.4.pdf` | `6f3a3211...` (full: `6f3a321142ce2ab126794d2ed55057e42c328eb394c45d5e6fae5056c3241484`) |
| Canonical B-suite bundle | `weft/bench/results.json` | `16b5c663...` (full: `16b5c663433a37540c77f9dd6e4b83abe3eed929cc4c142813ad0b684f5498f8`) |
| Latest release report | `reports/Weft-Phase5-Release-Report-v1.0.1.pdf` | `54eb4499...` (full: `54eb4499a7e3238339f2d3c1c80ade98e5828afc6c6b0ae9246efd42986c0534`) |

## Phase status (after R1–R6 repair batch)

- Phase 0 / 0.5 / 1 (kernel, litmus, bench): **CLOSED**
- Phase 2 (Tools): **CLOSED** at C2 (World A confirmed)
- Phase 3 (Whitepaper): **CLOSED** at C1r (v1.0.4 canonical)
- Phase 4 (Ports): **CLOSED** at C3+R4 (errata slip delivered)
- Phase 5 (Sandbox release): **architecture ACCEPTED**, closure CONDITIONAL on R4 unblock (staff-provided original v1.0.3) + senior round-6 verification

## Open deviations (per latest release report)

- **D-T7-3 (OPEN, BLOCKED)**: tarball's `v1.0.3.pdf` is a rebuilt impostor (`4db9b508...`), not the senior's adjudicated FAIL artifact (`7f546cb1...`, 93,076 bytes, 15pp). Senior said staff would place the original at `download/staff-provided/Weft-Whitepaper-v1.0.3.pdf` for the executor to copy — **file not in uploads as of R6 delivery**. Awaiting staff file placement.
- All other deviations (D-T7-1, D-T7-2, D-T7-4, D-T0-1, D-T0-2) are CLOSED or FIXED — see `reports/Weft-Phase5-Release-Report-v1.0.1.pdf` §8 for the full ledger.

## Honesty

- All measured numbers carry `[MEASURED x86_64-sandbox]`
- Structural gates are normative; telemetry is advisory (AXIOM T)
- Verification claims name scan scope + numeric threshold (per WO-P4-CLOSURE P4-W1 standing rule addendum)
- Quantitative misses are declared (per WO-P5-RELEASE §2 rule 7)
- Real-device verification is Phase 6+; this is the sandbox endpoint

## License

See `weft/LICENSE` (or the charter referenced in `weft/docs/WHITEPAPER.md` §11).
