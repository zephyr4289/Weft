// index.d.ts — @weft/heddle-hotplane type surface (v0.1).
export interface WHP2Config {
  mode: number;          // 0 ring | 1 state
  laneCount: number;     // 1..64
  sampleSize: number;    // 1..4080
  slotCapacity: number;  // per lane
  statKind?: number;     // 0 u64 | 1 i64 | 2 f64
  flags?: number;        // WHP2_F_*
}

export interface LaneStats {
  minRaw: bigint;
  maxRaw: bigint;
  currentRaw: bigint;
  commitCount: bigint;
}

export interface BBox {
  min: number;
  max: number;
  packed: bigint;
}

export interface PlaneDescriptor {
  magic: number;
  verMajor: number;
  verMinor: number;
  headerSize: number;
  layoutRev: number;
  mode: number;
  laneCount: number;
  laneStride: number;
  sampleSize: number;
  slotCapacity: number;
  statKind: number;
  flags: number;
  planeSize: bigint;
  createStampNs: bigint;
  multiProducer: boolean;
}

export declare class HotPlane {
  constructor(buffer: SharedArrayBuffer | ArrayBuffer,
              opts?: { offset?: number; role?: number });
  readonly mode: number;
  readonly laneCount: number;
  readonly laneStride: number;
  readonly sampleSize: number;
  readonly slotCapacity: number;
  readonly statKind: number;
  readonly flags: number;
  readonly planeSize: bigint;
  readonly createStampNs: bigint;
  readonly beginSeq: bigint;
  readonly commitSeq: bigint;
  readonly dirtyMask: bigint;
  readonly renderFrameId: bigint;
  readonly heartbeatNs: bigint;
  readonly dirtyTransitions: bigint;
  readonly epoch: bigint;
  harvestMask(): bigint;
  remark(bits: bigint): bigint;
  frameCommit(): bigint;
  readCell(lane: number, cell: number, out: Uint8Array,
           maxRetries?: number): number;
  readSlot(lane: number, seq: bigint, out: Uint8Array,
           maxRetries?: number): number;
  ringHead(lane: number): bigint;
  laneStats(lane: number, maxRetries?: number): LaneStats;
  bboxHarvest(lane: number, maxRetries?: number): BBox;
  describe(): PlaneDescriptor;
}

export declare const MAGIC: number;
export declare function crc32(bytes: Uint8Array, off?: number,
                              len?: number): number;
export declare function planeSize(cfg: WHP2Config): bigint;
export declare function slotStride(sampleSize: number): number;
export declare function cellStride(sampleSize: number): number;
export declare function align64(x: number): number;
