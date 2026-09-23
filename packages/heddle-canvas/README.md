# @weft/heddle-canvas

> **heddle-2.0 — the 240 FPS hardware canvas engine.** Hot-Plane
> `SharedArrayBuffer` memory bound directly into WebGPU / WebGL2 /
> Canvas2D pipelines. Zero per-frame heap allocation, dirty-mask
> selective rasterization, 1,000,000-point waveforms.
> [RFC-0022](../../rfcs/0022-heddle2-hardware-canvas-engine.md) ·
> [D-42 audit](../../reports/D-42-HEDDLE2-GRAPHICS.md) · Pillar 4.

## What it is

A render engine whose frame loop never allocates, never touches the DOM,
and never copies producer bytes: Engineer 1's Hot-Plane memory is bound
straight into GPU storage/vertex buffers, only mutated lanes are
uploaded and drawn, and the same min/max decimation runs **bit-exactly**
on every tier (WGSL compute, GLSL transform feedback, CPU) — compared by
hash, not by screenshot.

```
producer ──lock-free──▶ WHP1 plane (SAB) ──dirty bits──▶ HeddleEngine
                              │                            │ sub-range
                              └── lane views (zero-copy) ──▶ upload + draw
                                                           ▼
                              Tier 1 WebGPU · Tier 2 WebGL2 · Tier 3 Canvas2D
```

## Quick start

```ts
import {
  HotPlane, SynthProducer, HeddleEngine, NullHAL, acquireWebGL2, WebGL2HAL,
  HP_KIND,
} from '@weft/heddle-canvas';

// 1. The plane (or open one Engineer 1's engine built — same bytes).
const plane = HotPlane.create([
  { kind: HP_KIND.WAVEFORM_F32, capacity: 65536, stride: 4, granularity: 1024 },
]);

// 2. A backend (tier ladder in the browser; NullHAL anywhere).
const canvas = document.createElement('canvas');
const acq = acquireWebGL2(canvas); // or acquireWebGPU(navigator.gpu)
const engine = new HeddleEngine(plane, new WebGL2HAL(acq.gl, acq.context), {
  canvasWidth: 1280, canvasHeight: 720, tickHz: 240, columnCount: 1280,
});

// 3. Produce (your engine replaces SynthProducer — the contract is the dep).
const synth = new SynthProducer(plane);
synth.pumpWaveform(417, t);

// 4. Frames — driven by rAF (engine.start()) or your own clock.
engine.tick(performance.now() * 1000);
```

Clean lanes cost nothing: a lane whose 64-bit dirty word reads zero is
skipped entirely — no upload, no draw call (`stats.skippedCleanLanes`).

## The contract in one screen

- **Plane:** 64-B header, 16 × 128-B lane descriptors, data at `0x880`,
  every lane 128-B aligned (Law 2). Malformed planes are refused with
  named codes (`HC_E_PLANE_*`, `HC_E_LANE_*`) — the battery makes every
  code fire.
- **Lane kinds:** `WAVEFORM_F32` (stride 4), `DEPTH_LADDER_F32`,
  `CANDLE_OHLC_F32` (stride 64), `POINTCLOUD_QUAT_F32` (stride 128).
- **Dirty protocol:** producers mark ranges with one atomic 64-bit OR;
  the render loop read-clears with two 32-bit exchanges — no locks, no
  lost updates, no BigInt in the hot path.
- **Window walk:** `vis = min(write_pos, capacity)`,
  `slot(j) = (window_start - vis + j + capacity) % capacity` — identical
  in every shader dialect and the CPU oracle.

## Gates (all runnable headless)

```bash
npm test                          # 66-test battery (contract, protocol, engine, shaders, parity)
node scripts/law1_calibrate.ts    # the allocation profiler proves it can bite
node --expose-gc bench/frame_budget.ts   # 60,000 frames: 0 B engine allocations, 0 budget violations
make -C native run                # Vulkan probe: plane-as-SSBO, bit-exact (needs an ICD)
bash ../../ci/scripts/run_heddle_canvas_shard.sh   # the whole pillar, one command
```

## Honest boundaries

- The 240 FPS hardware present-rate lock needs hardware; everything
  software-provable is proven (see D-42 §1).
- WebGPU requires an adapter; where absent the ladder reports a named
  refusal and steps down — never a silent failure.
- v1 renders F32 lanes of the four kinds above; anything else is a
  named refusal, and every extension is a version bump.
