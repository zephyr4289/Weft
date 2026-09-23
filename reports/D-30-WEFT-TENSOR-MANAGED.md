# D-30 — WEFT-TENSOR-MANAGED: Pillar 2 Technical Report

**Project weft-tensor — Managed AI Runtimes, Ingestion Pipelines & 120 FPS UI Fabric**
To: Architecture & Systems Engineering Lead · From: Senior Systems Engineer 3 (Managed Runtimes, Cross-Platform AI APIs & UI Architecture Lead)
Branch: `feat/weft-tensor-managed` · Verdict: **ALL 8 INTEGRATION STAGES GREEN, ALL 4 LAWS MECHANICALLY ENFORCED**

---

## 0. TL;DR

| Directive item | Delivered | Evidence |
|---|---|---|
| A. WebRTC / Camera & Audio Ingestion Engine | `packages/weft-tensor` — `VideoFrameIngestor` (WebCodecs `copyTo` GPU→ring direct), `AudioPcmFeeder` (declared-quantum slot rolls) | commit p99 **1.21 µs** video / **0.73 µs** audio (gate: < 50 µs) |
| B. Python DLPack & PyTorch Zero-Copy | `python/weft_tensor` — ctypes `DLManagedTensor` capsules, `as_numpy()` shared alias | `np.from_dlpack` pointer-identity proof; 22 pytest green + 1 explicit torch skip |
| C. React Zero-Re-render AI Hooks | `packages/react-tensor` — `useWeftTensorCanvas` | 5 headless tests; stats-object identity = zero re-render by construction |
| D. Flutter AI Components | `packages/flutter-tensor` — `WeftTensorPainter`, `WeftVideoOverlay` | static audit **19/19** (42 constants parity, 89/89 explicit `Endian.little`) |
| E. E2E Flight Demo | `demos/realtime-ai-vision` — terminal + browser twins | detection p99 **0.11–0.13 ms** (gate < 1 ms); 120 Hz producer, stalls ≤ 6% |
| Cross-language wire parity | 8-stage CI shard `run_weft_tensor_shard.sh` | TS→Py and Py→TS byte-exact ring exchange; DLPack alias pointer proof |
| Zero-allocation (Law 1) | `--expose-gc` probes, 5 modes × 100k ops | heap delta 0.26–13 KiB (limit 64 KiB); negative control catches garbage |

---

## 1. The Strategic Imperative, Restated as a Memory Problem

Every framing of the "last-mile AI bottleneck" in the briefing collapses to one
physical fact: payloads cross **four ownership boundaries** — device memory,
camera/mic driver, the AI process, and the UI compositor — and each crossing is
currently implemented as a **copy plus a garbage-collection event**. A browser
tab that draws 60 tracked boxes over a 30 fps webcam feed with React is not
slow because detection is slow; it is slow because every frame constructs a
new `ImageData`, a new array of box objects, a new reconciliation pass, and a
new wave of nursery garbage that the GC must later sweep at exactly the moment
the next frame lands. The Python side multiplies this: camera frames cross
into NumPy/PyTorch through serialization or `np.copy` because there was no
shared memory contract both sides could validate.

Pillar 2 answers with one artifact: the **WTR1 tensor ring** — a single
contiguous, shareable, self-describing region of memory with a seqlock
publish protocol, produced once by the camera and consumed wait-free by
every downstream reader (detector, DLPack tensor, canvas plane, Flutter
painter). Nothing in the steady-state path allocates, nothing copies except
the one unavoidable producer memcpy (or, with WebCodecs, not even that), and
every reader validates the boundary before touching memory.

## 2. Architecture

```
        [ camera / mic / synthetic feed ]
                      |  (WebCodecs copyTo = GPU->ring, zero intermediate)
                      ▼
  ┌───────────────────────────────────────────────────────────────┐
  │  WTR1 ring — 128 B header + N slots (64 B slot header + payload) │
  │  publish = 2×SeqCst u32 stores (lo word LAST) · acquire =       │
  │  wait-free seqlock re-read (magic + committed bit + slot seq)    │
  └──────────┬───────────────────┬───────────────────┬─────────────┘
             |                   |                   |
             ▼                   ▼                   ▼
   VideoFrameIngestor   WeftTensorView        RingListener (rAF pump)
   AudioPcmFeeder       ├─ __dlpack__  → PyTorch/NumPy (zero-copy capsule)
   (TS + Python twins)  ├─ as_numpy()  → strided shared alias
                        ├─ Canvas2DPlane  (ONE ImageData, reused)
                        ├─ WebGL2Plane    (texSubImage2D from slot view)
                        └─ React/Flutter: controller + painter (no re-render)
```

