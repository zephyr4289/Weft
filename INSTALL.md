# Weft Sandbox v0.1 — Install Guide

> Per WO-P5-RELEASE §1.T6 + §5b. In-sandbox vs real-device split.

## 1. In-sandbox toolchain (verified)

The sandbox-built project runs end-to-end on the following toolchain. The
Phase 5 release was built and verified with these versions.

| Tool | Version (sandbox baseline) | Used by |
|---|---|---|
| gcc | 14.2.0 (Debian) | C kernel + record/probe tools |
| rustc | 1.98.1 (48a229cea 2026-09-01) | Rust kernel + record/probe |
| cargo | 1.98.1 (797e8a9bc 2026-08-05) | Rust build |
| node | v24.19.0 | TS kernel (type-stripping) |
| python3 | 3.12.14 | W-suite + bench_driver + site + validators |
| tectonic | 0.15.0 | Whitepaper PDF compilation (XeTeX) |
| pandoc | 3.1.11.1 | Markdown → LaTeX for whitepaper |
| PyMuPDF (fitz) | 1.26.7 | Forensic overflow scan (611.5pt threshold) |

To install Rust (if not already present):

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal
source $HOME/.cargo/env
```

## 2. Build & verify

Unpack the tarball into an empty directory, then:

```sh
cd weft-sandbox-v0.1

# All five make targets in sequence:
make litmus      # Phase 0 — 24 cells, exit 0
make bench       # Phase 1 + 5 — B-cells (canonical) + W-cells (separate bundle)
make site        # Phase 5 — static HTML site regeneration (deterministic)
make validate    # Phase 4 — port structural validation, 4/4 exit 0
make build       # All language builds (C, Rust, TS)

# Or: make all runs all five in order.
```

Expected outputs (see Reproducibility page of `bench/site/` for the full
honesty discussion):

- `litmus/REPORT.md` — 24-cell matrix (8 tests × 3 languages × 2 build modes), all green
- `bench/results.json` — canonical B-suite bundle (sha256 `16b5c663` — the whitepaper's pinned source)
- `bench/results/wsuite-x86_64-sandbox.json` — W-suite results (20 cells)
- `bench/results/wsuite-thermal-x86_64-sandbox.json` — thermal proxy curves (4 backends)
- `bench/site/index.html` + 3 more — static HTML site
- `litmus/evidence/ports/validation-results.json` — 4/4 port targets exit 0

## 3. NOT in-sandbox — Phase 6+ dependencies

The following toolchains are NOT installed in-sandbox and are required only
for real-device verification (Phase 6+):

| Toolchain | Why | Phase |
|---|---|---|
| JDK + Android SDK | Kotlin/Android compile + instrumented tests | Phase 6+ |
| Xcode + iOS SDK | Swift/iOS compile + device tests | Phase 6+ |
| Flutter SDK | Dart/Flutter compile + device tests | Phase 6+ |

The Kotlin, Swift, and Dart source files are shipped in `core/{kotlin,swift,dart}/`
with STATUS banners indicating "source-only, unbenchmarked." Per
`WO-P5-RELEASE §6 Out of scope`: device matrix and real UI stacks
(A/B device implementations) are Phase 6+.

## 4. Fresh-machine criterion (in-sandbox proxy)

Per `WO-P5-RELEASE §0 decision 6`: "Builds cleanly on a fresh Linux machine"
is tested in-sandbox as: unpack the tarball into an empty directory, run all
five make targets, log results. The literal fresh-machine claim stays honest
in this INSTALL.md: verified in-sandbox on the equivalent of a clean tree;
contributor confirmation on foreign hardware is the Phase 6+ / post-release
loop.

## 5. Files in this tarball

See `SHA256SUMS` for the full file listing with hash verification.
