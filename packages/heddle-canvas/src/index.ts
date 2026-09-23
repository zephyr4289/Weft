// index.ts — @weft/heddle-canvas public surface (Pillar 4 / RFC-0022).
//
// The export order mirrors the data flow: the plane contract first (the
// substrate everything binds to), then the dirty protocol, the HAL tier
// ladder, the frame engine, the Law-1 gates, and the oracle.

// --- the WHP1 plane contract (Engineer 1 seam) -----------------------------
export * from './plane/whp1.ts';
export { HotPlane } from './plane/hot_plane.ts';
export type { PlaneView, LaneView, HpLaneSpec } from './plane/hot_plane.ts';
export {
  createDirtyBits,
  createDirtyRange,
  takeDirtyBits,
  peekDirtyBits,
  dirtyRangeOf,
  markDirty64,
  markDirty32,
  producerCommit,
} from './plane/dirty_mask.ts';
export type { DirtyBits, DirtyRange } from './plane/dirty_mask.ts';
export { SynthProducer, mulberry32, synthSample } from './plane/synth.ts';

// --- the HAL contract + tier ladder ----------------------------------------
export type { RenderHAL } from './hal/hal.ts';
export { RENDER_TIERS, tierName, createHalStats, walkTierLadder, assertTierId } from './hal/tier.ts';
export type { RenderTierId, HalStats, EngineConfig, ProbeEnv, LadderResult } from './hal/tier.ts';
export { NullHAL } from './hal/null_device.ts';
export type { UploadRecord, DrawRecord } from './hal/null_device.ts';
export { GLContextStateMachine, acquireWebGL2, GL_CONTEXT_STATE } from './hal/webgl2/webgl2_context.ts';
export type { GLContextState, WebGL2AcquireResult } from './hal/webgl2/webgl2_context.ts';
export { WebGL2HAL, GL_FLOAT } from './hal/webgl2/webgl2_binder.ts';
export type { GL2 } from './hal/webgl2/webgl2_binder.ts';
export { GPUDeviceStateMachine, acquireWebGPU, GPU_DEVICE_STATE } from './hal/webgpu/webgpu_device.ts';
export type { GPUDeviceState, WebGPUAcquireResult, GPUDeviceLike } from './hal/webgpu/webgpu_device.ts';
export { WebGPUHAL } from './hal/webgpu/webgpu_binder.ts';
export { Canvas2DHAL } from './hal/canvas2d/canvas2d_blitter.ts';
export type { Canvas2DLike } from './hal/canvas2d/canvas2d_blitter.ts';

// --- the engine + budget ----------------------------------------------------
export { HeddleEngine } from './loop/frame_engine.ts';
export { FrameBudgetLedger } from './loop/budget.ts';

// --- the Law-1 gates --------------------------------------------------------
export { heapGate, heapGateAvailable, heapNow } from './law1/audit.ts';
export type { HeapGateResult } from './law1/audit.ts';
export { scanFrameMethods, extractMethodBody } from './law1/static_scan.ts';
export type { ScanViolation } from './law1/static_scan.ts';

// --- the cross-tier oracle --------------------------------------------------
export { decimateWindowMinMax, decimateHash, fnv1a32, bitsOf } from './renderers/cpu_oracle.ts';

// --- the error taxonomy (Law 4) ---------------------------------------------
export { HC_ERROR_CODES, HeddleError } from './errors.ts';
export type { HeddleErrorCode, TierFallbackEvent } from './errors.ts';
