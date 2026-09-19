// layout.js — WTR1 (Weft Tensor Ring v1) normative layout primitives.
//
// Spec: docs/weft-tensor/LAYOUT-V1.md (NORMATIVE).
// Law 2: every multi-byte access in this file is EXPLICITLY little-endian and
// byte-exact with the frozen C kernel envelope and the Python bridge.
// Law 3: standard Web primitives only (DataView, typed arrays) — runs on
// Node.js, Deno, Bun, browsers (main thread + workers), Electron.

/** ASCII magic of the ring header: "WEFT". */
export const RING_MAGIC = [0x57, 0x45, 0x46, 0x54]; // 'W' 'E' 'F' 'T'
/** ASCII magic of a slot header: "WFRM". */
export const SLOT_MAGIC = [0x57, 0x46, 0x52, 0x4d]; // 'W' 'F' 'R' 'M'

export const LAYOUT_VERSION = 1;
export const RING_HEADER_SIZE = 128;
export const SLOT_HEADER_SIZE = 64;

// Ring header offsets.
export const OFF_MAGIC = 0;
export const OFF_LAYOUT_VERSION = 4;
export const OFF_HEADER_SIZE = 6;
export const OFF_SLOT_COUNT = 8;
export const OFF_SLOT_STRIDE = 12;
export const OFF_DTYPE_CODE = 16;
export const OFF_DTYPE_BITS = 17;
export const OFF_LANES = 18;
export const OFF_ELEM_SIZE = 20;
export const OFF_SHAPE = 24;      // u32 x8
export const OFF_STRIDES = 56;    // u32 x8, in ELEMENTS (DLPack convention)
export const OFF_SCHEMA_ID = 88;  // u64
export const OFF_PRODUCER_SEQ = 96; // u64 — the publish word
export const OFF_TICK_HZ = 104;
export const OFF_FLAGS = 108;
export const OFF_HEADER_CRC = 112; // CRC-32/IEEE over bytes [0,96) ++ [104,112)

// Ring flags.
export const RING_FLAG_LITTLE_ENDIAN = 1;
export const RING_FLAG_SHARED_MEMORY = 2;

// Slot header offsets (relative to slot base).
export const SOFF_MAGIC = 0;
export const SOFF_PAYLOAD_LEN = 4;
export const SOFF_SEQ = 8;          // u64
export const SOFF_TIMESTAMP_NS = 16; // u64
export const SOFF_DURATION_US = 24;
export const SOFF_SLOT_FLAGS = 28;
export const SOFF_FOURCC = 32;
export const SOFF_RANK = 36;
export const SOFF_PLANES = 37;
export const SOFF_PLANE_OFFSET = 40; // u32 x3
export const SOFF_PLANE_SIZE = 52;   // u32 x3

/** Slot header flags: bit0 = COMMITTED (written last before publish). */
export const SLOT_FLAG_COMMITTED = 1;

/** Max rank this layout supports (shape[8]/strides[8]). */
export const MAX_RANK = 8;

/**
 * DLPack DLDataTypeCode — used VERBATIM on the wire so the Python DLPack
 * bridge maps dtype with zero translation (no table, no drift).
 */
export const DLPackCode = Object.freeze({
  INT: 0, UINT: 1, FLOAT: 2, BFLOAT: 3, COMPLEX: 4, BOOL: 5,
});

/** (code, bits) pairs accepted by V1, encoded as (code<<8)|bits. */
const VALID_DTYPES = new Set([
  0x008, 0x010, 0x020, 0x040,         // i8/i16/i32/i64   (kDLInt)
  0x108, 0x110, 0x120, 0x140,         // u8/u16/u32/u64   (kDLUInt)
  0x210, 0x220, 0x240,                // f16/f32/f64      (kDLFloat)
  0x508,                              // bool8            (kDLBool)
]);

export function dtypeKey(code, bits) { return (code << 8) | bits; }
export function isValidDtype(code, bits) { return VALID_DTYPES.has(dtypeKey(code, bits)); }

// ---------------------------------------------------------------------------
// CRC-32/IEEE (zlib-compatible) over the static header config.
// Table is built lazily ONCE (module-lifetime allocation, never per-frame).
// ---------------------------------------------------------------------------

