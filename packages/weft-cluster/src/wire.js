// wire.js — WCN1 (datagram) + WGS1 (gossip) wire format, per
// docs/weft-cluster/WIRE-V1.md (NORMATIVE).
//
// Law 1: every hot-path primitive is handle-reusing (`*Into` forms). Nothing
//        here allocates per frame — no new DataView/TypedArray/object.
// Law 2: every multi-byte access passes an explicit little-endian flag and is
//        byte-exact with python/weft_cluster/wire.py (struct '<' + zlib.crc32).
// Law 3: standard primitives only (DataView / typed arrays / Atomics) — runs
//        on Node.js, Deno, Bun, browsers (main + workers), Electron.
// Law 4: decoders RETURN stable numeric codes (§5) on the hot path; throwing
//        wrappers exist only for cold-path callers.

import { crc32Range } from './crc32.js';
import { WC } from './errors.js';

/** "WCN1" bytes read as a little-endian u32. */
export const WCN1_MAGIC = 0x314e4357;
/** "WGS1" bytes read as a little-endian u32. */
export const WGS1_MAGIC = 0x31534757;
export const WCN1_VERSION = 1;
export const WCN1_HEADER_SIZE = 64;
export const WGS1_VERSION = 1;
export const WGS1_HEADER_SIZE = 48;
export const WGS1_ENTRY_SIZE = 32;

export const WC_FLAG_INLINE_PAYLOAD = 1;
export const WC_FLAG_RDMA_REF = 2;
export const WC_FLAG_CRC_PRESENT = 4;
export const WC_FLAG_CONTROL = 8; // payload[0]: 1 = SUB, 2 = UNSUB

// WCN1 header field offsets (docs/weft-cluster/WIRE-V1.md §1).
export const OFF_VERSION = 4;
export const OFF_HEADER_SIZE = 6;
export const OFF_FLAGS = 8;
export const OFF_SRC_NODE = 12;
export const OFF_TOPIC_HASH = 16;   // u64 LE: lo at +16, hi at +20
export const OFF_SEQ = 24;          // u64 LE: lo at +24, hi at +28
export const OFF_TIMESTAMP = 32;    // u64 LE
export const OFF_PAYLOAD_LEN = 40;
export const OFF_SCHEMA_ID = 44;
export const OFF_CRC = 48;
export const OFF_RDMA_KEY = 52;

// WGS1 header field offsets (§2).
export const GOFF_SENDER = 8;
export const GOFF_ENTRY_COUNT = 12;
export const GOFF_ROUND = 16;
export const GOFF_SENDER_TS = 24;
export const GOFF_BOOT_ID = 32;
export const GOFF_SENDER_FLAGS = 40;

// Gossip entry offsets (§2.1).
export const EOFF_NODE = 0;
export const EOFF_ADDR = 4;
export const EOFF_GOSSIP_PORT = 8;
export const EOFF_ENTRY_FLAGS = 10;
export const EOFF_LAST_SEEN = 12;
export const EOFF_INCARNATION = 20;
export const EOFF_DATA_PORT = 24;

export const GOSSIP_FLAG_ALIVE = 1;
export const GOSSIP_FLAG_LEAVING = 2;

const FNV_OFFSET = 0xcbf29ce484222325n;
const FNV_PRIME = 0x100000001b3n;
const U64_MASK = 0xffffffffffffffffn;

/**
 * FNV-1a 64 of a topic name's UTF-8 bytes. COLD PATH ONLY (topic
 * registration); hot paths carry the cached hi/lo pair inside handles.
 * @param {string} topic
 * @returns {{hi: number, lo: number}} u64 split into unsigned u32 halves
 */
export function fnv1a64(topic) {
  let h = FNV_OFFSET;
  const bytes = /** @type {Uint8Array} */ (new TextEncoder().encode(topic));
  for (let i = 0; i < bytes.length; i++) {
    h ^= BigInt(bytes[i]);
    h = (h * FNV_PRIME) & U64_MASK;
  }
  return { lo: Number(h & 0xffffffffn) >>> 0, hi: Number((h >> 32n) & 0xffffffffn) >>> 0 };
}

/** Reassemble a hi/lo u32 pair into a BigInt (cold path: logs, maps, tests). */
export function u64ToBigInt(lo, hi) { return (BigInt(hi >>> 0) << 32n) | BigInt(lo >>> 0); }

/**
 * Reusable WCN1 frame handle. ONE instance per subscriber/publisher hot path,
 * bound to a buffer via `bind()` and re-bound per frame via `decodeInto`.
 */
