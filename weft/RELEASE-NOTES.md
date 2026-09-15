# Weft Sandbox v0.1 — Release Notes

> Per WO-P5-RELEASE §1.T6 + decision 8: "Release notes document the one kernel delta: the Phase 2 debug-view accessor (semver minor, both kernels, read-only/wait-free per WO-P2 T1). Nothing else has touched the kernel since Phase 0 freeze; say so."

## 1. What this is

The sandbox-buildable Weft v0.1 release. Per the sandbox-constrained roadmap,
this is the endpoint: the sandbox-buildable project is complete at this
phase. Real-device verification is Phase 6+.

## 2. The one kernel delta since Phase 0 freeze

The kernel (C: `core/c/weft.{h,c}`, Rust: `core/rust/src/lib.rs`, TypeScript:
`core/ts/weft.ts`) is **FROZEN** with a single semver-minor delta landed in
Phase 2 (WO-P2-TOOLS T1):

- **`weft_debug_view()` / `debug_state()`** — read-only, wait-free,
  allocation-free state inspector. Added to support `weft-probe`'s
  quiesced/live-dump modes. Never dereferences freed/poisoned buffers (I6).
  Telemetry fields labeled advisory (AXIOM T).

Nothing else has touched the kernel since Phase 0 freeze. The Triad Protocol
specification (RFC-0001, single atomic exchange per publish/claim) is
unchanged across all five W-suite backends and all three language ports.

## 3. What this release contains

### Phase 0 — Kernel + Litmus (CLOSED)
- 3-language kernel (C, Rust, TypeScript) implementing the corrected Triad Protocol
- 8-test litmus suite (L1–L8) × 3 languages × 2 build modes = 24 cells, all green
- TSAN supplementary column (5×8 = 40 runs, clean)
- Loom v1 + v2 model-check transcripts

### Phase 1 — Benchmark suite (CLOSED)
- 5 B-cell benchmarks × 3 languages = 15 cells, all informational-gate green
- Canonical bundle: `bench/results.json` (sha256 `16b5c663` — the whitepaper's pinned source)

### Phase 2 — Tools (CLOSED at C2)
- `weft-probe` (C + Rust) — state inspector with quiesced/live-dump/revocation-safe modes
- `weft-record` (C + Rust) — capture/replay with `.weftrec` v1 format (32-byte header, per-record CRC, crash-tolerant)
- 4/4 interop combos green (C→C, C→Rust, Rust→Rust, Rust→C)
- 30s soak evidence: World A confirmed (writer paced at 120 Hz, RSS flat, replay byte-identical)

### Phase 3 — Whitepaper (CLOSED at C1r)
- `Weft-Whitepaper-v1.0.4.pdf` (16pp, sha256 `6f3a3211...`)
- Forensic scan-clean at pinned 611.5pt threshold (whole-document, all 16 pages)
- TOC restored, single numbering scheme, 1in margins, XeTeX-native fonts (§, ·, ×, →, ≤ all extract cleanly)

### Phase 4 — Ports (CLOSED at C3)
- Kotlin/Android (kernel + Steward + Heddle + TriadNative + README), validator 16/16
- Swift/iOS (kernel + Steward + Heddle + README), validator 15/15
- Dart/Flutter (kernel + Steward + Heddle + README + forward interface + single-isolate banner), validator 18/18
- TS Heddles (react, svelte, vue, react-native), validator 8/8
- All source-only, structurally validated, unbenchmarked

### Phase 5 — Sandbox release (THIS RELEASE)
- W-suite: 5 workloads × 4 implementations (A=reactive naive, B=best practice pooled, C=Weft ctypes, D=hand-rolled triple-buffer) = 20 cells, all PASS
- 0-alloc assertion satisfied for C and D across all 5 workloads
- D-vs-C finding: D is consistently slower than C on P99 FPS — the specified protocol beats the hand-rolled implementation on the metrics that matter
- Thermal proxy: 4/4 backends FLAT decay curves (headless expected; device thermal is Phase 6+)
- Static site: 4 pages (index, B-suite, W-suite, reproducibility), zero client-side JS, deterministic render (byte-identical on re-render with same bundles)
- `weft-sandbox-v0.1.tar.gz` (this artifact)
- `INSTALL.md` (in-sandbox vs real-device split)
- `SHA256SUMS` (tarball + whitepaper PDF + all shipped report PDFs)
- `Weft-Phase5-Release-Report.pdf` (this document, typeset)

## 4. Known limitations (what the sandbox cannot prove)

- **Device matrix**: real-hardware benchmarks (Pixel 7a, iPhone 13, etc.) are Phase 6+
- **Real UI stacks**: Compose / SwiftUI / Flutter widget-tree integration is Phase 6+
- **Literal fresh-machine verification on foreign hardware**: post-release contributor loop
- **Thermal envelope**: headless server has no meaningful thermal curve; device thermal is Phase 6+
- **Public `weft.dev` launch**: Phase 6+
- **Cross-process / shm Wefts**: Phase 6+
- **Pro tier** (leak-detection dashboard, crash analytics): out of scope, charter clause 4

## 5. Honesty labels

- All measured numbers carry `[MEASURED x86_64-sandbox]`
- Structural gates are normative; telemetry is advisory (AXIOM T)
- Verification claims name scan scope + numeric threshold (per WO-P4-CLOSURE P4-W1 standing rule addendum)
- Quantitative misses are declared (per WO-P5-RELEASE §2 rule 7)
