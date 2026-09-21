// index.d.ts — @weft/robotics public API (managed side).

export declare const RNG1_HEADER_SIZE: number;
export declare const SLOT_HDR: number;
export declare const FMT_IMU6DOF: 1;
export declare const FMT_POINTS_F32: 2;
export declare const FMT_FRAME_DESC: 3;
export declare const FMT_BOXES_F32: 4;
export declare const FMT_NAMES: Map<number, string>;
export declare const E_SHORT: 1, E_MAGIC: 2, E_VERSION: 3, E_HEADER: 4, E_GEOMETRY: 5;

export declare class Rng1Error extends Error {
  code: number;
  constructor(code: number);
}

export declare class RecordView {
  dv: DataView;
  u8: Uint8Array;
  base: number;
  seq: number;
  topicId: number;
  tsNs: number;
  fmt: number;
  payloadOff: number;
  payloadLen: number;
  fmtName(): string;
  pointsView(): Float32Array;
  boxesView(): Float32Array;
  imuInto(out: Float64Array): Float64Array;
  frm1Into(out: {
    valid: boolean; width: number; height: number; stride: number;
    format: number; handleNs: number; handleLo: number; flags: number;
  }): void;
}

export declare function attachRing(buffer: ArrayBufferLike, opts?: {
  byteOffset?: number;
}): RingReader;

export declare class RingReader {
  slotSize: number;
  slotCount: number;
  view: RecordView;
  stats: Int32Array;
  committedSeq(): number;
  headerDropCount(): number;
  topicTable(): number[];
  acquire(topicId?: number): RecordView | null;
  drain(visit: (rec: RecordView) => boolean | void): void;
}

export declare class PointCloudEngine {
  constructor(gl: WebGL2RenderingContext, opts?: { capacityPoints?: number });
  ok: boolean;
  pointsDrawn: number;
  frames: number;
  draw(f32: Float32Array, pointCount: number, mvp: Float32Array): boolean;
  onContextLost(): void;
}

export declare function makeMvp(out: Float32Array, az: number, el: number, dist: number): Float32Array;

export declare class AttitudeEngine {
  constructor(ctx: CanvasRenderingContext2D | null);
  ok: boolean;
  frames: number;
  draw(qw: number, qx: number, qy: number, qz: number, w: number, h: number): boolean;
}

export declare function createWeftPointCloudViewer(React: unknown): (props: Record<string, unknown>) => unknown;
export declare function createWeftAttitudeIndicator(React: unknown): (props: Record<string, unknown>) => unknown;
