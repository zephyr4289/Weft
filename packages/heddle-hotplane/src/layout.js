// layout.js — the WHP2 (Weft Hot-Plane v2) normative layout, mirrored
// byte-for-byte from HOTPLANE-LAYOUT-V2 (docs/heddle/) and
// core/c/heddle/heddle_hotplane.h. Every offset here is a normative
// constant; the golden interop test (test/golden.test.mjs) proves this
// file against a C-engine-generated plane image.
//
// All multibyte fields are LITTLE-ENDIAN (Law 2). All lanes, slots and
// cells are 64-byte cacheline-aligned.

export const MAGIC = 0x57485032;          // "WHP2"
export const LAYOUT_MAJOR = 2;
export const LAYOUT_MINOR = 0;
export const LAYOUT_REV = 1;

export const HEADER_SIZE = 128;
export const LANE_DESC_SIZE = 64;
export const SLOT_HEADER_SIZE = 16;
export const MIN_SLOT_STRIDE = 64;
export const MAX_LANES = 64;
export const MIN_CAPACITY = 2;
export const MAX_CAPACITY = 1 << 20;
export const MAX_SAMPLE_SIZE = 4080;

export const MODE_RING = 0;
export const MODE_STATE = 1;

export const STAT_U64 = 0;
export const STAT_I64 = 1;
export const STAT_F64 = 2;

export const F_MULTI_PRODUCER = 1 << 0;
export const F_BBOX = 1 << 1;
export const F_STATS = 1 << 2;

export const LANE_F_ACTIVE = 1 << 0;
export const LANE_F_BP_MARK = 1 << 1;
export const LANE_F_OVERRUN = 1 << 2;

export const BBOX_EMPTY = 0xffffffff00000000n;

export const ENDIAN_CANARY = 0x0dd0c0de;
export const STATIC_CRC_COVER_OFF = 0x10;
export const STATIC_CRC_COVER_LEN = 0x30;

export const ROLE_PRODUCER = 1;
export const ROLE_CONSUMER = 2;
export const ROLE_BOTH = 3;

// --- error ladder (mirrors heddle_err_t) ---------------------------------
export const OK = 0;
export const E_ARG = 1;
export const E_MAGIC = 2;
export const E_VERSION = 3;
export const E_LAYOUT = 4;
export const E_ENDIAN = 5;
export const E_CFG_CRC = 6;
export const E_REGION_SIZE = 7;
export const E_CAPACITY = 8;
export const E_LANE_OVERFLOW = 9;
export const E_PAYLOAD = 10;
export const E_MODE = 11;
export const E_STATE = 12;
export const E_SEQ_TORN = 13;
export const E_OVERRUN = 14;
export const E_NOT_PUBLISHED = 15;
export const E_UNSUPPORTED = 16;

export const ERR_NAMES = {
  [OK]: 'HEDDLE_OK',
  [E_ARG]: 'HEDDLE_E_ARG',
  [E_MAGIC]: 'HEDDLE_E_MAGIC',
  [E_VERSION]: 'HEDDLE_E_VERSION',
  [E_LAYOUT]: 'HEDDLE_E_LAYOUT',
  [E_ENDIAN]: 'HEDDLE_E_ENDIAN',
  [E_CFG_CRC]: 'HEDDLE_E_CFG_CRC',
  [E_REGION_SIZE]: 'HEDDLE_E_REGION_SIZE',
  [E_CAPACITY]: 'HEDDLE_E_CAPACITY',
  [E_LANE_OVERFLOW]: 'HEDDLE_E_LANE_OVERFLOW',
  [E_PAYLOAD]: 'HEDDLE_E_PAYLOAD',
  [E_MODE]: 'HEDDLE_E_MODE',
  [E_STATE]: 'HEDDLE_E_STATE',
  [E_SEQ_TORN]: 'HEDDLE_E_SEQ_TORN',
  [E_OVERRUN]: 'HEDDLE_E_OVERRUN',
  [E_NOT_PUBLISHED]: 'HEDDLE_E_NOT_PUBLISHED',
  [E_UNSUPPORTED]: 'HEDDLE_E_UNSUPPORTED',
};

// --- header field offsets (u64 register indices = offset / 8) -------------
export const OFF = {
  magic: 0x00,
  verMajor: 0x04,
  verMinor: 0x06,
  headerSize: 0x08,
  staticCrc32: 0x0c,
  layoutRev: 0x10,
  mode: 0x14,
  laneCount: 0x18,
  laneStride: 0x1c,
  sampleSize: 0x20,
  slotCapacity: 0x24,
  planeSize: 0x28,
  createStampNs: 0x30,
  statKind: 0x38,
  flags: 0x3c,
  // cacheline 1: the synchronization plane
  epoch: 0x40,
  beginSeq: 0x48,
  commitSeq: 0x50,
  dirtyMask: 0x58,
  renderFrameId: 0x60,
  heartbeatNs: 0x68,
  endianCanary: 0x70,
  reserved0: 0x74,
  dirtyTransitions: 0x78,
};

// --- lane descriptor field offsets (relative to the descriptor base) ------
export const LANE_OFF = {
  min: 0x00,
  max: 0x08,
  current: 0x10,
  beginSeq: 0x18,
  commitSeq: 0x20,
  commitCnt: 0x28,
  bbox: 0x30,
  flags: 0x38,
  misc: 0x3c,
};

// --- stride math (pure arithmetic, identical to the engine) ----------------
export function align64(x) {
  return Math.max(MIN_SLOT_STRIDE, (x + 63) & ~63);
}
export function slotStride(sampleSize) {
  return align64(SLOT_HEADER_SIZE + sampleSize);
}
export function cellStride(sampleSize) {
  return align64(sampleSize);
}
export function planeSize(cfg) {
  const stride = cfg.mode === MODE_RING
    ? slotStride(cfg.sampleSize)
    : cellStride(cfg.sampleSize);
  const laneStride = BigInt(cfg.slotCapacity) * BigInt(stride);
  return BigInt(HEADER_SIZE) +
    BigInt(cfg.laneCount) * BigInt(LANE_DESC_SIZE) +
    BigInt(cfg.laneCount) * laneStride;
}

// --- CRC-32/IEEE (poly 0xEDB88320 reflected, init/xorout 0xFFFFFFFF) ------
export function crc32(bytes, off = 0, len = bytes.length - off) {
  let crc = 0xffffffff;
  for (let i = 0; i < len; i++) {
    crc ^= bytes[off + i];
    for (let k = 0; k < 8; k++) {
      crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}
