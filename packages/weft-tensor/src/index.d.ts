// index.d.ts — @weft/tensor public typings (hand-written, layout-normative).
// Spec: docs/weft-tensor/LAYOUT-V1.md — offsets below are BYTE-OFFSET constants.

export declare const RING_MAGIC: readonly [number, number, number, number]; // "WEFT"
export declare const SLOT_MAGIC: readonly [number, number, number, number]; // "WFRM"
export declare const LAYOUT_VERSION: 1;
export declare const RING_HEADER_SIZE: 128;
export declare const SLOT_HEADER_SIZE: 64;
/** Ring header offsets (bytes): magic, version, header_size, slot_count, slot_stride, dtype, elem, shape, strides, schema, publish word, tick, flags, crc. */
export declare const OFF_MAGIC: 0;
export declare const OFF_LAYOUT_VERSION: 4;
export declare const OFF_HEADER_SIZE: 6;
export declare const OFF_SLOT_COUNT: 8;
export declare const OFF_SLOT_STRIDE: 12;
export declare const OFF_DTYPE_CODE: 16;
export declare const OFF_DTYPE_BITS: 17;
export declare const OFF_LANES: 18;
export declare const OFF_ELEM_SIZE: 20;
export declare const OFF_SHAPE: 24;      // u32 x8
export declare const OFF_STRIDES: 56;    // u32 x8, ELEMENTS (DLPack convention)
export declare const OFF_SCHEMA_ID: 88;  // u64
export declare const OFF_PRODUCER_SEQ: 96; // u64 publish word (lo word stored LAST)
export declare const OFF_TICK_HZ: 104;
export declare const OFF_FLAGS: 108;
export declare const OFF_HEADER_CRC: 112;
/** Slot header offsets (bytes, relative to slot base). */
export declare const SOFF_MAGIC: 0;
export declare const SOFF_PAYLOAD_LEN: 4;
export declare const SOFF_SEQ: 8;
export declare const SOFF_TIMESTAMP_NS: 16;
export declare const SOFF_DURATION_US: 24;
export declare const SOFF_SLOT_FLAGS: 28;
export declare const SOFF_FOURCC: 32;
export declare const SOFF_RANK: 36;
export declare const SOFF_PLANES: 37;
export declare const SOFF_PLANE_OFFSET: 40;
export declare const SOFF_PLANE_SIZE: 52;
export declare const SLOT_FLAG_COMMITTED: 1;
export declare const MAX_RANK: 8;

export declare const DLPackCode: {
  readonly INT: 0; readonly UINT: 1; readonly FLOAT: 2;
  readonly BFLOAT: 3; readonly COMPLEX: 4; readonly BOOL: 5;
};

export declare function dtypeKey(code: number, bits: number): number;
export declare function isValidDtype(code: number, bits: number): boolean;
export declare function ringHeaderCrc(dv: DataView): number;
export declare function f16FromBits(h: number): number;
export declare function f16ToBits(v: number): number;
export declare function fourccFromString(s: string): number;
export declare function fourccToString(u32: number): string;

export declare class LayoutError extends Error {
  code: string;
  constructor(code: string, message: string);
}

export interface RingLayout {
  version: number; headerSize: number; slotCount: number; slotStride: number;
  dtype: { code: number; bits: number; lanes: number };
  elemSize: number;
  shape: Uint32Array; strides: Uint32Array; rank: number;
  schemaId: number; schemaIdLo: number; schemaIdHi: number;
  tickHz: number; flags: number; payloadCap: number; byteLength: number;
}

export interface SlotMeta {
  seq: number; seqLo: number; seqHi: number;
  payloadLen: number; timestampLo: number; timestampHi: number;
  durationUs: number; flags: number; fourcc: number; rank: number; planes: number;
}

export declare function validateRingHeader(buffer: ArrayBufferLike): RingLayout;
export declare function readSlotHeader(dv: DataView, slotBase: number, out: SlotMeta): boolean;

/** Flyweight over one committed slot — reuse via ring.acquire*(); never retain. */
export declare class WeftTensorFrameView {
  get isBound(): boolean;
  get payloadLength(): number;
  get payloadByteOffset(): number;
  get timestampNs(): number;
  get timestampMs(): number;
  get fourcc(): string;
  get seq(): number;
  get ring(): WeftTensorRing;
  payloadView(): Uint8Array;
  copyPayloadInto(dest: Uint8Array | Uint8ClampedArray): number;
  get shape(): Uint32Array;
  get stridesElems(): Uint32Array;
  get rank(): number;
  get dtype(): { code: number; bits: number; lanes: number };
  get elemSize(): number;
  get schemaId(): number;
  getF16(flatIndex: number): number;
  getF32(flatIndex: number): number;
  getF64(flatIndex: number): number;
  getU8(flatIndex: number): number;
  getI8(flatIndex: number): number;
  getU16(flatIndex: number): number;
  getI16(flatIndex: number): number;
  getU32(flatIndex: number): number;
  getI32(flatIndex: number): number;
}

export interface CommitHandle {
  seq: number; slot: number;
  payloadU8: Uint8Array;
  payloadTyped: ArrayBufferView | null;
}