The managed plane is deliberately a **sibling** of the frozen kernel envelope
(`core/c/weft.{c,h}` untouched — verified by the patch-series diff): the ring
header re-uses the `WEFT` magic and little-endian discipline, slots are the
tensor analogue of envelopes, and `schema_id` carries the Pillar 1 IR identity.
Engineer 1/2 native adapters can fill WTR1 rings without touching this pillar's
code; the managed side only ever reads.

## 3. WTR1 Memory Layout (normative — `docs/weft-tensor/LAYOUT-V1.md`)

Ring header (128 B): magic `"WEFT"`, `layout_version=1`, `header_size=128`,
`slot_count`, `slot_stride` (64-aligned), dtype as **DLPack `DLDataTypeCode`
on the wire** (`{code, bits, lanes}` — zero-mapping parity with the Python
bridge), `elem_size`, `shape[8]`/`strides[8]` (u32, strides in ELEMENTS per
DLPack convention), `schema_id` (u64), `producer_seq` (u64 publish word),
`tick_hz`, flags (bit0 = LE, bit1 = shared), **CRC-32/IEEE over the static
config** (bytes [0,96) ++ [104,112) — excludes the mutable seq and the CRC
field itself), 12 B reserved.

Slot header (64 B): magic `"WFRM"`, `payload_len`, `seq` (u64, 1-based),
`timestamp_ns` (u64), `duration_us`, flags (bit0 = COMMITTED, written last),
`fourcc` (LE ASCII: `RGBA`/`BGRA`/`I420`/`PCM `/`F32 `/`RAW `), rank, planes,
plane_offset[3]/plane_size[3] (V1 single-plane).

Design decisions worth defending:

1. **dtype = DLPack enum verbatim.** The Python capsule copies
   `{code, bits, lanes}` from the wire into `DLDataType` with no translation
   table. Any drift between the wire format and the consumer protocol is
   structurally impossible rather than tested-for.
2. **CRC over static config only.** Readers validate the full header once at
   attach (Law 4 boundary); the hot loop checks slot magic + committed bit +
   slot seq. A per-frame CRC would tax the 120 Hz path for integrity the
   seqlock already provides.
3. **Strides in elements, never "0 = derive".** Row-major sanity
   (`strides[i] >= strides[i+1] * shape[i+1]`) is validated fail-closed;
   the writer must declare a real layout.
4. **Two-store publish, lo word LAST.** `producer_seq` is published as two
   32-bit SeqCst stores (hi first, lo last) — zero BigInt allocation per
   commit on the TS side. A torn two-word read can never corrupt a consumer:
   every acquire re-validates the slot's own 64-bit seq, so a torn pointer
   read costs one bounded retry, never correctness.

## 4. Deliverable A — Ingestion Engine (TS, browser/Node/Deno/Electron)

**VideoFrameIngestor.** The WebCodecs fast path calls
`frame.copyTo(slotWritable, {format:'RGBA'})` — the decoder's output planes
are written **directly into ring slot memory**; no base64, no JSON, no
intermediate canvas, no per-frame allocation. Both the legacy sync signature
(returns void/length) and the spec's async `Promise<number>` are handled; the
async path is documented as allowing a Promise + two closures per frame.
Failures are **drops, never throws** into the frame loop (`stats.dropped`).
ImageData and raw-Uint8Array paths exist for environments without WebCodecs;
the ImageData path wraps the clamped array in one small view (documented).

**AudioPcmFeeder.** Slots roll at the **declared quantum** (`chunkSamples`),
not at the 64 B-padded physical capacity — `payload_len` records the live
bytes so `duration_us` stays truthful. Chunks of any size stream across slot
boundaries through a preallocated carry handle and bounded segment copies;
`flush()` commits a trailing partial slot with live `payload_len`. The dtype
gate rejects non-f32 rings at construction (Law 4).