export class FrameHandle {
  constructor() {
    this.dv = null;          /** @type {DataView|null} */
    this.u8 = null;          /** @type {Uint8Array|null} */
    this.offset = 0;
    this.version = 0;
    this.headerSize = 0;
    this.flags = 0;
    this.srcNode = 0;
    this.topicLo = 0; this.topicHi = 0;
    this.seqLo = 0; this.seqHi = 0;
    this.tsLo = 0; this.tsHi = 0;
    this.payloadLen = 0;
    this.schemaId = 0;
    this.crc = 0;
    this.rdmaKey = 0;
    this.payloadOffset = 0;  // headerSize + offset (where payload begins)
  }
  /** Bind handle to a buffer view (allocate-once). */
  bind(buffer, byteOffset) {
    this.dv = new DataView(buffer, byteOffset);
    this.u8 = new Uint8Array(buffer, byteOffset);
    return this;
  }
  /** Cold-path BigInt view of seq. */
  seqBigInt() { return u64ToBigInt(this.seqLo, this.seqHi); }
  /** Cold-path BigInt view of topic hash. */
  topicBigInt() { return u64ToBigInt(this.topicLo, this.topicHi); }
  /** True if the frame carries an inline payload. */
  get inline() { return (this.flags & WC_FLAG_INLINE_PAYLOAD) !== 0; }
}

/**
 * Encode a WCN1 header into `dst` at `off`. Caller writes the payload at
 * `off + WCN1_HEADER_SIZE` itself and passes its CRC (0 when absent).
 * Zero allocation. Returns `off + WCN1_HEADER_SIZE`.
 * @param {DataView} dst little-endian-capable view of the destination
 * @param {number} off
 * @param {{flags:number, srcNode:number, topicLo:number, topicHi:number,
 *          seqLo:number, seqHi:number, tsLo:number, tsHi:number,
 *          payloadLen:number, schemaId:number, crc:number, rdmaKey:number}} f
 */
export function encodeWcn1Into(dst, off, f) {
  dst.setUint8(off + 0, 0x57); dst.setUint8(off + 1, 0x43);
  dst.setUint8(off + 2, 0x4e); dst.setUint8(off + 3, 0x31);       // "WCN1"
  dst.setUint16(off + OFF_VERSION, WCN1_VERSION, true);
  dst.setUint16(off + OFF_HEADER_SIZE, WCN1_HEADER_SIZE, true);
  dst.setUint32(off + OFF_FLAGS, f.flags, true);
  dst.setUint32(off + OFF_SRC_NODE, f.srcNode >>> 0, true);
  dst.setUint32(off + OFF_TOPIC_HASH, f.topicLo >>> 0, true);
  dst.setUint32(off + OFF_TOPIC_HASH + 4, f.topicHi >>> 0, true);
  dst.setUint32(off + OFF_SEQ, f.seqLo >>> 0, true);
  dst.setUint32(off + OFF_SEQ + 4, f.seqHi >>> 0, true);
  dst.setUint32(off + OFF_TIMESTAMP, f.tsLo >>> 0, true);
  dst.setUint32(off + OFF_TIMESTAMP + 4, f.tsHi >>> 0, true);
  dst.setUint32(off + OFF_PAYLOAD_LEN, f.payloadLen >>> 0, true);
  dst.setUint32(off + OFF_SCHEMA_ID, f.schemaId >>> 0, true);
  dst.setUint32(off + OFF_CRC, f.crc >>> 0, true);
  dst.setUint32(off + OFF_RDMA_KEY, f.rdmaKey >>> 0, true);
  dst.setUint32(off + 56, 0, true); dst.setUint32(off + 60, 0, true);
  return off + WCN1_HEADER_SIZE;
}

/**
 * Decode + validate a WCN1 header from `src` at `off` into `h`.
 * RETURNS a stable WC_* code (0 = WC_OK) — Law 4 hot path, never throws.
 * @param {FrameHandle} h handle previously bound to the source buffer
 * @param {number} off byte offset of the header within the bound buffer
 */
