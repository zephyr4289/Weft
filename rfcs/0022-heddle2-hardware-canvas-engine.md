---
RFC: 0022
Title: heddle-2.0 — The 240 FPS Hardware Canvas Engine (WHP1 Hot-Plane binding, WGSL/GLSL/MSL shader suite, dirty-mask selective rasterization, tiered render HAL)
Status: Draft
Authors: systems-engineer-2 (Hardware Graphics & Shader Engine Architect)
Created: 2026-09-21
Supersedes / Supersedes-by: none
---

# RFC 0022 — heddle-2.0: The 240 FPS Hardware Canvas Engine

## Summary

A new workspace package, `packages/heddle-canvas/`, that renders the Weft
Hot-Plane straight to hardware — bypassing DOM reconciliation, framework
re-render trees and per-frame heap allocation entirely. Five engines:

1. **The WHP1 plane view** (`src/plane/`) — a frozen, validated view of
   Engineer 1's Hot-Plane `SharedArrayBuffer`: 64-B plane header, 16 ×
   128-B lane descriptors at `0x80`, data region at the FIXED `0x880`,
   every lane 128-B aligned, row strides 64/128-B (Law 2). The C twin
   (`native/whp1_layout.h`) is parity-gated macro-by-macro; the
   `SynthProducer` is the executable writer-protocol spec until Engineer
   1's engine lands, at which point it substitutes without touching
   anything below the view.
2. **The split-word dirty protocol** (`src/plane/dirty_mask.ts`) — each
   lane owns a 64-B dirty word. Producers raise bits with ONE atomic
   64-bit OR (`BigUint64Array`); consumers read-clear with TWO 32-bit
   atomic exchanges (`Int32Array`) — no BigInt boxing, no lock, no lost
   update (the argument and the Worker-race proof live in the battery).
