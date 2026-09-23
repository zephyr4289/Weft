# D-42 — heddle-2.0 Hardware Canvas Engine: Technical Audit Report

**Series:** Project heddle-2.0 (Pillar 4) · **RFC:** 0022 · **Branch:** `feat/heddle2-graphics`
**Scope:** `packages/heddle-canvas/`, `shaders/` (in-package), `native/`, CI shard `heddle-canvas`
**Laws:** 1 (zero per-frame allocation) · 2 (explicit little-endian + 64/128-B alignment) · 3 (kernel byte-frozen, verified) · 4 (named refusals at every boundary)

---

## 1. Executive summary

The Pillar 4 mandate — a 240 FPS, zero-GC hardware canvas engine bound
directly to the Hot-Plane — is delivered as `packages/heddle-canvas` with
every claim carrying its instrument:

| Claim | Instrument | Verdict |
|---|---|---|
| Zero per-frame heap allocation (Law 1) | V8 sampling heap profiler, 60,000 consecutive frames | **MEASURED: 0 B / 0 samples** attributed to engine modules; instrument calibration catches a planted allocator |
| Frame budget < 4.1667 ms under sustained 100k samples/sec | `bench/frame_budget.ts` ledger | **MEASURED: p99 0.68 µs, worst 1.29 ms, 0 violations** (engine path, NullHAL) |
| 1,000,000-point waveform decimation | WGSL/GLSL/CPU/Vulkan pipelines + native probe | **MEASURED (lavapipe): 0/1280 column mismatches**, linear + wrapped windows |
| Cross-tier bit-exactness | FNV-1a hash gates (rig + oracle + probe) | **MEASURED (Chromium): WebGL2 == Canvas2D == CPU oracle** |
| Dirty-mask selective rasterization | clean-lane skip counters (battery + rig) | **MEASURED: 0 uploads, 0 draws on clean frames** |
| Kernel freeze (Law 3) | `git diff origin/main -- core/c/weft.{c,h}` | **0 diffs** |
| Hardware present-rate lock at 240 Hz | — (needs hardware display + GPU) | **HARDWARE-DEFERRED** (§6) |

Shard: `ci/scripts/run_heddle_canvas_shard.sh` — 5 legs, all green
locally (vitest 66-test battery, Law-1 calibration + 60k gate, native
Vulkan probe on lavapipe, Chromium COOP/COEP rig, kernel freeze).

## 2. What shipped

```
packages/heddle-canvas/
  src/plane/          WHP1 contract: whp1.ts (layout), hot_plane.ts (view +
                      refusal ladder), dirty_mask.ts (split-word protocol),
                      synth.ts (executable writer-protocol spec)
  src/hal/            hal.ts (RenderHAL), tier.ts (ladder + stats),
                      null_device.ts (executable spec), webgl2/ (binder +
                      context-loss machine), webgpu/ (binder + device-loss
                      machine), canvas2d/ (blitter)
  src/loop/           frame_engine.ts (the tick), budget.ts (the ledger)
  src/law1/           audit.ts (two-instrument gate), static_scan.ts
  src/renderers/      cpu_oracle.ts (reference window walk + hashing)
  shaders/            wgsl/ (5), glsl/webgl2/ (10), vk/ (1 comp + spv),
                      msl/ (mirror) — canonical text; bundles generated
  native/             whp1_layout.h (C twin), vk_heddle_probe.c, Makefile
  bench/              frame_budget.ts (the evidence bench)
  rig/                the Chromium rig (page + server + driver)
  test/               66 tests / 8 files
ci/scripts/run_heddle_canvas_shard.sh   + extreme-test.yml registration
rfcs/0022-heddle2-hardware-canvas-engine.md
reports/D-42-HEDDLE2-GRAPHICS.md         (this report)
```

## 3. Evidence log (labels per house discipline)

All numbers below are **MEASURED** on this 2-CPU sandbox unless labeled
otherwise. Run of record: `ci/run-artifacts/shard-heddle-canvas.log`.

### 3.1 Law-1 gate — the instrument first, then the claim

- **Calibration (must-bite):** a planted per-frame allocator inside the
  engine's attribution scope (`src/renderers/planted_allocator.ts`) is
  caught by the profiler — 37,104 B over 4,000 frames. A clean engine
  loop reads 0 B. An instrument that cannot fail proves nothing.
- **The gate:** 60,000 consecutive frames (5,000 warmup), 4-lane plane,
  sustained 100,000 samples/sec. **0 B / 0 allocation samples attributed
  to `src/{plane,hal,loop,renderers}`.**
