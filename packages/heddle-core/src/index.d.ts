// @weft/heddle-core — public type surface (HPL1 Hot-Plane engine).
// Normative layout: docs/heddle2/HPL1-LAYOUT-V1.md

export declare const HPL1: {
  readonly OK: 0; readonly BAD_MAGIC: 1; readonly BAD_VERSION: 2;
  readonly NOT_LITTLE_ENDIAN: 3; readonly CAPACITY_MISMATCH: 4;
  readonly LANE_OUT_OF_RANGE: 5; readonly TORN_SEQLOCK: 6;
  readonly EPOCH_CHANGED: 7; readonly PLANE_DETACHED: 8;
  readonly CONTEXT_LOST: 9; readonly TAB_HIDDEN: 10; readonly WORKER_CRASH: 11;
  readonly BAD_RENDER_ENGINE: 12; readonly INVALID_SAMPLE: 13;
  readonly RING_UNDERRUN: 14;
};
export declare const HPL1_NAME: readonly string[];
export declare class Hpl1Error extends Error {
  code: number;
  hpl1: string;
  constructor(code: number, message: string);
}

export declare const HEADER_SIZE: 128;
export declare const LANE_CTRL_STRIDE: 64;
export declare const MAGIC_U32: number;
export declare const MAGIC_BYTES: readonly [number, number, number, number];
export declare const VERSION: 1;
export declare const FLAG_LE_REQUIRED: number;
export declare const FLAG_EPOCH_STABLE: number;
export declare const LANE_FLAG_ACTIVE: number;
export declare const LANE_FLAG_MANUAL: number;
export declare const MAX_LANES: number;
export declare const HDR: Record<string, number>;
export declare const LANE: Record<string, number>;

export declare function align8(x: number): number;
export interface Hpl1Geometry {
  dirtyWords: number; laneCtrlBase: number; ringBase: number; totalBytes: number;
  laneCount: number; samplesPerLane: number; flags: number; tickHz: number;
}
export declare function deriveGeometry(laneCount: number, samplesPerLane: number): {
  dirtyWords: number; laneCtrlBase: number; ringBase: number; totalBytes: number;
};
export declare function isPow2(x: number): boolean;
export declare function validatePlane(
  buffer: ArrayBufferLike, byteOffset?: number, byteLength?: number,
): Hpl1Geometry;
export declare function initHeader(
  dv: DataView, laneCount: number, samplesPerLane: number,
  tickHz: number, epoch: number,
): { dirtyWords: number; laneCtrlBase: number; ringBase: number; totalBytes: number };

export interface LaneOut {
  seqLo: number; seqHi: number;
  current: number; min: number; max: number; avg: number;
  samplesSeenLo: number; samplesSeenHi: number;
  head: number; flags: number;
  publishNsLo: number; publishNsHi: number;
  drops: number;
}
export interface HeaderOut {
  publishSeqLo: number; publishSeqHi: number;
  epochLo: number; epochHi: number;
  lastPublishNsLo: number; lastPublishNsHi: number;
  framesDroppedLo: number; framesDroppedHi: number;
  globalMin: number; globalMax: number; globalAvg: number; globalCurrent: number;
  tickHz: number; flags: number;
}
export declare function makeLaneOut(): LaneOut;
export declare function makeHeaderOut(): HeaderOut;

export interface PlaneOptions {
  byteOffset?: number; byteLength?: number; maxTries?: number;
}
export declare class HotPlaneView {
  constructor(buffer: ArrayBufferLike, opts?: PlaneOptions);
  readonly geo: Hpl1Geometry;
  readonly laneCount: number;
  readonly samplesPerLane: number;
  tears: number;
  dirtyCount: number;
  readHeader(out: HeaderOut): number;
  readLane(lane: number, out: LaneOut): number;
  readRecent(lane: number, k: number, outF64: Float64Array): number;
  scanDirty(changedOut: Uint32Array | null): number;
}
export declare class HotPlaneProducer {
  constructor(buffer: ArrayBufferLike, opts?: PlaneOptions);
  static create(opts: {
    laneCount: number; samplesPerLane: number; tickHz?: number; epoch?: number;
  }): HotPlaneProducer;
  readonly geo: Hpl1Geometry;
  readonly planeBuffer: ArrayBufferLike;
  publishLane(lane: number, value: number, nowNs: number): number;
  beginBatch(): void;
  publishTick(values: Float64Array | number[], count: number, nowNs: number): void;
  addDrops(n: number): void;
  epochRestart(nextEpoch?: number): number;
}
export interface FrameCtx {
  frame: number; nowNs: number; skipped: number; lateNs: number;
}
export declare class FrameScheduler {
  constructor(opts?: { hz?: number; raf?: ((cb: () => void) => number) | null; now?: () => number });
  readonly hz: number;
  rendered: number; skipped: number; pauseCount: number; resumeCount: number;
  start(callback: (ctx: FrameCtx) => void): FrameScheduler;
  startWithTimer(callback: (ctx: FrameCtx) => void, ms: number): FrameScheduler;
  stop(): FrameScheduler;
  pump(nowNs: number): boolean;
  pause(): FrameScheduler;
  resume(nowNs?: number): FrameScheduler;
}