let CRC_TABLE = null;
function crcTable() {
  if (CRC_TABLE === null) {
    CRC_TABLE = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = (c & 1) !== 0 ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
      CRC_TABLE[n] = c >>> 0;
    }
  }
  return CRC_TABLE;
}

/**
 * CRC-32/IEEE over the static ring-header config: bytes [0,96) ++ [104,112).
 * Excludes the mutable producer_seq ([96,104)) and the CRC field itself.
 */
export function ringHeaderCrc(dv) {
  const t = crcTable();
  let c = 0xffffffff;
  for (let i = 0; i < 96; i++) c = t[(c ^ dv.getUint8(i)) & 0xff] ^ (c >>> 8);
  for (let i = 104; i < 112; i++) c = t[(c ^ dv.getUint8(i)) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

// ---------------------------------------------------------------------------
// IEEE 754 half-precision (float16) codec — pure arithmetic, zero allocation.
// Golden vectors generated from numpy float16 (test/f16.test.mjs).
// ---------------------------------------------------------------------------

const _f32Scratch = new Float32Array(1);
const _i32Scratch = new Int32Array(_f32Scratch.buffer);

/** Decode a float16 bit pattern to a JS number (exact). */
export function f16FromBits(h) {
  const sign = (h & 0x8000) !== 0;
  const exp = (h & 0x7c00) >>> 10;
  const frac = h & 0x03ff;
  if (exp === 0x1f) {
    if (frac !== 0) return NaN;
    return sign ? -Infinity : Infinity;
  }
  if (exp === 0) {
    if (frac === 0) return sign ? -0 : 0;
    // Subnormal: value = frac * 2^-24 exactly.
    const m = frac * 2 ** -24;
    return sign ? -m : m;
  }
  const m = (1 + frac / 1024) * 2 ** (exp - 15);
  return sign ? -m : m;
}

/** Encode a JS number to float16 bits (round-to-nearest-even, numpy parity). */
export function f16ToBits(v) {
  _f32Scratch[0] = v;
  const x = _i32Scratch[0];
  const sign = (x >>> 16) & 0x8000;
  const exp32 = (x >>> 23) & 0xff;
  const mant32 = x & 0x7fffff;
  if (exp32 === 0xff) { // Inf / NaN
    if (mant32 !== 0) return sign | 0x7e00; // canonical quiet NaN
    return sign | 0x7c00;
  }
  let exp = exp32 - 112; // 127 - 15 re-bias
  if (exp >= 0x1f) return sign | 0x7c00; // overflow -> Inf
  if (exp <= 0) {
    if (exp < -10) return sign; // underflow -> signed zero
    // Subnormal: shift the implicit-1 mantissa, round-to-nearest-even.
    let mant = (mant32 | 0x800000) >>> (1 - exp);
    if ((mant & 0x1000) !== 0 && ((mant & 0x2000) !== 0 || (mant & 0x0fff) !== 0)) {
      mant += 0x2000;
    }
    return sign | (mant >>> 13);
  }
  let mant = mant32;
  if ((mant & 0x1000) !== 0 && ((mant & 0x2000) !== 0 || (mant & 0x0fff) !== 0)) {
    mant += 0x2000;
    if ((mant & 0x800000) !== 0) {
      mant = 0;
      exp += 1;
      if (exp >= 0x1f) return sign | 0x7c00;
    }
  }
  return sign | (exp << 10) | (mant >>> 13);
}

// ---------------------------------------------------------------------------
// fourcc helpers (LE ASCII in a u32).
// ---------------------------------------------------------------------------

export function fourccFromString(s) {
  // "RGBA" -> bytes 'R','G','B','A' -> LE u32 0x41424752
  const b0 = s.charCodeAt(0) & 0xff, b1 = s.charCodeAt(1) & 0xff,
        b2 = s.charCodeAt(2) & 0xff, b3 = s.charCodeAt(3) & 0xff;
  return ((b3 << 24) | (b2 << 16) | (b1 << 8) | b0) >>> 0;
}

export function fourccToString(u32) {
  return String.fromCharCode(u32 & 0xff, (u32 >>> 8) & 0xff, (u32 >>> 16) & 0xff, (u32 >>> 24) & 0xff);
}

// ---------------------------------------------------------------------------
// Validation — fail-closed with machine-readable codes (Law 4).
// ---------------------------------------------------------------------------

export class LayoutError extends Error {
  /**
   * @param {string} code machine-readable, e.g. "WTR1_BAD_MAGIC"
   * @param {string} message human explanation
   */
  constructor(code, message) {
    super(`[${code}] ${message}`);
    this.name = 'LayoutError';
    this.code = code;
  }
}

function magicMatches(dv, off, expected) {
  for (let i = 0; i < 4; i++) {
    if (dv.getUint8(off + i) !== expected[i]) return false;
  }
  return true;
}

/** Row-major stride sanity: strides[i] >= strides[i+1] * shape[i+1] (elements). */
function stridesSane(shape, strides, rank) {
  for (let i = 0; i < rank - 1; i++) {
    if (strides[i] < strides[i + 1] * shape[i + 1]) return false;
  }
  if (rank > 0 && strides[rank - 1] < 1) return false;
  return true;
}

/**
 * Validate a WTR1 ring header at the start of `buffer` (Law 4 boundary).
 * Fail-closed: throws LayoutError with a code for EVERY corruption class.
 *
 * @param {ArrayBufferLike} buffer
 * @returns {RingLayout} preallocated layout info object (allocated once, at attach)
 */
export function validateRingHeader(buffer) {
  const byteLength = buffer.byteLength;
  if (byteLength < RING_HEADER_SIZE) {
    throw new LayoutError('WTR1_SHORT', `buffer is ${byteLength}B, ring header needs ${RING_HEADER_SIZE}B`);
  }
  const dv = new DataView(buffer);
  if (!magicMatches(dv, OFF_MAGIC, RING_MAGIC)) {
    throw new LayoutError('WTR1_BAD_MAGIC', 'ring magic is not "WEFT"');
  }
  const version = dv.getUint16(OFF_LAYOUT_VERSION, true);
  if (version !== LAYOUT_VERSION) {
    throw new LayoutError('WTR1_BAD_VERSION', `layout_version ${version} != ${LAYOUT_VERSION}`);
  }
  const headerSize = dv.getUint16(OFF_HEADER_SIZE, true);
  if (headerSize !== RING_HEADER_SIZE) {
    throw new LayoutError('WTR1_BAD_HEADER_SIZE', `header_size ${headerSize} != ${RING_HEADER_SIZE}`);
  }
  const slotCount = dv.getUint32(OFF_SLOT_COUNT, true);
  if (slotCount < 2) {
    throw new LayoutError('WTR1_BAD_SLOT_COUNT', `slot_count ${slotCount} < 2`);
  }
  const slotStride = dv.getUint32(OFF_SLOT_STRIDE, true);
  if ((slotStride & 63) !== 0 || slotStride < SLOT_HEADER_SIZE) {
    throw new LayoutError('WTR1_BAD_SLOT_STRIDE', `slot_stride ${slotStride} is not 64-aligned / < 64`);
  }
  const code = dv.getUint8(OFF_DTYPE_CODE);
  const bits = dv.getUint8(OFF_DTYPE_BITS);
  const lanes = dv.getUint16(OFF_LANES, true);
  if (!isValidDtype(code, bits) || lanes !== 1) {
    throw new LayoutError('WTR1_BAD_DTYPE', `dtype ${code}/${bits}bits x${lanes}lanes not supported in V1`);
  }
  const elemSize = dv.getUint32(OFF_ELEM_SIZE, true);
  if (elemSize !== (bits >>> 3) * lanes) {
    throw new LayoutError('WTR1_BAD_ELEM_SIZE', `elem_size ${elemSize} != ${(bits >>> 3) * lanes}`);
  }
  // shape/strides rank
  const shape = new Uint32Array(MAX_RANK);
  const strides = new Uint32Array(MAX_RANK);
  let rank = 0;
  for (let d = 0; d < MAX_RANK; d++) {
    const s = dv.getUint32(OFF_SHAPE + 4 * d, true);
    if (s !== 0) rank = d + 1;
    shape[d] = s;
  }
  for (let d = 0; d < MAX_RANK; d++) strides[d] = dv.getUint32(OFF_STRIDES + 4 * d, true);
  if (rank === 0) throw new LayoutError('WTR1_BAD_RANK', 'shape is all-zero (rank-0 tensors not supported in V1)');
  if (!stridesSane(shape, strides, rank)) {
    throw new LayoutError('WTR1_BAD_STRIDES', 'strides are not a sane row-major layout');
  }
  // producer_seq bound: managed Number path is exact below 2^53 (Pillar 1 Lo/Hi discipline)
  const seqHi = dv.getUint32(OFF_PRODUCER_SEQ + 4, true);
  if (seqHi >= 0x200000) {
    throw new LayoutError('WTR1_SEQ_OVERFLOW', 'producer_seq exceeds 2^53 — unsupported by the managed Number path');
  }
  const flags = dv.getUint32(OFF_FLAGS, true);
  if ((flags & RING_FLAG_LITTLE_ENDIAN) === 0) {
    throw new LayoutError('WTR1_NOT_LITTLE_ENDIAN', 'flags bit0 (little-endian) not set — refusing to guess byte order');
  }
  const wantCrc = dv.getUint32(OFF_HEADER_CRC, true);
  const gotCrc = ringHeaderCrc(dv);
  if (wantCrc !== gotCrc) {
    throw new LayoutError('WTR1_BAD_CRC', `header crc ${wantCrc.toString(16)} != computed ${gotCrc.toString(16)}`);
  }
  const need = headerSize + slotCount * slotStride;
  if (byteLength < need) {
    throw new LayoutError('WTR1_SHORT_RING', `buffer ${byteLength}B < header + ${slotCount} slots (${need}B)`);
  }

  const schemaLo = dv.getUint32(OFF_SCHEMA_ID, true);
  const schemaHi = dv.getUint32(OFF_SCHEMA_ID + 4, true);
  return {
    version, headerSize, slotCount, slotStride,
    dtype: { code, bits, lanes },
    elemSize,
    shape, strides, rank,
    schemaId: schemaHi * 2 ** 32 + schemaLo,
    schemaIdLo: schemaLo, schemaIdHi: schemaHi,
    tickHz: dv.getUint32(OFF_TICK_HZ, true),
    flags,
    payloadCap: slotStride - SLOT_HEADER_SIZE,
    byteLength,
  };
}

/**
 * Read + validate one slot header into a CALLER-PROVIDED `out` object so the
 * hot loop stays allocation-free. Returns false when the slot is not a valid
 * committed frame (torn write, wrong magic, uncommitted).
 */
export function readSlotHeader(dv, slotBase, out) {
  if (!magicMatches(dv, slotBase + SOFF_MAGIC, SLOT_MAGIC)) return false;
  const flags = dv.getUint32(slotBase + SOFF_SLOT_FLAGS, true);
  if ((flags & SLOT_FLAG_COMMITTED) === 0) return false; // torn / in-progress write
  const seqLo = dv.getUint32(slotBase + SOFF_SEQ, true);
  const seqHi = dv.getUint32(slotBase + SOFF_SEQ + 4, true);
  if (seqHi >= 0x200000) return false; // out of managed bound
  out.seq = seqHi * 2 ** 32 + seqLo;
  out.seqLo = seqLo; out.seqHi = seqHi;
  out.payloadLen = dv.getUint32(slotBase + SOFF_PAYLOAD_LEN, true);
  out.timestampLo = dv.getUint32(slotBase + SOFF_TIMESTAMP_NS, true);
  out.timestampHi = dv.getUint32(slotBase + SOFF_TIMESTAMP_NS + 4, true);
  out.durationUs = dv.getUint32(slotBase + SOFF_DURATION_US, true);
  out.flags = flags;
  out.fourcc = dv.getUint32(slotBase + SOFF_FOURCC, true);
  out.rank = dv.getUint8(slotBase + SOFF_RANK);
  out.planes = dv.getUint8(slotBase + SOFF_PLANES);
  return true;
}
