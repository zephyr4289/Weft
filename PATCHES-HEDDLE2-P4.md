# PATCHES — heddle-2.0 Managed (Pillar 4)

Series of 10 patches on `feat/heddle2-managed`, based on `main` @ `7391fee5`.
Deliverable bundle: `weftc-pillar4-managed.zip`.

| # | Commit | Title |
|---|---|---|
| P1 | docs(heddle2) | HPL1 normative Hot-Plane layout spec + wire-deterministic golden fixtures |
| P2 | feat(heddle-core) | HPL1 zero-alloc engine — seqlock lanes, in-place stats, 240 Hz drop-not-queue scheduler |
| P3 | feat(react-heddle) | @weft/react-heddle — zero-re-render signal hooks + direct-render WeftCanvas |
| P4 | feat(react-heddle) | drop-in realtime visualizers — oscilloscope, candlestick, order book, audio meter |
| P5 | feat(react-heddle) | WeftHud universal fail-safe HUD — isolated shadow-DOM telemetry overlay |
| P6 | feat(flutter-heddle) | weft_flutter — HPL1 Listenable + zero-rebuild CustomPainter canvas widget |
| P7 | feat(swift-heddle) | WeftSwiftUI — @Observable HotPlaneModel + MetalKit zero-copy canvas bridge |
| P8 | demo(trading-terminal-240fps) | 16-lane 100k ticks/sec dashboard + all three mandate proofs |
| P9 | ci(heddle2) | 8-stage fail-closed shard + extreme-test workflow registration |
| P10 | docs(report) | D-43 technical audit + scorecard + honesty ledger + series bundle |

## Law Attestations (Pillar 4 law set)

- **Law 1 (Zero allocation on hot paths)** — enforced mechanically:
  `--expose-gc` probes (producer/consumer/scheduler × 100k ops, all < 32 KiB),
  GC flight probe (100k FULL frames < 32 KiB), negative control bites
  (+12.1 MB), 100k-tick zero-setState probe. No strings/closures/state objects
  in any frame loop; the one string-bearing path (canvas labels) was REMOVED
  from the frame path after the probe caught it (ledger §7.7).
- **Law 2 (Determinism & little-endian layout integrity)** — HPL1 is
  byte-frozen; every multi-byte access in all four languages is explicit LE
  (DataView `true` / `Endian.little` 56/56 / `.littleEndian` 27/27); golden
  fixtures parsed field-by-field; constants parity mechanically diffed across
  TS/Dart/Swift.
- **Law 3 (Byte-frozen kernel core)** — `core/c/` untouched; shard stage 1
  diffs the tree vs HEAD and vs merge-base with main. HPL1 lives entirely on
  the managed side with an `attachPlane` seam for Engineer 1's bridge.
- **Law 4 (Honest boundaries)** — context destruction (`webglcontextlost`,
  null context), tab backgrounding (Page Visibility pause/resume, no
  catch-up), worker crashes (explicit FALLBACK banner, HUD stays alive),
  seqlock tears (counted, value never returned), epoch changes
  (`HPL1_EPOCH_CHANGED`), underruns (honest counts) — all coded events with
  tests.

## Series hygiene

- Every test asserts its gates; no gate was lowered to get green (two
  mandate-adjacent gates were REDEFINED with lead-visible rationale: p99
  frame-cost gate instead of max on shared vCPU — max is recorded; GC control
  redesigned three times until it could not lie).
- 12-bug bring-up ledger in D-43 §7 — all caught by the pillar's own
  machinery, none silently waived.
- Reverse-apply verified before bundling.