export function decodeWcn1Into(h, off) {
  const dv = h.dv; if (dv === null) return WC.WC_E_BAD_HEADER;
  const avail = dv.byteLength - off;
  if (avail < 4) return WC.WC_E_TRUNCATED;
  if (dv.getUint32(off, true) !== WCN1_MAGIC) return WC.WC_E_BAD_MAGIC;
  if (avail < 8) return WC.WC_E_TRUNCATED;
  const version = dv.getUint16(off + OFF_VERSION, true);
  if (version > WCN1_VERSION) return WC.WC_E_BAD_VERSION;
  const headerSize = dv.getUint16(off + OFF_HEADER_SIZE, true);
  if (headerSize < WCN1_HEADER_SIZE || headerSize > avail) return WC.WC_E_BAD_HEADER;
  const flags = dv.getUint32(off + OFF_FLAGS, true);
  const payloadLen = dv.getUint32(off + OFF_PAYLOAD_LEN, true);
  const inline = (flags & WC_FLAG_INLINE_PAYLOAD) !== 0;
  if (inline && payloadLen > avail - headerSize) return WC.WC_E_TRUNCATED;
  h.offset = off;
  h.version = version;
  h.headerSize = headerSize;
  h.flags = flags;
  h.srcNode = dv.getUint32(off + OFF_SRC_NODE, true);
  h.topicLo = dv.getUint32(off + OFF_TOPIC_HASH, true);
  h.topicHi = dv.getUint32(off + OFF_TOPIC_HASH + 4, true);
  h.seqLo = dv.getUint32(off + OFF_SEQ, true);
  h.seqHi = dv.getUint32(off + OFF_SEQ + 4, true);
  h.tsLo = dv.getUint32(off + OFF_TIMESTAMP, true);
  h.tsHi = dv.getUint32(off + OFF_TIMESTAMP + 4, true);
  h.payloadLen = payloadLen;
  h.schemaId = dv.getUint32(off + OFF_SCHEMA_ID, true);
  h.crc = dv.getUint32(off + OFF_CRC, true);
  h.rdmaKey = dv.getUint32(off + OFF_RDMA_KEY, true);
  h.payloadOffset = off + headerSize;
  return WC.WC_OK;
}

/**
 * CRC + inline-payload consistency check. Hot path, zero allocation.
 * @param {FrameHandle} h a successfully decoded handle
 * @returns {number} WC_OK or WC_E_BAD_CRC
 */
export function verifyFrameCrc(h) {
  if ((h.flags & WC_FLAG_CRC_PRESENT) === 0) return WC.WC_OK;
  const got = crc32Range(/** @type {Uint8Array} */(h.u8), h.payloadOffset, h.payloadOffset + h.payloadLen);
  return got === (h.crc >>> 0) ? WC.WC_OK : WC.WC_E_BAD_CRC;
}

/**
 * Cold-path snapshot of a WCN1 header (allocates; for tests, CLI, logging).
 * @param {DataView} dv @param {number} off
 */
export function parseWcn1(dv, off) {
  const h = new FrameHandle().bind(dv.buffer, dv.byteOffset);
  const code = decodeWcn1Into(h, off);
  if (code !== WC.WC_OK) { const e = new Error(wcMsg(code)); e.code = code; throw e; }
  return h;
}

// ---------------------------------------------------------------------------
// WGS1 gossip

/**
 * Encode a WGS1 gossip header into `dst` at `off`. Entries are appended by
 * the caller via `writeGossipEntryInto`. Zero allocation.
 */
export function encodeWgs1Into(dst, off, f) {
  dst.setUint8(off + 0, 0x57); dst.setUint8(off + 1, 0x47);
  dst.setUint8(off + 2, 0x53); dst.setUint8(off + 3, 0x31);       // "WGS1"
  dst.setUint16(off + 4, WGS1_VERSION, true);
  dst.setUint16(off + 6, WGS1_HEADER_SIZE, true);
  dst.setUint32(off + GOFF_SENDER, f.senderNode >>> 0, true);
  dst.setUint32(off + GOFF_ENTRY_COUNT, f.entryCount >>> 0, true);
  dst.setUint32(off + GOFF_ROUND, f.roundLo >>> 0, true);
  dst.setUint32(off + GOFF_ROUND + 4, f.roundHi >>> 0, true);
  dst.setUint32(off + GOFF_SENDER_TS, f.tsLo >>> 0, true);
  dst.setUint32(off + GOFF_SENDER_TS + 4, f.tsHi >>> 0, true);
  dst.setUint32(off + GOFF_BOOT_ID, f.bootLo >>> 0, true);
  dst.setUint32(off + GOFF_BOOT_ID + 4, f.bootHi >>> 0, true);
  dst.setUint32(off + GOFF_SENDER_FLAGS, f.senderFlags >>> 0, true);
  dst.setUint32(off + 44, 0, true);
  return off + WGS1_HEADER_SIZE;
}

