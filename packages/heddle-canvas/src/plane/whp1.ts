// whp1.ts — the WHP1 (Weft Hot-Plane v1) memory contract, single source of
// truth for the TS side. RFC-0022 §3. The native twin is
// native/whp1_layout.h; test/layout_parity.test.ts cross-checks the numbers
// below against that header's macros so drift is a CI failure, not a
// debugging session.
//
// WHY A FROZEN LAYOUT: heddle-2.0's whole thesis (Pillar 4 directive) is
// that the render loop NEVER repacks, converts or copies producer bytes —
// the GPU consumes the producer's own memory. That is only sound if every
// offset, stride and alignment is a written-down contract both sides
// compile against. Engineer 1's Hot-Plane engine writes this layout; the
// heddle-canvas HAL reads it; the Vulkan probe (native/) round-trips it
// through a real ICD so the native reading is MEASURED, not declared.
//
// LAW 2 (explicit little-endian + alignment, docs/PHILOSOPHY.md):
//   * plane header  = exactly 64 B (one cache line)
//   * lane table    = 16 descriptors × 128 B, each 128-B aligned
//   * data region   = starts at WHP1_DATA_START (128-B aligned, FIXED)
//   * every lane data offset MUST be 128-B aligned
//   * row-lane strides are 64 B (ladder/candle) or 128 B (point cloud) so
//     the same words feed a WGSL storage buffer, a GLSL 300 es vertex
//     attribute and a Metal buffer without re-swizzling
//   * all integers little-endian (the platform ABI of every tier target)
//
// THE PER-LANE 64-BIT DIRTY WORD, HONESTLY SPLIT: JS Atomics cannot do a
// 64-bit read-modify-write without BigInt — and BigInt arithmetic BOXES,
// a Law-1 violation inside the frame loop. The consumer therefore aliases
// the word with an Int32Array and read-clears it as TWO 32-bit atomic
// exchanges (src/plane/dirty_mask.ts — no-lost-update argument there).
// The PRODUCER (Engineer 1's engine, the synthetic producer, the native
// probe) uses the single 64-bit BigUint64Array OR road — the word is at a
// descriptor +0x30, 8-B aligned, so one atomic OR raises a whole range.
// Both roads are bit-identical; the battery proves the equivalence.

/** Plane magic: bytes "WPL1" read as a little-endian u32. */
export const WHP1_MAGIC = 0x314c5057;
/** Lane-descriptor magic: bytes "WNL1" read as a little-endian u32. */
export const WHP1_LANE_MAGIC = 0x314c4e57;
/** WHP1 layout version this package speaks (and requires). */
export const WHP1_VERSION = 1;
/** Plane header size in bytes — one x86/ARM cache line (Law 2). */
export const WHP1_HEADER_BYTES = 64;
/** Maximum lanes a v1 plane can describe. */
export const WHP1_MAX_LANES = 16;
/**
 * Lane table start: first 128-B boundary after the 64-B header (the
 * 64-B gap 0x40..0x80 is reserved for header growth — plane-level
 * counters a future version may add without moving the table).
 */
export const WHP1_LANE_TABLE = 0x80;
/** Bytes per lane descriptor (Law 2: 128-B granularity). */
export const WHP1_LANE_STRIDE_TABLE = 128;
/**
 * Data region start. FIXED (not "computed") so every tier and the native
 * probe agree without arithmetic that could drift:
 * 0x80 + 16 × 128 = 0x880.
 */
export const WHP1_DATA_START = 0x880;

// --- plane header offsets (bytes) -----------------------------------------
export const WHP1_OFF_MAGIC = 0x00;
export const WHP1_OFF_VERSION = 0x04;
export const WHP1_OFF_HEADER_BYTES = 0x08;
export const WHP1_OFF_LANE_COUNT = 0x0c;
export const WHP1_OFF_EPOCH = 0x10; // ATOMIC u32: plane liveness heartbeat
export const WHP1_OFF_PRODUCER_SEQ = 0x18; // ATOMIC u32: total publications
export const WHP1_OFF_FLAGS = 0x1c; // bit0 = teardown requested
export const WHP1_OFF_DATA_START = 0x20; // u64: must equal WHP1_DATA_START
export const WHP1_OFF_PLANE_BYTES = 0x28; // u64: full SAB size

