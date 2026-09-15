# Weft — The Continuous-State Plane for Declarative UI

> **Sandbox v0.1** · 2026-09-12 · `x86_64-sandbox` (runtime-verified)
> Per WO-P5-RELEASE. The sandbox-buildable project is complete at this phase.

## What this is

Weft is a protocol for shipping continuous, high-frequency state (audio
PCM, particle physics, streaming AI tokens, financial order books) to a
declarative UI without routing it through the reactive snapshot system.
The protocol — the Triad Protocol (RFC-0001, single atomic exchange per
publish/claim) — is the moat; this tarball is the sandbox-buildable
reference implementation.

## What's in the box

- **Kernel** (3 languages, FROZEN since Phase 0 + one semver-minor debug-view delta in Phase 2):
  - `core/c/` — C kernel (reference semantics)
  - `core/rust/` — Rust kernel (production target)
  - `core/ts/` — TypeScript kernel (web/RN)
- **Litmus suite** (`litmus/`): 8 tests × 3 languages × 2 build modes = 24 cells, all green
- **Benchmark suite** (`bench/`): 5 B-cell benchmarks × 3 languages = 15 cells; canonical bundle sha256 `16b5c663`
- **W-suite** (`bench/workloads/`): 5 workloads × 4 implementations (A=reactive naive, B=best practice, C=Weft ctypes, D=hand-rolled triple-buffer) = 20 cells
- **Tools** (`tools/`): `weft-probe` (state inspector), `weft-record` (capture/replay), `make_site.py` (static site), `litmus_driver.py`, `bench_driver.py`, `port_validator.py`
- **Ports** (`core/{kotlin,swift,dart}/` + `heddles/`): source-only, structurally validated, unbenchmarked
- **Docs** (`docs/`): WHITEPAPER.md (v1.0.4 canonical), ERRATA.md, PORTS.md, FORMATS.md, PHASE2-REPORT.md, PHASE4-REPORT.md
- **Reports** (`reports/`): all phase PDFs (whitepaper, spike, phase1-5 reports, errata, C2 evidence)
- **Site** (`bench/site/`): 4 static HTML pages, zero client-side JS, deterministic render
- **INSTALL.md, RELEASE-NOTES.md, SHA256SUMS, Makefile**: this directory

## Quick start

```sh
make all       # runs all five: litmus, bench, site, validate, build
```

Or individually:

```sh
make litmus    # 24 cells, exit 0
make bench     # B-cells (canonical 16b5c663) + W-cells (separate bundle)
make site      # regenerate static HTML
make validate  # 4/4 port targets exit 0
make build     # C + Rust + TS builds
```

See `INSTALL.md` for the in-sandbox vs real-device split and the
toolchain versions.

## Canonical artifacts

| Artifact | Path | sha256 (first 8) |
|---|---|---|
| Whitepaper (canonical) | `download/Weft-Whitepaper-v1.0.4.pdf` | `6f3a3211` |
| B-suite results (canonical) | `bench/results.json` | `16b5c663` |
| Phase 5 release report | `download/Weft-Phase5-Release-Report.pdf` | (see SHA256SUMS) |
| C2 soak evidence | `download/Weft-Phase5-C2-Soak-Evidence.pdf` | `7986c357` |
| Phase 4 errata | `download/Weft-Phase4-Errata.pdf` | `fba22c26` |

## Honesty

- All measured numbers carry `[MEASURED x86_64-sandbox]`
- Structural gates are normative; telemetry is advisory (AXIOM T)
- Verification claims name scan scope + numeric threshold
- Quantitative misses are declared
- Real-device verification is Phase 6+; this is the sandbox endpoint

## License

See `LICENSE` (or the charter referenced in `docs/WHITEPAPER.md` §11).
