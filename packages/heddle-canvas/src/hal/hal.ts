// hal.ts — @weft/heddle-canvas the RenderHAL contract.
//
// WHY EXISTS: four backends (WebGPU / WebGL2 / Canvas2D / Null) must be
// behaviorally INTERCHANGEABLE — the frame loop (Law 1) and the renderers
// (Law 2 alignment) are written once against this interface, and the CI
// battery runs the SAME scenario battery against every backend, so a tier
// regression is a diff in observable behavior, not a "works on my GPU"
// story. The NullHAL (null_device.ts) is the executable spec of this
// contract; each real backend is verified against it.
//
// LAW 1 SHAPE: every method is one of exactly two kinds —
//   init-time:  initialize() / handleLoss() — may allocate freely;
//   frame-time: beginFrame/upload*/draw*/endFrame — MUST NOT allocate,
//               MUST NOT close over fresh state, MUST NOT touch BigInt.
// The static scanner (src/law1/static_scan.ts) enforces the frame-time
// half textually; the heap gate (60,000 frames) enforces it empirically.

import type { PlaneView, LaneView } from '../plane/hot_plane.ts';
import type { EngineConfig, HalStats } from './tier.ts';
import type { TierFallbackEvent } from '../errors.ts';

export interface RenderHAL {
  /** RENDER_TIERS.* id of this backend. */
  readonly tier: number;
  /** True only for native zero-copy maps (weft_render_bridge parity). */
  readonly directMapped: boolean;
  /** Live counters (HUD + audit source). */
  readonly stats: HalStats;
  /** Whether SAB views upload directly (false => pre-allocated staging). */
  readonly sabViewAccepted: boolean;
  /** Tier name for logs ('webgpu' | 'webgl2' | 'canvas2d' | 'null'). */
  readonly name: string;

  /**
   * Carve every GPU/resource pool (Law 1: the ONLY allocating method).
   * Throws HeddleError (HC_E_*) on any refusal — the ladder catches and
   * walks down.
   */
  initialize(plane: PlaneView, cfg: EngineConfig): void;

  /**
   * Law 4: a backend died (webglcontextlost / gpuDevice.lost). Returns the
   * degradation event for the ladder, or null when the backend recovered
   * in place (e.g. restored GL context after WEBGL_lose_context.restoreContext()).
   */
  handleLoss(reason: string): TierFallbackEvent | null;

  // --- frame hot path (zero allocation, all four backends) ---------------
  beginFrame(): void;
  /** Upload the f32 sample sub-range [elemStart, elemEndExcl) of a WAVEFORM lane. */
  uploadWaveform(lane: LaneView, elemStart: number, elemEndExcl: number): void;
  /** Upload the row sub-range of a strided lane (DEPTH/CANDLE/POINTCLOUD). */
  uploadRows(lane: LaneView, elemStart: number, elemEndExcl: number): void;
  /**
   * Decimate + draw the oscilloscope ribbon for a WAVEFORM lane.
   * `elemCount` = valid samples in the window = min(writePos, capacity);
   * `windowStart` = writePos % capacity — the slot where the NEWEST sample
   * lives + 1 (age order walks (windowStart-1+j) mod capacity). Every tier
   * implements the identical window walk so the decimation is bit-exact
   * across tiers (the cross-tier ==-gate).
   */
  drawOscillo(lane: LaneView, elemCount: number, windowStart: number): void;
  /** Draw the Level 2/3 depth ladder (instanced) for a DEPTH_LADDER lane. */
  drawDepthLadder(lane: LaneView, rowCount: number): void;
  /** Draw the OHLC candlesticks (instanced bodies + wicks) for a CANDLE lane. */
  drawCandles(lane: LaneView, rowCount: number): void;
  /** Draw the point cloud (instanced, quaternion-oriented) lane. */
  drawPointcloud(lane: LaneView, pointCount: number): void;
  endFrame(): void;
}