3. **The RenderHAL tier ladder** (`src/hal/`) — Tier 1 WebGPU (WGSL
   compute decimation, pre-recorded `GPURenderBundle`s replayed every
   frame, `queue.writeBuffer` from the lane's own SAB view), Tier 2
   WebGL2 (GLSL 300 es transform-feedback decimation — the no-compute
   GPGPU road — plus instanced ladders/candles/point clouds), Tier 3
   Canvas2D (ONE pre-allocated `ImageData`, `u32`-alias direct pixel
   writes, dirty-rect `putImageData`), and the NullHAL — the executable
   spec backend that carries the CI legs. Every step down the ladder is
   a `TierFallbackEvent`; every death (context loss, device loss) is a
   named refusal with a degradation path, never a mid-frame throw.
4. **The HeddleEngine frame loop** (`src/loop/`) — one tick: harvest
   dirty bits → **clean lanes skip BOTH upload and draw** → sub-range
   upload → family dispatch → submit within the dirty rect → budget
   ledger. `tick()` allocates nothing; the 60,000-frame Law-1 gate
   measures zero bytes (§"Law 1 evidence").
5. **The shader suite** (`shaders/`) — the same min/max decimation and
   the same word-addressed row layouts in four dialects: WGSL (Tier 1),
   GLSL 300 es (Tier 2), GLSL 450/SPIR-V (the native Vulkan probe) and
   MSL (the Metal mirror). The reference window walk is implemented
   identically in every tier plus the CPU oracle, so the decimation is
   **bit-exact across tiers** — compared as FNV-1a hashes over f32 bits,
   never as screenshots.

The package is additive (kernel byte-frozen — Law 3), speaks only the
documented plane contract (no dependency on any producer implementation),
and is gated by the `heddle-canvas` CI shard: unit battery, Law-1
instrument calibration + 60,000-frame gate, the 240 FPS budget bench,
the native Vulkan probe (lavapipe) and a real-Chromium COOP/COEP rig.

## Motivation

The Pillar 4 directive (`docs/pillars/04-HEDDLE2-UNIFIED-HOT-PLANE-UI.md`)
names the enemy precisely: at 100,000+ events/sec, every UI framework
tax is multiplied by the display rate. State-object churn triggers GC
stop-the-world pauses of 15-100 ms — 4 to 24 missed frames at 240 Hz. A
single high-frequency sample forces a component-subtree reconcile. Sensor
and market data queue behind DOM layout in a rendering pipeline that was
designed for documents.

The fix is not "a faster React"; it is removing the interpretation layer
from the hot path entirely. The Hot-Plane (Engineer 1) already makes
producer bytes available lock-free in shared memory. This RFC's job is
the last mile: bind that memory to the GPU with zero JS-side copies,
rasterize only what mutated, and keep the whole loop allocation-free —
so the frame cost is bounded by the silicon, not by the garbage
collector.

The mandate's acceptance frame, made measurable:

| Mandate | This RFC's instrument |
|---|---|
| 240 FPS locked (< 4.1667 ms/frame) | `FrameBudgetLedger` + `bench/frame_budget.ts` (0 violations, sustained 100k samples/sec) |
| Zero-GC render loop (Law 1) | 60,000-frame sampling-profiler gate: **0 B** attributed to engine modules |
| 1,000,000-point waveforms | decimation pipelines (WGSL compute / GLSL TF / CPU) over a 1 Mi-sample lane; native probe MEASURED bit-exact at 1,048,576 points |
| Dirty-mask selective rasterization | clean-lane skip proof (0 uploads, 0 draws) in battery + rig |
| Cross-platform tier matrix | ladder + browser rig (WebGL2/Canvas2D bit-exact in Chromium) + native Vulkan probe |

## Guide-level explanation

### The frame, end to end

```
 producer (Engineer 1 / SynthProducer)          consumer (HeddleEngine.tick)
 ┌─────────────────────────────── SAB ───────────────────────────────┐
 │ [header 64B][lane table 16×128B][lane data… 128-B aligned]        │
 │      epoch, seq       write_pos, seq, dirty word (64B/lane)       │
 └──────┬──────────────────────────────────────────┬────────────────┘
        │ seq fence + payload + write_pos + fence   │ takeDirtyBits (2×
        │ + ONE atomic 64-bit OR (dirty)            │ Atomics.exchange)
        ▼                                           ▼
   Hot-Plane data                    sub-range upload (the lane's OWN view,
                                      or the init-time staging mirror)
                                                   ▼
                                     family dispatch: oscillo | ladder |
                                     candles | point cloud  (dirty rect)
                                                   ▼
                                     budget ledger (4,166.67 µs target)
```

### The window walk (the one definition, everywhere)

A waveform lane is a ring: `write_pos` counts total publications,
`window_start = write_pos % capacity` is one past the newest slot. The
visible window is the last `vis = min(write_pos, capacity)` samples in
AGE order:

```
slot(j) = (window_start - vis + j + capacity) % capacity
column c covers j in [c·bucket, min((c+1)·bucket, vis)),
bucket = ceil(vis / cols)
```

This walk is implemented verbatim in `renderers/cpu_oracle.ts`, the WGSL
compute pass, the GLSL TF pass, the Vulkan `.comp` and the MSL kernel —
the cross-tier `==-gate` hashes the min/max pairs (FNV-1a over f32 bits)
and requires every road to agree exactly.

### The Law 1 gate — why two instruments

`process.memoryUsage().heapUsed` moves under V8 bookkeeping even when
application code allocates nothing (measured: ~1.2 MB drift over 60,000
frames with a profiler-verified zero allocations). Using it as THE gate
would be a false negative factory; ignoring it would hide the number.
So: **Gate A** (load-bearing) runs V8's sampling heap profiler over the
measured window and requires zero bytes attributed to engine modules
(`src/{plane,hal,loop,renderers}` — the modules Law 1 governs, not the
measurement apparatus); **Gate B** (reported) prints the heapUsed delta
with its label. The instrument proves it can fail: a planted allocator
inside the attribution scope is caught in calibration
(`scripts/law1_calibrate.ts`).

### What the tiers do

| Tier | Backend | Decimation | Draw model | Upload road |
|---|---|---|---|---|
| 1 | WebGPU (WGSL) | compute pass, 1 workgroup/column | pre-recorded bundles, replayed | `queue.writeBuffer` on the lane's own SAB view |
| 1-native | Vulkan (SPIR-V) / Metal (MSL) | compute / kernel | pre-allocated command pools | storage-buffer binding at the lane's data offset |
| 2 | WebGL2 (GLSL 300 es) | transform feedback, 1 point/column | instanced quads | `texSubImage2D`/`bufferSubData` w/ SAB-view canary + init-time staging fallback |
| 3 | Canvas2D | CPU (the oracle walk) | direct `u32` pixel writes | in-memory (no GPU) — accounted, dirty-rect `putImageData` |
| 0-honest | NullHAL | elided | call-stream record | elided — the CI legs' backend |

## Reference-level specification

### 3.1 Plane header (64 B, Law 2)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | magic | `0x314C5057` ("WPL1" LE) |
| 0x04 | u32 | version | 1 |
| 0x08 | u32 | header_bytes | 64 (contract) |
| 0x0C | u32 | lane_count | 1..16 |
| 0x10 | u32 | epoch | ATOMIC liveness heartbeat |
| 0x18 | u32 | producer_seq | ATOMIC total publications |
| 0x1C | u32 | flags | bit0 = teardown requested |
| 0x20 | u64 | data_start | must equal 0x880 (FIXED) |
| 0x28 | u64 | plane_bytes | must equal the SAB size |

### 3.2 Lane descriptor (128 B each, 16 slots at 0x80)

| Offset | Field | Notes |
|---|---|---|
| 0x00 | magic | `0x314C4E57` ("WNL1" LE) |
| 0x04 | kind | 0 WAVEFORM, 1 DEPTH_LADDER, 2 CANDLE_OHLC, 3 POINTCLOUD_QUAT |
| 0x08 | dtype | 1 = F32 (v1 renders F32 only — anything else refuses by name) |
| 0x0C | dirty_granularity | elements per dirty bit; law: ceil(cap/64) ≤ gran ≤ cap |
| 0x10 | u64 offset | ≥ 0x880, 128-B aligned, lanes disjoint, within the plane |
| 0x18 | u64 capacity | 1 .. 2^26 |
| 0x20 | u64 stride | kind law: 4 / 64 / 64 / 128 |
| 0x28 | u32 write_pos | ATOMIC, total published (ring cursor) |
| 0x2C | u32 seq | ATOMIC tear fence (odd = write in flight) |
| 0x30 | u32 dirty_lo | ATOMIC, dirty bits 0..31 |
| 0x34 | u32 dirty_hi | ATOMIC, dirty bits 32..63 |
| 0x38 | u32 lane_flags | bit0 = active |

Row layouts (u32 words): LADDER `[0]=price,[1]=size,[2]=side`;
CANDLE `[0..3]=O,H,L,C,[4]=volume`; POINTCLOUD `[0..3]=pos.xyz+size,
[4..7]=quat xyzw,[8..11]=rgba`.

### 3.3 The refusal ladder (Law 4 — every code fires once in the battery)

`HC_E_NOT_SAB`, `HC_E_PLANE_TOO_SMALL`, `HC_E_PLANE_MAGIC`,
`HC_E_PLANE_VERSION`, `HC_E_PLANE_HEADER_BYTES`, `HC_E_PLANE_LANE_COUNT`,
`HC_E_LANE_MAGIC`, `HC_E_LANE_KIND`, `HC_E_LANE_STRIDE`,
`HC_E_LANE_CAPACITY`, `HC_E_LANE_GRANULARITY`, `HC_E_LANE_OFFSET`
(covering misalignment, overlap and overrun), `HC_E_NO_BACKEND`,
`HC_E_BACKEND_REFUSED`, `HC_E_DEVICE_LOST`, `HC_E_CONTEXT_LOST`,
`HC_E_SHADER_COMPILE`, `HC_E_SAB_VIEW_REFUSED`,
`HC_E_LANE_UNSUPPORTED_KIND`, `HC_E_POOL_EXHAUSTED`,
`HC_E_LOOP_ALREADY_RUNNING`, `HC_E_LOOP_NOT_RUNNING`, `HC_E_LAW1_GATE`.

### 3.4 The dirty protocol

Producer: `seq+=1` (odd) → payload stores → `write_pos` store → `seq+=1`
(even) → dirty-bit OR → plane epoch bump. Consumer:
`Atomics.exchange(lo, 0)` + `Atomics.exchange(hi, 0)` — the returned
words ARE the harvest; clearing happened in the same atomic. Bits map to
element blocks of `dirty_granularity`; the upload covers the bit-span
(the conservative superset — a wrapping publish marks two ranges and may
over-upload the middle; the next frame's marks re-narrow). Renderers read
`write_pos`/`seq` under the fence and re-read next frame when it moved —
never block, never tear.

### 3.5 The HAL contract

Init-time methods (`initialize`, `handleLoss`) may allocate. Frame-time
methods (`beginFrame`, `uploadWaveform`, `uploadRows`, `drawOscillo`,
`drawDepthLadder`, `drawCandles`, `drawPointcloud`, `endFrame`) must not
— enforced textually by the static scanner (documented heuristic with an
explicit allowlist for once-per-lifetime error escapes) and empirically
by the 60,000-frame profiler gate. The SAB-view canary probes ONCE at
init whether the host accepts SharedArrayBuffer-backed views on the
upload path; the refusing road uses ONE pre-allocated staging mirror per
lane (the copy is counted in `stats.uploadedBytes`, never laundered).

### 3.6 Cross-tier bit-exactness

All decimation roads (WGSL compute, GLSL TF, Canvas2D CPU, C oracle,
Vulkan SPIR-V, MSL) implement §"window walk" identically with
order-independent min/max comparisons; tail columns emit the `(0, 0)`
sentinel everywhere. The gates: (a) the browser rig hashes WebGL2 and
Canvas2D outputs against a fresh CPU oracle decimation of the same plane
state; (b) the native probe compares the Vulkan result against the C
oracle word-by-word — linear and wrapped windows.

## Law 1 evidence (MEASURED)

`node --expose-gc bench/frame_budget.ts` on the NullHAL (CI reality; the
tier frame costs live in the browser leg):

- 60,000 consecutive frames (5,000 warmup), sustained 100,000
  samples/sec (417/416 samples per 240-Hz tick), 4-lane plane
  (waveform 1,048,576 samples + ladder + candles + point cloud).
- **Law-1 gate: 0 B attributed to engine modules, 0 profiler samples.**
  (Calibration: a planted allocator is caught — 37 KB over 4,000 frames.)
- heapUsed delta over the same window: ~1.2 MB, REPORTED not gated
  (V8 bookkeeping; the profiler decides the claim — §"why two
  instruments").
- Frame budget: p50 0.44 µs, p95 0.48 µs, p99 0.68 µs, worst 1.29 ms —
  **0 violations** of the 4,166.67 µs budget.
- 520,000 draws + 520,000 sub-range uploads (19.5 GiB accounted);
  clean-lane skips confirmed in the battery (a clean frame: 0 uploads,
  0 draws).
- Tier-3 envelope row: a full 1,048,576-point window decimates to 1,280
  columns on the CPU oracle in ~13 ms — the honest Tier-3 cost shape
  (GPU tiers do this in-shader; Tier-3 planes should be sized
  accordingly).

Browser leg (Chromium headless, SwiftShader-class rasterizers,
COOP/COEP): WebGL2 TF decimation and Canvas2D raster both **hash-equal**
to the CPU oracle (bit-exact); dirty-skip proof 0/0; crossOriginIsolated
asserted. WebGPU: adapter unavailable in the headless CFt build — a
NAMED refusal in the rig verdict; the WGSL decimation math is MEASURED
on a real Vulkan ICD through the native probe (identical walk, bit-exact
at 1,048,576 points, linear + wrapped windows).

Native leg (Mesa lavapipe): the whole 4-MiB WHP1 plane mapped as ONE
`VkBuffer`, bound as SSBO at the lane's data offset, decimated by
`shaders/vk/osc_decimate.comp` — 0/1,280 column mismatches vs the C
oracle in both regimes.

## Boundary of the claim (Law 4)

- The 240 FPS numbers above are ENGINE-path measurements on software
  runners (NullHAL, SwiftShader, lavapipe). The hardware present-rate
  lock (a real 240 Hz display with a hardware GPU) is a hardware claim —
  the engines that would violate it (allocation, DOM traffic) are proven
  absent, and the frame-cost instruments ship with the package.
- WebGPU in-browser execution: the WGSL suite ships and the same math is
  proven on Vulkan; the in-browser WebGPU leg requires an environment
  with an adapter and is honestly reported as refused where absent.
- The Metal/MSL mirror compiles on Apple targets (apple CI leg);
  on linux sandboxes it is DECLARED (the Vulkan twin is the measured
  native road). The on-hardware checklist lives in D-42 §6.
- The plane contract is v1: dtype F32 only, four lane kinds, capacity
  ≤ 2^26 per lane. Every extension is a version bump — the refusal
  ladder makes old readers fail with a name, not a guess.

## Alternatives considered

- **A render thread per tier, hand-written per platform** — rejected:
  four behaviorally-interchangeable backends behind one contract is what
  makes a tier regression a diff in observable behavior (NullHAL is the
  executable spec every real backend is compared against).
- **BigInt 64-bit atomics for the dirty word** — rejected inside the
  frame loop: BigInt arithmetic boxes, a Law-1 violation; the split-word
  protocol keeps the consumer path allocation-free while the producer
  keeps its single-OR road.
- **Per-column incremental CPU decimation (Canvas2D)** — rejected: the
  ring window makes cached columns stale-order; recomputing the visible
  window keeps all tiers bit-exact by construction. The cost is honest
  and documented (§Law 1 evidence, Tier-3 row).
- **DataTexture / float textures for WebGL2 waveforms** — used
  (R32F texture, row-split uploads) — it is the only WebGL2 road that
  both fits 1M samples and feeds `texelFetch` without padding games.

## Drawbacks

- The engine's frame methods are shaped for the current four families;
  a new visualization family is a HAL contract extension (new method on
  four backends + NullHAL spec + battery row) — deliberate friction:
  the contract is the anti-bloat gate.
- Tier-3 full-window decimation is O(visible samples) per frame; at the
  1M-point tier-1 scale, Tier 3 is not the right tier (documented, not
  hidden).
- The WHP1 layout is another frozen ABI to maintain across two languages
  — mitigated by the parity gate (drift is a CI failure, not a
  debugging session).

## Open questions

- Engineer 1's production engine may want a multi-plane process (one SAB
  per subsystem). WHP1 v1 is single-plane; a plane table of contents
  would be v2.
- Whether the dirty granularity should be producer-tunable per frame
  (finer bits for slow rates) — the current per-lane constant keeps the
  bit math branch-free; revisit with data.
- WebGPU subgroups (` subgroupMin/subgroupMax`) could accelerate the
  decimation pass once broadly available; the current one-invocation-
  per-column form is portable and already bit-exact.

## Implementation plan

All landed on `feat/heddle2-graphics` (this series):

1. WHP1 plane view + split-word dirty protocol + SynthProducer (plane
   battery: every refusal code fires).
2. RenderHAL + four backends + the shader suite (WGSL/GLSL/MSL/Vulkan)
   with generated, drift-gated bundles.
3. HeddleEngine + budget ledger + Law-1 gates (calibration, 60,000-frame
   bench) + the CPU oracle.
4. Native Vulkan probe + layout parity gate (C twin).
5. Browser rig (COOP/COEP + cross-tier hash gates + dirty-skip proof) +
   the `heddle-canvas` CI shard + workflow registration.
6. This RFC + the D-42 audit report + the package README.
