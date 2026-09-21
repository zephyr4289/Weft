// src/layout.js — HPL1 offset math + fail-closed plane validation.
//
// Law 2: every multi-byte access below is EXPLICITLY little-endian (the `true`
// argument on DataView methods). No exceptions, no implicit-endian reads.
// u64 fields are lo/hi u32 pairs; `lo` lives at the field offset and is stored
// LAST by writers (lo-last publish ordering, HPL1 §4.2).
//
// This module is mirrored byte-exactly by packages/flutter-heddle (Dart) and
// packages/swift-heddle (Swift); the fixture manifests pin the constants.

import { HPL1, Hpl1Error } from './errors.js';

export const HEADER_SIZE = 128;
export const LANE_CTRL_STRIDE = 64;
export const MAGIC_U32 = 0x314c5048; // 'H','P','L','1' little-endian
export const MAGIC_BYTES = [0x48, 0x50, 0x4c, 0x31];
export const VERSION = 1;

export const FLAG_LE_REQUIRED = 1;
export const FLAG_EPOCH_STABLE = 2;

export const LANE_FLAG_ACTIVE = 1;
export const LANE_FLAG_MANUAL = 2;

export const MAX_LANES = 4096;

// Header field offsets (HPL1 §2)
export const HDR = {
  MAGIC: 0x00,
  VERSION: 0x04,
  FLAGS: 0x08,
  LANE_COUNT: 0x0c,
  SAMPLES_PER_LANE: 0x10,
  RING_MASK: 0x14,
  TICK_HZ: 0x18,
  RESERVED0: 0x1c,
  EPOCH: 0x20,
  PUBLISH_SEQ: 0x28,
  LAST_PUBLISH_NS: 0x30,
  FRAMES_DROPPED: 0x38,
  GLOBAL_MIN: 0x40,
  GLOBAL_MAX: 0x48,
  GLOBAL_AVG: 0x50,
  GLOBAL_CURRENT: 0x58,
  DIRTY_WORDS: 0x60,
  LANE_CTRL_STRIDE: 0x64,
  RING_BASE: 0x68,
  TOTAL_BYTES: 0x6c,
};

// Lane control block field offsets, relative to laneCtrlBase + lane*64 (HPL1 §4)
export const LANE = {
  SEQ: 0x00,       // u64 lo/hi: odd = write in progress
  CURRENT: 0x08,   // f64
  MIN: 0x10,       // f64
  MAX: 0x18,       // f64
  AVG: 0x20,       // f64
  SAMPLES_SEEN: 0x28, // u64 lo/hi
  HEAD: 0x30,      // u32 next ring write index (pre-mask)
  FLAGS: 0x34,     // u32 bit0 ACTIVE, bit1 MANUAL
  PUBLISH_NS: 0x38, // u64 lo/hi
  DROPS: 0x40,     // u32
};

export function align8(x) {
  return (x + 7) & ~7;
}

// Pure geometry derivation — the single source of truth for offsets.
export function deriveGeometry(laneCount, samplesPerLane) {
  const dirtyWords = Math.ceil(laneCount / 32);
  const laneCtrlBase = HEADER_SIZE + align8(4 * dirtyWords);
  const ringBase = align8(laneCtrlBase + LANE_CTRL_STRIDE * laneCount);
  const totalBytes = ringBase + laneCount * samplesPerLane * 8;
  return { dirtyWords, laneCtrlBase, ringBase, totalBytes };
}

export function isPow2(x) {
  return x >= 2 && (x & (x - 1)) === 0;
}