**Measured ingestion cost** (Node 24.21, 50k ops, `bench/commit.bench.mjs`,
evidence in `packages/weft-tensor/bench/evidence/`):

| Path | p50 | p99 | max |
|---|---|---|---|
| Video 320×180 RGBA commit (WebCodecs-shaped sync copyTo) | 0.26 µs | **1.21 µs** | 158 µs (JIT warm edge) |
| Video acquireLatest | 0.19 µs | 0.30 µs | 43 µs |
| Audio 2ch×128 f32 feed (256-sample quanta) | 0.50 µs | **0.73 µs** | 174 µs |
| Audio acquireLatest | 0.10 µs | 0.60 µs | 148 µs |

The directive's "< 50 µs commit" gate is met with ~40× headroom on both paths.

## 5. Deliverable B — Python DLPack & PyTorch Bridge

`python/weft_tensor` mirrors the TS ring byte-for-byte (same constants, same
error codes, chained-zlib CRC verified against `node:zlib` in both suites).

**DLPack implementation** (`dlpack_capi.py`) is the hard part and is done
properly:

- Capsules are the unversioned `"dltensor"` protocol — accepted by both
  `np.from_dlpack` and `torch.from_dlpack`; consumers rename to
  `used_dltensor` on adoption.
- **All scratch (struct + shape + strides) is `malloc`'d and freed only by
  the consumer-triggered deleter.** During development we crash-tested the
  exact failure mode of freeing ctypes-owned memory with libc —
  `free(): invalid pointer` — and eliminated it by allocating every DLPack
  structure with explicit `malloc`. The deleter pops a registry entry that
  holds a plain Python reference to the ring, so the tensor's lifetime pins
  the ring's memory for exactly as long as the consumer holds the tensor —
  DLPack lifetime semantics, no manual refcounting.
- `strides=NULL` is served for C-contiguous rings (broadest consumer
  compatibility); real element strides otherwise.

**Zero-copy proof** (CI shard stage 6, over a ring produced by the *TypeScript*
side): `np.from_dlpack(view)` returns a tensor whose data pointer is
**identity-equal** to the precreated slot ndarray and lies inside the ring
buffer's address range. `as_numpy()` returns the SAME precreated ndarray per
slot every acquire — zero Python-side allocation at the bridge, verified by
`gc.callbacks` counters over 10k frame and 20k audio-chunk loops
(`tests/test_alloc.py`: **0 collections**).

PyTorch: the canonical line from the directive —
`torch.from_dlpack(view)` — is exercised in the same test via the identical
protocol path; the torch lane is `importorskip`-guarded with the reason
inlined (no torch in this environment; nothing is silently green).

## 6. Deliverable C — React Zero-Re-render Canvas Hooks

`packages/react-tensor` follows Pillar 1's dependency-injection pattern:
`createTensorCanvasHooks({useRef, useEffect})` produces the hook; the core
`TensorCanvasController` is framework-agnostic and testable headlessly.

- **One effect per `(ring, canvasRef)`.** Inside it: a `RingListener`
  (injectable pump — real rAF in production, manual stepping in tests) + a
  render plane (`Canvas2DPlane` or `WebGL2Plane`).
- **The stats object is sealed and identity-stable.** `statsRef.current` IS
  the controller's own live stats object. HUDs read it from rAF/intervals,
  never from React state — React literally has no re-render trigger.
- **Overlay contract:** the caller's `overlay(frame, scratch, plane)` callback
  owns the scratch lifecycle (`clear()` then pushes) — detectors run on ONE
  preallocated `Float32Array` (stride-6 box records), so the "AI" side also
  never allocates. Stall ticks re-invoke the overlay with `null` and repaint
  the last frame: the canvas never flickers and the HUD stays alive.
- Fixed during review: shape inference now handles rank-2 `[h, w]` rings
  (channels mode only when `rank >= 3 && last == 4`) plus a payload-cap gate
  in both planes.

## 7. Deliverable D — Flutter AI Components