- **The secondary number, honestly reported:** `heapUsed` drifts ~1.2 MB
  over the same window with profiler-verified zero engine allocations —
  V8 bookkeeping (semi-space accounting, IC metadata, OSR of the
  measurement loop). Gate B reports it; Gate A decides the claim. The
  separation is documented in `src/law1/audit.ts` and RFC-0022.

### 3.2 Frame budget (240 FPS target = 4,166.67 µs)

Engine path on NullHAL, 100k samples/sec sustained (417/416 per tick —
exact long-run rate):

| Metric | Value |
|---|---|
| p50 / p95 / p99 | 0.44 µs / 0.48 µs / 0.68 µs |
| worst frame | 1.29 ms (a V8-internal GC of the harness's own noise) |
| budget violations | **0** |
| draws / uploads accounted | 520,000 / 520,000 (19.5 GiB sub-range uploads) |

Browser rig (Chromium headless, SwiftShader — software rasterizers, the
label matters): WebGL2 avg tick 19.1 ms (the TF decimation over a 64-Ki
window in software), Canvas2D 2.1 ms, NullHAL 23 µs. **CI-GATED, labeled
SwiftShader** — these are software-renderer costs proving correctness,
not hardware throughput.

Tier-3 envelope: a full 1,048,576-point window decimates to 1,280
columns on the CPU in ~13 ms — the honest Tier-3 cost shape (Tier-3
planes should be sized smaller; GPU tiers decimate in-shader).

### 3.3 Cross-tier bit-exactness (the ==-gate)

| Road | Where | Result |
|---|---|---|
| GLSL transform-feedback vs CPU oracle | Chromium rig, 64-Ki window | hash 2136325545 == 2136325545 **BIT-EXACT** |
| Canvas2D raster vs CPU oracle | Chromium rig | **BIT-EXACT** (same hash) |
| Vulkan SPIR-V vs C oracle, linear window (600,000 visible) | native probe, lavapipe | **0/1280 mismatches** |
| Vulkan SPIR-V vs C oracle, wrapped ring (writePos 1,060,921) | native probe, lavapipe | **0/1280 mismatches** |
| WGSL vs CPU oracle | **DECLARED** in this sandbox (no WebGPU adapter in headless CFt Chromium; named refusal in the rig verdict) — the identical window walk is MEASURED via the Vulkan twin |

### 3.4 Plane contract battery

26 tests: every PLANE_* refusal code fires exactly once (magic, version,
header size, data_start, plane_bytes, lane count, lane magic, kind,
dtype, stride law, capacity, granularity law, offset alignment / overlap
/ overrun) + create→open roundtrip + protocol semantics (fence parity,
heartbeat, write_pos word). The C twin
(`native/whp1_layout.h`) is macro-for-macro equal (parity test), and the
C plane-size helper is compiled and executed against
`whp1PlaneBytes` — two languages, one contract.

### 3.5 Dirty protocol

6 tests: read-clear-in-one-op semantics, non-destructive peek, road A
(64-bit OR) == road B (two 32-bit ORs) across 64 trials, high-word bits,
bit-span→element-range mapping, and a Worker hammering road A 3,000×
while the main thread consumes — after quiescence every distinct bit is
accounted and the word reads clean (no lost update).

## 4. Defects found and fixed during the work (the honest section)

1. **WebGPU params-buffer split (inherited design):** the bind groups
   held an init-time params uniform buffer while `writeParams` wrote a
   lazily-created second one — the shaders would never have seen
   per-frame updates. Fixed: ONE params buffer per lane, carved in
   `initialize`, held by every bind group; a missing buffer is now a
   named refusal.
2. **WebGL2 transform-feedback hazard (found by the rig, MEASURED):**
   the min/max buffer rode `ARRAY_BUFFER` inside the ribbon VAO while
   the TF pass captured into it — Chromium rejects TF writes to a
   buffer bound to another target (`INVALID_OPERATION`), the draw
   silently skips, the readback returns zeros. Fixed by unbinding
   `ARRAY_BUFFER` for the capture; the failure and its message are
   documented in the binder.
3. **Context-loss events wired to the wrong target:** the first draft
   listened on the WebGL2 *context* — Chromium fires
   `webglcontextlost` at the *canvas* (the context has no
   `addEventListener`). Found by the rig probe; fixed with the canvas as
   the documented event target.
4. **Hand-remembered Vulkan sType constants (the recurring lesson):**
   the native probe's first draft defined `DESCRIPTOR_POOL=21`,
   `SET_ALLOCATE=22`, `WRITE_DESCRIPTOR_SET=23`, `STORAGE_BUFFER=0x8`,
   `COMPUTE_PIPELINE=17`… stale-memory numbers. The repo's Series-8
   ABI-audited `vk_min.h` has the true values (33/34/35, 0x20, 29). The
   pipeline "created fine" under lavapipe's lenience and the descriptor
   update crashed — exactly the failure class vk_min.h's audit header
   predicts. Fixed by using only the audited constants; the lesson is
   now a comment in the probe source.
5. **heapUsed as a Law-1 gate (instrument design):** the first gate
   demanded `heapUsed delta === 0` and correctly FAILED while the
   profiler showed zero real allocations — V8 bookkeeping drifts ~1.2 MB
   per 60k frames on its own. The honest instrument split (profiler
   decides, heapUsed is reported) replaced it, with the heap-gate
   semantics kept as the secondary.
6. **BigInt in the bench producer:** the 60k Law-1 loop drove both
   producer and consumer; `SynthProducer`'s contract road (BigInt 64-bit
   OR) boxes, which would have failed the gate on the PRODUCER side.
   The bench producer now uses the split-u32 road (proven
   bit-identical) plus an SMI-only waveform generator — the rig keeps
   the full-precision generator (producers may allocate; the engine may
   not).