// Fail-closed validation of an existing plane. Throws Hpl1Error on ANY
// disagreement with the derived geometry (HPL1 §2 redundancy checks).
// `byteOffset`/`byteLength` allow a plane to live inside a larger region
// (Engineer 1's bridge may wrap an SAB that carries more than the plane).
export function validatePlane(buffer, byteOffset = 0, byteLength = -1) {
  if (!buffer) throw new Hpl1Error(HPL1.PLANE_DETACHED, 'no buffer given');
  if (byteOffset % 8 !== 0) {
    throw new Hpl1Error(HPL1.PLANE_DETACHED, `byteOffset ${byteOffset} not 8-aligned`);
  }
  const available = (byteLength >= 0 ? byteLength : buffer.byteLength) - byteOffset;
  if (available < HEADER_SIZE) {
    throw new Hpl1Error(HPL1.PLANE_DETACHED, `buffer too small: ${available} < ${HEADER_SIZE}`);
  }
  const dv = new DataView(buffer, byteOffset);
  const magic = dv.getUint32(HDR.MAGIC, true);
  if (magic !== MAGIC_U32) throw new Hpl1Error(HPL1.BAD_MAGIC, `got 0x${magic.toString(16)}`);
  const version = dv.getUint32(HDR.VERSION, true);
  if (version !== VERSION) throw new Hpl1Error(HPL1.BAD_VERSION, `got ${version}`);
  const flags = dv.getUint32(HDR.FLAGS, true);
  if ((flags & FLAG_LE_REQUIRED) === 0) {
    throw new Hpl1Error(HPL1.NOT_LITTLE_ENDIAN, 'flags bit0 (LE_REQUIRED) is clear');
  }
  const laneCount = dv.getUint32(HDR.LANE_COUNT, true);
  const samplesPerLane = dv.getUint32(HDR.SAMPLES_PER_LANE, true);
  if (laneCount < 1 || laneCount > MAX_LANES) {
    throw new Hpl1Error(HPL1.CAPACITY_MISMATCH, `laneCount ${laneCount} out of range`);
  }
  if (!isPow2(samplesPerLane)) {
    throw new Hpl1Error(HPL1.CAPACITY_MISMATCH, `samplesPerLane ${samplesPerLane} not a power of two >= 2`);
  }
  const ringMask = dv.getUint32(HDR.RING_MASK, true);
  if (ringMask !== samplesPerLane - 1) {
    throw new Hpl1Error(HPL1.CAPACITY_MISMATCH, `ringMask ${ringMask} != samplesPerLane-1`);
  }
  const geo = deriveGeometry(laneCount, samplesPerLane);
  const checks = [
    ['dirtyWords', dv.getUint32(HDR.DIRTY_WORDS, true), geo.dirtyWords],
    ['laneCtrlStride', dv.getUint32(HDR.LANE_CTRL_STRIDE, true), LANE_CTRL_STRIDE],
    ['ringBaseOffset', dv.getUint32(HDR.RING_BASE, true), geo.ringBase],
    ['totalBytes', dv.getUint32(HDR.TOTAL_BYTES, true), geo.totalBytes],
  ];
  for (let i = 0; i < checks.length; i++) {
    if (checks[i][1] !== checks[i][2]) {
      throw new Hpl1Error(
        HPL1.CAPACITY_MISMATCH,
        `header ${checks[i][0]} = ${checks[i][1]}, derived ${checks[i][2]}`,
      );
    }
  }
  if (available < geo.totalBytes) {
    throw new Hpl1Error(HPL1.PLANE_DETACHED, `buffer ${available}B smaller than plane ${geo.totalBytes}B`);
  }
  return { ...geo, laneCount, samplesPerLane, flags, tickHz: dv.getUint32(HDR.TICK_HZ, true) };
}

// Cold-path convenience: initialize a fresh plane in an (existing) buffer.
// Used by HotPlaneProducer.create; separated from hot-path code.
export function initHeader(dv, laneCount, samplesPerLane, tickHz, epoch) {
  const geo = deriveGeometry(laneCount, samplesPerLane);
  dv.setUint32(HDR.MAGIC, MAGIC_U32, true);
  dv.setUint32(HDR.VERSION, VERSION, true);
  dv.setUint32(HDR.FLAGS, FLAG_LE_REQUIRED, true);
  dv.setUint32(HDR.LANE_COUNT, laneCount, true);
  dv.setUint32(HDR.SAMPLES_PER_LANE, samplesPerLane, true);
  dv.setUint32(HDR.RING_MASK, samplesPerLane - 1, true);
  dv.setUint32(HDR.TICK_HZ, tickHz, true);
  dv.setUint32(HDR.RESERVED0, 0, true);
  // epoch/publishSeq lo/hi: epoch = 1 session; publishSeq starts at 0 (stable)
  dv.setUint32(HDR.EPOCH + 0, epoch >>> 0, true);
  dv.setUint32(HDR.EPOCH + 4, 0, true);
  dv.setUint32(HDR.PUBLISH_SEQ + 0, 0, true);
  dv.setUint32(HDR.PUBLISH_SEQ + 4, 0, true);
  dv.setUint32(HDR.DIRTY_WORDS, geo.dirtyWords, true);
  dv.setUint32(HDR.LANE_CTRL_STRIDE, LANE_CTRL_STRIDE, true);
  dv.setUint32(HDR.RING_BASE, geo.ringBase, true);
  dv.setUint32(HDR.TOTAL_BYTES, geo.totalBytes, true);
  return geo;
}
