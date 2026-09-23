# PATCHES-WEFT-TENSOR-P2 — Pillar 2 Scorecard (Project weft-tensor)

Engineer 3 · branch `feat/weft-tensor-managed` · base: `feat/weftc-codegen-managed` tip

| # | Commit | Deliverable | Tests / gates |
|---|--------|-------------|---------------|
| P1 | `docs(weft-tensor): WTR1 normative layout spec + deterministic golden fixtures` | layout spec §law map + fixture generator + 2 committed fixtures | double-run byte-identical |
| P2 | `feat(weft-tensor): WTR1 seqlock tensor ring — zero-alloc core` | layout/ring/frame-view/listener/runtime + d.ts | 21 tests (CRC vs zlib oracle, f16 vs numpy, torn-read sim, corruption matrix, SAB smoke) |
| P3 | `feat(weft-tensor): ingestion engines + allocation probes` | VideoFrameIngestor + AudioPcmFeeder + probes | 10 ingest tests + 6 alloc tests (5 modes × 100k + negative control) |
| P4 | `feat(weft-tensor): Canvas2D/WebGL2 render planes + overlay scratch + benches` | 2 planes + OverlayScratch + COCO17 + benches | 5 render tests; bench evidence JSON committed |
| P5 | `feat(weft-tensor): Python weft_tensor — DLPack capsule + NumPy zero-copy bridge` | weft_tensor pkg (layout/ring/view/dlpack_capi/ingest) | 22 pytest + 1 explicit torch skip; 0 GC collections over 10k frames |
| P6 | `ci(weft-tensor): 8-stage cross-language shard` | run_weft_tensor_shard.sh + workflow + ci/README | 8/8 stages green (evidence log committed) |
| P7 | `feat(react-tensor): @weft/react-tensor — zero-re-render AI canvas hooks` | controller + hook + shim tests | 5 tests |
| P8 | `feat(flutter-tensor): weft_flutter_tensor — dart:ffi zero-GC AI overlay widgets` | pure-Dart header + ffi ring + painter/overlay/notifier | static audit 19/19 |
| P9 | `demo(realtime-ai-vision): 120 FPS flight demo — terminal + browser twins` | demo.mjs + lib/ + web/ + evidence | both modes exit 0; detection p99 < 0.13 ms |
| P10 | `docs(report): D-30 + this scorecard` | reports/D-30-WEFT-TENSOR-MANAGED.md | — |

## Law enforcement matrix (mechanical, not aspirational)

| Law | Mechanism | Where |
|---|---|---|
| 1 zero-alloc | preallocated per-slot views/meta scratch; `--expose-gc` probes w/ hard 64 KiB gate + negative control; Python `gc.callbacks` counters | alloc.test.mjs, test_alloc.py, shard stage 7 |
| 2 LE + IEEE 754 | byte-compared magics; explicit LE on every access; f16 numpy vectors; chained-zlib CRC vs node:zlib; Dart 89/89 `Endian.little` scan | layout tests (TS/Py), static_audit C04/C06/C07 |
| 3 universal runtime | zero `node:` imports in src; injectable pumps; runtime matrix; unbundled browser demo | runtime.test.mjs, demos/web |
| 4 boundary validation | 12-class corruption matrix with machine-readable codes in BOTH languages; dtype gates; seqlock slot validation per acquire | layout.test.mjs / test_layout.py, shard stages 2–3 |

## Bugs caught by our own gates during development (no-silent-green)

1. `VALID_DTYPES` hex encoding error (0x232 vs 0x220) — dtype table test.
2. Missing slot-magic write in TS `finishCommit` — acquire tests.
3. 1-based seq base error in `acquireLatest` (spec §5 corrected together with code).
4. Node Buffer pool-slice pitfall in tests (`buffer.slice(0)`).
5. XOR-of-two-CRCs ≠ CRC-of-concatenation in Python — node:zlib oracle.
6. ctypes-owned DLPack scratch freed by libc → `free(): invalid pointer`;
   malloc-only scratch + registry lifetime redesign (crash-tested).
7. Missing slot magic in Python feeder `_finish_slot` (same class as #2).
8. `{...meta}` spread + closure allocation in our own hot paths — removed
   after the alloc probes; meta scratch pattern now used everywhere.

## Guardrail attestations

- Kernel `core/c/weft.{c,h}`: untouched (byte-frozen).
- Pillar 1 deliverables: untouched (diff-verified).
- CI: shard registered with ownership per the anti-regrowth contract;
  `set -euo pipefail` (silent-green contract) throughout.
