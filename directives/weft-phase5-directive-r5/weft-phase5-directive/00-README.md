# Weft Phase 0 — Execution Directive

**Package:** `weft-phase0-directive/v0.1` · **Issued by:** Architecture (staff++) · **Audience:** executing engineers
**Mission:** build Phase 0 of the Weft Sandbox-Constrained Roadmap — the corrected Triad kernel + the L1–L8 litmus suite, in C, Rust, and TypeScript, green in all 24 cells.

This package is **decision-complete**. Every design choice that would normally trigger a question has been made and written down. If you believe a decision is wrong, do **not** silently deviate: file an RFC amendment request against the advisory number and continue on the written path until it is answered.

---

## 0. Assignment brief (paste into the ticket)

> Implement the corrected Triad Protocol kernel per `02-KERNEL.md` and `03-ENVELOPE.md`, expose it through the API contract in `02` §4, and make the eight litmus tests in `04-LITMUS.md` pass in C, Rust, and TypeScript against the catalog/CLI/JSON contracts in `05-CONTRACTS.md`. Read `01-ADVISORIES.md` before writing any code; keep `06-PITFALLS.md` open while writing it. Definition of done is `07`-quality evidence in `litmus/REPORT.md` — 24 green cells × {debug, release} — no cell may be marked green on reasoning alone.

## 1. Package map — read in this order

| # | File | What it is | When to read |
|---|------|------------|--------------|
| 1 | `01-ADVISORIES.md` | Architecture review of the sandbox roadmap: 8 decisions already made, with rationale. **Binding unless vetoed.** | Before anything |
| 2 | `02-KERNEL.md` | The corrected Triad kernel: state, ownership argument, full API contract, memory-ordering matrix, I6 revocation handshake | Before writing the kernel |
| 3 | `03-ENVELOPE.md` | Tier-0 frozen 16-byte frame envelope, bit-exact, plus version negotiation | With `02` |
| 4 | `04-LITMUS.md` | L1–L8: exact procedures, measurement protocols, and mechanical verdict predicates. **This file is the product.** | Before writing the runner |
| 5 | `05-CONTRACTS.md` | Runner CLI, JSON verdict schema, `catalog.yaml` schema, `REPORT.md` format | With `04` |
| 6 | `06-PITFALLS.md` | Per-language traps, sandbox ground truth (verified), false-green/false-red hazards | While writing |
| 7 | `07-ACCEPTANCE.md` | Definition of done, evidence requirements, failure protocol, honesty labels | Before declaring done |

## 2. Execution order

```
Step 0  Environment gate (30 min)         — see 06-PITFALLS §1; Rust requires a rustup install
Step 1  litmus/catalog.yaml + validator   — 05-CONTRACTS §3     (~0.5 day)
Step 2  C kernel + C runner               — 02, 03, 04          (~2 days)
        Gate: L1–L8 green on C before ANY other language is started
Step 3  Rust kernel + runner              — mirror of Step 2    (~1.5 days)  ┐ parallelizable
Step 4  TypeScript kernel + runner        — mirror of Step 2    (~1.5 days)  ┘ by two engineers
Step 5  Driver + REPORT.md + evidence     — 05-CONTRACTS §4–5   (~0.5 day)
Step 6  Hardening pass                    — 06-PITFALLS §6 (TSAN column)  (~0.5 day)
```

Total: ~6 engineer-days for one developer; ~4 calendar days with two.

## 3. Repository layout to produce

```
weft/
├── litmus/
│   ├── catalog.yaml            # canonical test catalog (05 §3)
│   └── REPORT.md               # generated 24-cell matrix (05 §5)
├── core/
│   ├── c/                      # weft.h, weft.c, litmus_runner.c, Makefile
│   ├── rust/                   # Cargo.toml, src/lib.rs, src/bin/litmus.rs
│   └── ts/                     # weft.ts, litmus.ts, writer_worker.ts, package.json
├── tools/
│   ├── litmus_driver.py        # builds nothing; runs runners, assembles matrix
│   └── validate_catalog.py     # schema check for catalog.yaml
├── Makefile                    # build-c / build-rust / litmus / clean
└── README.md                   # what runs here, what is deferred, honesty labels
```

Kotlin/Swift/Dart are **out of scope** for Phase 0 (Phase 4, source-only, per roadmap §0.3). Do not scaffold them.

## 4. Rules of engagement

1. **The catalog is canonical.** Test parameters come from `catalog.yaml` via the driver. Runners must not hardcode parameters that the catalog carries.
2. **The protocol is frozen.** The single-exchange design in `02` is not a suggestion. No second atomic may guard buffer ownership. Any change = RFC amendment, not a local edit.
3. **A red litmus test is a discovery, not an obstacle.** If L1–L8 goes red, STOP, capture the repro (seed, params, interleaving evidence), and report. Do not add sleeps to make it pass. Do not widen a tolerance without an advisory.
4. **Honesty labels are mandatory.** Every number written anywhere is labeled `x86_64-sandbox`. Runtime-green is "runtime-verified under adversarial scheduling," never "proven." Formal proof (loom model check) is a follow-up, not a Phase 0 gate.
5. **A phase ends when its success criterion is met, not when effort is spent** (roadmap §"one rule"). Slippage is published, not absorbed.

## 5. Definition of done (summary — full detail in `07-ACCEPTANCE.md`)

- [ ] 24 green cells: 8 tests × 3 languages, each in debug **and** release builds (48 runs total, all exit 0)
- [ ] L1 torn-frame count 0 under all hold injections (5/10/50 ms)
- [ ] L2/L3 step bounds hold on release builds
- [ ] L7 zero writes to poisoned pages; `DROPPED_REVOKED` on every post-revocation publish
- [ ] Cross-language: every test passes in all three languages, **or the protocol is wrong** — treat as the latter until proven otherwise
- [ ] `litmus/REPORT.md` generated by the driver, with environment capture and honesty labels
- [ ] Zero external dependencies in all three kernels (C11 only / std-only / Node stdlib only)