`packages/flutter-tensor` (`weft_flutter_tensor`) was built against the same
normative layout with the same enforcement style: pure-Dart header layer
(`ring_header.dart`, no flutter/ffi imports — runs on the plain VM), seqlock
`WeftRing` via `dart:ffi` acquiring into a **reused** flyweight,
`WeftTensorNotifier` (fixed-capacity listener storage, swap-remove — the
Pillar 1 zero-GC Listenable pattern), `WeftTensorPainter`
(`shouldRepaint => false`, notifier-driven) and `WeftVideoOverlay` (one
Ticker, no `setState` in the frame path).

Sandbox honesty: no Dart SDK exists here, so enforcement is a **structural
audit** (`test/static_audit.mjs`, 19/19 green) rather than a compiler run —
it re-derives CRC known-answers from `node:zlib` at runtime, diffs **42
layout constants** against the TypeScript `layout.js`, scans **89 multi-byte
ByteData accesses** (0 without explicit `Endian.little`), and greps the
paint/tick hot paths for allocation patterns. The first real compile happens
on the flutter CI lane; this is stated, not hidden.

## 8. Deliverable E — End-to-End Flight Demo

`demos/realtime-ai-vision` runs the full pipeline: procedural synthetic
camera (drift-corrected 120 Hz producer) → WTR1 ring → **real** connected-
component blob detector (preallocated downsample grid + iterative flood fill
+ bbox extraction — not a mock) → terminal ASCII paint with live box overlays.

Flight verdicts (4 s flight, evidence committed):

| Metric | `--no-paint` (Law-1 flight) | painted flight |
|---|---|---|
| produced / rendered | 479 / 476 | 479 / 465 |
| producer slips | 0 | 0 |
| detection p50 / p99 | ~12 µs / **0.11 ms** | ~13 µs / **0.13 ms** |
| GC pauses / total | **4 / 1.87 ms** (0.05% duty) | 24 / (string fabric) |
| exit | 0 | 0 |

The painted flight's heap delta is attributed honestly: terminal output is
built from JS strings, which allocate by definition. That layer does not
exist in the web path — `Canvas2DPlane`/`WebGL2Plane` blit from the slot view
through `putImageData`/`texSubImage2D`, and the zero-alloc property of that
path is proven by the `render` mode of the allocation probe. The browser twin
(`web/`) runs the identical pipeline with zero bundler steps and an optional
webcam button that routes `VideoFrame.copyTo` straight into ring slots.

## 9. The Four Laws — Compliance Matrix

| Law | Requirement | Mechanical enforcement | Evidence |
|---|---|---|---|
| 1 | Zero allocation in ingestion/render loop | Per-slot preallocated views at attach; preallocated meta scratch (no `{...meta}` spreads — removed from our own hot paths during development); bound-method callbacks; `--expose-gc` probes | 5 modes × 100k ops: 0.26–13 KiB delta (limit 64 KiB); negative control allocates MBs and is caught; Python: 0 GC collections over 10k frames + 20k chunks |
| 2 | Strict LE + IEEE 754 | Byte-compared magics; explicit `true` LE on every DataView access; `<` struct formats in Python; `Endian.little` audit in Dart; f16 codec vs numpy golden vectors; chained-zlib CRC vs node:zlib | 46 TS tests, 22 Py tests, 19 Dart audit checks; f16 round-trip incl. subnormals + round-to-nearest-even ties |
| 3 | Browser / Node / Deno / Electron | Core = standard ESM + Web primitives only (`DataView`, `Atomics`, `performance.now`); zero `node:` imports in `src/`; injectable pumps/schedulers; runtime matrix detector | `runtime.test` covers node/deno/bun/browser/worker/electron detection + SAB gate; browser demo runs unbundled |
| 4 | Boundary schema validation | `validateRingHeader` fail-closed at attach — magic, version, sizes, dtype table, stride sanity, LE flag, CRC, bounds; slot validation on every acquire (seqlock); dtype gates on ingestors/renderers | 12-class corruption matrix in BOTH languages asserts machine-readable codes (`WTR1_BAD_MAGIC` … `WTR1_SHORT_RING`) |

## 10. Integration Stages & Parity