/** Write one 32-byte gossip entry. Returns offset after the entry. */
export function writeGossipEntryInto(dst, off, e) {
  dst.setUint32(off + EOFF_NODE, e.nodeId >>> 0, true);
  dst.setUint32(off + EOFF_ADDR, e.addr >>> 0, true);
  dst.setUint16(off + EOFF_GOSSIP_PORT, e.gossipPort & 0xffff, true);
  dst.setUint16(off + EOFF_ENTRY_FLAGS, e.entryFlags & 0xffff, true);
  dst.setUint32(off + EOFF_LAST_SEEN, e.lastSeenLo >>> 0, true);
  dst.setUint32(off + EOFF_LAST_SEEN + 4, e.lastSeenHi >>> 0, true);
  dst.setUint32(off + EOFF_INCARNATION, e.incarnation >>> 0, true);
  dst.setUint16(off + EOFF_DATA_PORT, e.dataPort & 0xffff, true);
  dst.setUint16(off + 26, 0, true);
  dst.setUint32(off + 28, 0, true);
  return off + WGS1_ENTRY_SIZE;
}

/**
 * Decode + validate a WGS1 header. RETURNS a stable WC_* code (Law 4).
 * @param {DataView} dv @param {number} off @param {{senderNode:number,
 * entryCount:number, roundLo:number, roundHi:number, tsLo:number, tsHi:number,
 * bootLo:number, bootHi:number, senderFlags:number}} h preallocated handle
 */
export function decodeWgs1Into(dv, off, h) {
  const avail = dv.byteLength - off;
  if (avail < 4) return WC.WC_E_TRUNCATED;
  if (dv.getUint32(off, true) !== WGS1_MAGIC) return WC.WC_E_BAD_MAGIC;
  if (avail < 8) return WC.WC_E_TRUNCATED;
  if (dv.getUint16(off + 4, true) > WGS1_VERSION) return WC.WC_E_BAD_VERSION;
  const headerSize = dv.getUint16(off + 6, true);
  if (headerSize < WGS1_HEADER_SIZE || headerSize > avail) return WC.WC_E_BAD_HEADER;
  const entryCount = dv.getUint32(off + GOFF_ENTRY_COUNT, true);
  if (WGS1_HEADER_SIZE + entryCount * WGS1_ENTRY_SIZE > avail) return WC.WC_E_TRUNCATED;
  h.senderNode = dv.getUint32(off + GOFF_SENDER, true);
  h.entryCount = entryCount;
  h.roundLo = dv.getUint32(off + GOFF_ROUND, true);
  h.roundHi = dv.getUint32(off + GOFF_ROUND + 4, true);
  h.tsLo = dv.getUint32(off + GOFF_SENDER_TS, true);
  h.tsHi = dv.getUint32(off + GOFF_SENDER_TS + 4, true);
  h.bootLo = dv.getUint32(off + GOFF_BOOT_ID, true);
  h.bootHi = dv.getUint32(off + GOFF_BOOT_ID + 4, true);
  h.senderFlags = dv.getUint32(off + GOFF_SENDER_FLAGS, true);
  return WC.WC_OK;
}

/** Read gossip entry `i` into the reusable handle `e`. Zero allocation. */
export function readGossipEntryInto(dv, off, i, e) {
  const base = off + WGS1_HEADER_SIZE + i * WGS1_ENTRY_SIZE;
  e.nodeId = dv.getUint32(base + EOFF_NODE, true);
  e.addr = dv.getUint32(base + EOFF_ADDR, true);
  e.gossipPort = dv.getUint16(base + EOFF_GOSSIP_PORT, true);
  e.entryFlags = dv.getUint16(base + EOFF_ENTRY_FLAGS, true);
  e.lastSeenLo = dv.getUint32(base + EOFF_LAST_SEEN, true);
  e.lastSeenHi = dv.getUint32(base + EOFF_LAST_SEEN + 4, true);
  e.incarnation = dv.getUint32(base + EOFF_INCARNATION, true);
  e.dataPort = dv.getUint16(base + EOFF_DATA_PORT, true);
  return base + WGS1_ENTRY_SIZE;
}

function wcMsg(code) {
  // Local import-free copy of names to keep wire.js dependency-light in
  // environments where errors.js constants are tree-shaken apart.
  switch (code) {
    case 1: return 'WC_E_BAD_MAGIC'; case 2: return 'WC_E_BAD_VERSION';
    case 3: return 'WC_E_TRUNCATED'; case 4: return 'WC_E_BAD_HEADER';
    case 5: return 'WC_E_BAD_CRC'; case 6: return 'WC_E_UNKNOWN_NODE';
    default: return `WC_ERROR_${code}`;
  }
}

export { crc32Range };