export declare class WeftTensorRing {
  static create(opts: {
    slotCount?: number; payloadCap: number;
    dtype?: { code: number; bits: number; lanes?: number };
    shape: number[]; schemaId?: bigint | number | { lo: number; hi: number };
    tickHz?: number; shared?: boolean; fourcc?: string;
  }): WeftTensorRing;
  static attach(buffer: ArrayBufferLike): WeftTensorRing;
  get buffer(): ArrayBufferLike;
  get isShared(): boolean;
  get layout(): RingLayout;
  get slotCount(): number;
  get slotStride(): number;
  get payloadCap(): number;
  get byteLength(): number;
  get producerSeq(): number;
  get producerSeqLoHi(): { lo: number; hi: number };
  get tickHz(): number;
  readonly stats: { commits: number; acquireCalls: number; acquireRetries: number; tornReads: number; overruns: number };
  beginCommit(): CommitHandle;
  finishCommit(handle: CommitHandle, byteLen: number, meta?: {
    ts?: bigint; tsLo?: number; tsHi?: number; timestampNs?: number;
    durationUs?: number; fourcc?: string | number; rank?: number; planes?: number;
  }): number;
  commit(src: ArrayBufferView, meta?: Parameters<WeftTensorRing['finishCommit']>[2]): number;
  acquireLatest(lastSeq?: number): WeftTensorFrameView | null;
  acquireFrame(seq: number): WeftTensorFrameView | null;
  waitForNewFrame(lastSeq: number, timeoutMs?: number, scheduler?: unknown): Promise<number | null>;
  describe(): string;
}

export interface RuntimeInfo {
  node: boolean; deno: boolean; bun: boolean; browser: boolean; worker: boolean;
  electron: boolean; sharedArrayBuffer: boolean; webCodecs: boolean; webgpu: boolean; atomics: boolean;
}
export declare function detectRuntime(g?: unknown): RuntimeInfo;
export declare function runtimeMatrix(g?: unknown): { where: string; features: string[] };
export declare function assertShareable(buffer: ArrayBufferLike, why: string): void;

export declare function rafPump(): { request(cb: (t: number) => void): number; cancel(id: number): void };
export declare function timeoutPump(intervalMs: number): { request(cb: (t: number) => void): number; cancel(id: number): void };

export declare class RingListener {
  constructor(ring: WeftTensorRing, opts?: {
    onFrame?: ((frame: WeftTensorFrameView) => void) | null;
    onStall?: ((stats: { ticks: number; frames: number; stalls: number }) => void) | null;
    pump?: { request(cb: () => void): number; cancel(id: number): void };
  });
  get running(): boolean;
  get lastSeq(): number;
  readonly stats: { ticks: number; frames: number; stalls: number };
  start(): this;
  stop(): this;
  setOnFrame(cb: (frame: WeftTensorFrameView) => void): this;
}

export declare class VideoFrameIngestor {
  constructor(ring: WeftTensorRing, opts?: { width?: number; height?: number; fourcc?: string });
  readonly stats: { ingested: number; asyncFastPath: number; syncFastPath: number; fallback: number; dropped: number };
  ingestWebCodecs(frame: {
    copyTo(dst: ArrayBufferView, opts?: { format?: string }): Promise<number> | number;
    displayWidth?: number; displayHeight?: number;
  }, meta?: Record<string, unknown>): number | null | Promise<number | null>;
  ingestImageData(imageData: { data: Uint8ClampedArray; width: number; height: number }, meta?: Record<string, unknown>): number;
  ingestRaw(rgba: Uint8Array, width: number, height: number, meta?: Record<string, unknown>): number;
}

export declare class AudioPcmFeeder {
  constructor(ring: WeftTensorRing, opts: { channels?: number; chunkSamples: number; sampleHz?: number });
  get channels(): number;
  get chunkSamples(): number;
  get sampleHz(): number;
  readonly stats: { chunks: number; samples: number; slotRolls: number; partials: number };
  feed(chunk: Float32Array, meta?: Record<string, unknown>): number;
  flush(meta?: Record<string, unknown>): number | null;
}

export declare class Canvas2DPlane {
  constructor(canvas: { getContext(kind: '2d', opts?: Record<string, unknown>): {
    createImageData(w: number, h: number): { data: Uint8ClampedArray; width: number; height: number };
    putImageData(img: unknown, x: number, y: number): void;
  } | null } | null, ring: WeftTensorRing, opts?: { width?: number; height?: number });
  get width(): number;
  get height(): number;
  readonly stats: { draws: number; frames: number; reused: number };
  draw(frame: WeftTensorFrameView | null | undefined): number;
}

export declare class WebGL2Plane {
  constructor(canvas: unknown, ring: WeftTensorRing, opts?: { width?: number; height?: number });
  get width(): number;
  get height(): number;
  readonly stats: { draws: number; frames: number; uploads: number };
  draw(frame: WeftTensorFrameView | null | undefined): number;
  dispose(): void;
}

export declare const MAX_BOXES: number;
export declare const BOX_STRIDE: 6;
export declare class OverlayScratch {
  constructor(maxBoxes?: number);
  readonly boxes: Float32Array;
  maxBoxes: number;
  count: number;
  clear(): void;
  pushBox(x: number, y: number, w: number, h: number, score: number, classId: number): boolean;
  boxAt(i: number, out: Float32Array | number[]): Float32Array | number[];
}
export declare function drawBoxes2D(ctx: {
  lineWidth: number; font: string; textBaseline: string;
  strokeStyle: unknown; fillStyle: unknown;
  strokeRect(x: number, y: number, w: number, h: number): void;
  fillText(t: string, x: number, y: number): void;
}, scratch: OverlayScratch, style?: Record<string, unknown>): number;
export declare const COCO17_EDGES: Int8Array;
export declare function drawSkeleton2D(ctx: {
  lineWidth: number; strokeStyle: unknown;
  beginPath(): void; moveTo(x: number, y: number): void;
  lineTo(x: number, y: number): void; stroke(): void;
}, keypoints: Float32Array, count: number, confidence?: number): void;

export declare function fabricFingerprint(): string;