The CI shard (`ci/scripts/run_weft_tensor_shard.sh`, registered in
`extreme-test.yml` on the `any_code` route; `python/**` added to the route
with ownership declared in `ci/README.md`):

1. **Fixture determinism** — `make_ring_fixture.py --verify` (double-run byte parity)
2. **TypeScript suite** — 46 tests
3. **Python suite** — 22 tests + 1 explicit skip
4. **TS producer → Python consumer** — byte-exact ring exchange
5. **Python producer → TS consumer** — reverse exchange
6. **DLPack alias proof** — `np.from_dlpack` over the TS-produced ring,
   pointer-checked to lie inside the ring buffer
7. **Zero-allocation probes** — 5 modes × 100k ops, hard 64 KiB gate
8. **Latency gates** — commit p99 < 50 µs, video and audio

Result: **ALL 8 STAGES GREEN** (`evidence/pillar2-shard-run.log`). The
canonical fixtures (`fixtures/weft-tensor/ring-v1-{f32,u8}.bin`) are asserted
byte-exactly by both language suites — including the wrap-awareness that only
the last `slot_count` frames remain readable (seq 7..10 of 10 through 4 slots).

## 11. Benchmark Methodology

- Node 24.21.0, Linux x86_64 sandbox; `process.hrtime.bigint()`; 50k measured
  ops after 5k warmup; quantiles over sorted Float64Array; evidence JSON
  committed (`packages/weft-tensor/bench/evidence/*.json`).
- Allocation probes run in a **separate `--expose-gc` child process** with a
  20k warmup phase (JIT/IC metadata is one-time allocation, not per-frame
  garbage), full GC before and after the measured window.
- GC pauses recorded via `PerformanceObserver('gc')` in the demo.
- Honest caveat: `max_us` columns include JIT warm-edge outliers and OS
  scheduling; the p50/p99 columns are the steady-state story the gates use.

## 12. Limitations & Honest Notes

1. **Two u64 fields are Number-bounded (< 2^53).** `producer_seq` and slot
   `seq` above 2^53 are rejected fail-closed (`WTR1_SEQ_OVERFLOW`); Lo/Hi
   accessors exist for full-fidelity paths. Timestamps accept BigInt or
   `{lo, hi}`; wall-clock nanoseconds as bare Numbers are inexact above
   2^53 and the API refuses to pretend otherwise.
2. **Async WebCodecs path allocates** a Promise + two closures per frame
   (documented in code); use the sync path or `ingestRaw` in
   allocation-critical loops.
3. **Cross-process publication without atomics.** Python's mmap publish is
   two stores (lo last); the seqlock slot re-validation is what makes torn
   pointer reads safe. A future V2 could add a futex word for wait-free
   Python-side blocking, mirroring the TS `Atomics.wait` path.
4. **Flutter compile lane.** Structurally audited (19/19), not compiler-run
   in this sandbox; the first compile happens on the flutter CI lane.
5. **bfloat16 is reserved** (rejected in V1's dtype table) rather than
   half-implemented.

## 13. Handoff

- Branch `feat/weft-tensor-managed` (stacked on `feat/weftc-codegen-managed`),
  9 commits P1–P9 at report time + scorecard commit; patch series + mbox in
  `weftc-pillar2-managed.zip`.
- **Engineer 1/2 adapter contract:** produce WTR1 rings per
  `docs/weft-tensor/LAYOUT-V1.md`; the managed plane attaches to any buffer
  you hand it (`WeftTensorRing.attach` / `WeftRing.attach` / `WeftRing.attachPointer`).
  If your DMA ring already has a header, an adapter shim (not a layout fork)
  is the right seam — the IR `schema_id` identity from Pillar 1 carries over.
- Files touched: `packages/weft-tensor`, `packages/react-tensor`,
  `packages/flutter-tensor`, `python/weft_tensor`, `demos/realtime-ai-vision`,
  `docs/weft-tensor`, `fixtures/weft-tensor`, `scripts/make_ring_fixture.py`,
  `ci/scripts/run_weft_tensor_shard.sh`, `evidence/pillar2-shard-run.log`.
  Frozen kernel, `heddles/`, and Pillar 1 packages untouched (diff-verified).