// --- lane descriptor offsets (bytes within one 128-B descriptor) ----------
export const WHP1_LANE_OFF_MAGIC = 0x00;
export const WHP1_LANE_OFF_KIND = 0x04;
export const WHP1_LANE_OFF_DTYPE = 0x08;
export const WHP1_LANE_OFF_GRANULARITY = 0x0c; // elements per dirty bit
export const WHP1_LANE_OFF_OFFSET = 0x10; // u64: byte offset into the plane
export const WHP1_LANE_OFF_CAPACITY = 0x18; // u64: element count
export const WHP1_LANE_OFF_STRIDE = 0x20; // u64: bytes per element
export const WHP1_LANE_OFF_WRITE_POS = 0x28; // ATOMIC u32: total published
export const WHP1_LANE_OFF_SEQ = 0x2c; // ATOMIC u32: publication seq / tear fence
export const WHP1_LANE_OFF_DIRTY_LO = 0x30; // ATOMIC u32: dirty bits 0..31
export const WHP1_LANE_OFF_DIRTY_HI = 0x34; // ATOMIC u32: dirty bits 32..63
export const WHP1_LANE_OFF_FLAGS = 0x38; // bit0 = lane active

/** Lane kinds (u32 at WHP1_LANE_OFF_KIND) — the v1 set, 0-based. */
export const HP_KIND = {
  /** Oscilloscope / waveform lane: raw f32 samples, stride 4. */
  WAVEFORM_F32: 0,
  /** Order-book depth ladder row: price, size, side, pad… stride 64. */
  DEPTH_LADDER_F32: 1,
  /** Candlestick row: open, high, low, close, volume, pad… stride 64. */
  CANDLE_OHLC_F32: 2,
  /** Point-cloud row: pos.xyz+size, quat, rgba — stride 128. */
  POINTCLOUD_QUAT_F32: 3,
} as const;
export type HpKind = (typeof HP_KIND)[keyof typeof HP_KIND];

/** Element dtype. v1 renders F32 only — anything else is a named refusal. */
export const HP_DTYPE = { F32: 1 } as const;

/** Stride law per kind: the ONLY strides a v1 lane may declare. */
export const HP_KIND_STRIDE: Record<number, number> = {
  [HP_KIND.WAVEFORM_F32]: 4,
  [HP_KIND.DEPTH_LADDER_F32]: 64,
  [HP_KIND.CANDLE_OHLC_F32]: 64,
  [HP_KIND.POINTCLOUD_QUAT_F32]: 128,
};

/** u32 words per row element (stride / 4) — raster loop unroll count. */
export const HP_KIND_ROW_WORDS: Record<number, number> = {
  [HP_KIND.WAVEFORM_F32]: 1,
  [HP_KIND.DEPTH_LADDER_F32]: 16,
  [HP_KIND.CANDLE_OHLC_F32]: 16,
  [HP_KIND.POINTCLOUD_QUAT_F32]: 32,
};

// --- row layouts (u32 word indices within one row) ------------------------
/** DEPTH_LADDER row: [0]=price f32, [1]=size f32, [2]=side u32 (0 bid/1 ask). */
export const HP_LADDER_W_PRICE = 0;
export const HP_LADDER_W_SIZE = 1;
export const HP_LADDER_W_SIDE = 2;
/** CANDLE row: [0..3]=O,H,L,C f32, [4]=volume f32. */
export const HP_CANDLE_W_OHLC = 0; // vec4
export const HP_CANDLE_W_VOLUME = 4;
/** POINTCLOUD row: [0..3]=pos.xyz+size f32, [4..7]=quat xyzw, [8..11]=rgba. */
export const HP_PC_W_POSSIZE = 0; // vec4
export const HP_PC_W_QUAT = 4; // vec4
export const HP_PC_W_COLOR = 8; // vec4

/** Sanity ceiling for a single lane's capacity (64 Mi elements = 256 MiB f32). */
export const WHP1_MAX_LANE_CAPACITY = 1 << 26;

/** Plane flag: producer requests teardown; consumers drain and stop. */
export const WHP1_FLAG_TEARDOWN = 1 << 0;
/** Lane flag: lane is active (mapped by its owning renderer). */
export const WHP1_LANE_FLAG_ACTIVE = 1 << 0;

/**
 * Total SAB size for a lane set — the writer-side arithmetic
 * (HotPlane.create). Lanes are packed from WHP1_DATA_START, each 128-B
 * aligned (Law 2). Pure function of numbers, no allocation.
 */
export function whp1PlaneBytes(lanes: readonly { stride: number; capacity: number }[]): number {
  let off = WHP1_DATA_START;
  for (let i = 0; i < lanes.length; i++) {
    off = (off + 127) & ~127;
    off += lanes[i].stride * lanes[i].capacity;
  }
  return (off + 127) & ~127;
}