## 5. Boundaries (Law 4)

- **Engine path vs tier cost:** the 0.68 µs p99 is the ENGINE loop
  (dirty harvest, sub-range handoff, dispatch, ledger) on the NullHAL.
  Tier frame costs are measured separately (rig) and carry the
  SwiftShader label.
- **WebGPU in-browser:** adapter unavailable in the headless
  Chrome-for-Testing build (named refusal in the rig verdict). The WGSL
  suite ships; its decimation math is MEASURED on real Vulkan via the
  native probe (same walk, bit-exact). Environments with adapters run
  the same rig gate unchanged.
- **The spec-mandated WebGPU residual:** one command encoder + one
  submit per frame are WebGPU-spec objects user code cannot pool. The
  JS heap gate does not see them (driver-side); they are documented in
  RFC-0022 §8-adjacent notes and dwarfed by the bundle replay design
  (zero per-frame draw-command construction).
- **Tier-3 scale:** full-window CPU decimation is O(visible); at 1M
  points Tier 3 is the wrong tier (~13 ms/window on this box) —
  documented with the number, not hidden.

## 6. Hardware-deferred checklist (the on-hardware leg)

- [ ] 240 Hz display + hardware GPU: run `bench/frame_budget.ts` shape
      with the WebGPU HAL wired to a real canvas context; the ledger is
      the gate (0 violations at 4,166.67 µs).
- [ ] Hardware adapter rig pass (WebGPU tier: oracle hash + dirty-skip +
      bundle replay counters).
- [ ] Metal/MSL leg on Apple CI (mirror compiles; `osc_decimate` kernel
      vs the same CPU oracle — the MSL is the byte-twin of the measured
      Vulkan road).
- [ ] Discrete-GPU upload bandwidth for the 4-MiB plane (the lavapipe
      proof is correctness, not bandwidth).
- [ ] Real context-loss/device-loss injection on hardware (the state
      machines are unit-tested via the injected probe matrix; a hardware
      kill-switch pass exercises the same ladder end-to-end).

## 7. Verification matrix (local, this series)

| Leg | Command | Result |
|---|---|---|
| Unit battery | `npm run test` (vitest) | 65 passed, 1 conditional skip, 0 failed |
| Typecheck | `tsc --noEmit` | clean |
| Law-1 calibration | `node scripts/law1_calibrate.ts` | CALIBRATED (bites) |
| Law-1 + budget | `node --expose-gc bench/frame_budget.ts` | PASS (0 B, 0 violations, 60k frames) |
| Native probe | `make -C native run` (lavapipe) | BIT-EXACT ×2 regimes |
| Browser rig | shard leg 4 (Chromium, COOP/COEP) | pass=true (WebGL2+Canvas2d bit-exact; WebGPU named-refused) |
| Shader drift | vitest `shaders.test.ts` | bundles == canonical text |
| Layout parity | vitest `layout_parity.test.ts` | TS == C, macro-for-macro + size fn |
| Kernel freeze | shard leg 5 | 0 diffs vs origin/main |
| Full shard | `bash ci/scripts/run_heddle_canvas_shard.sh` | **ALL LEGS GREEN** |
