// ring.js — RNG1 robotics record ring reader (managed TS side).
//
// Mirrors docs/adapters/MANAGED-SEAMS-V1.md §5 and python/weft_robotics
// byte-for-byte (Law 4). Reader protocol — drop-not-block:
//   slot seq must equal the expected continuous window value; an
//   in-flight write publishes a transient that fails that check; the
//   value is re-read after payload bind (publish check). A reader NEVER
//   blocks, allocates or throws on torn/skipped records — counters only.
//
// Zero-alloc hot path: ONE DataView + ONE flyweight RecordView reused
// across the whole feed (Law 2).

export const RNG1_HEADER_SIZE = 128;
export const SLOT_HDR = 64;

export const FMT_IMU6DOF = 1;
export const FMT_POINTS_F32 = 2;
export const FMT_FRAME_DESC = 3;
export const FMT_BOXES_F32 = 4;

export const FMT_NAMES = new Map([
  [FMT_IMU6DOF, 'imu6dof'],
  [FMT_POINTS_F32, 'points_f32'],
  [FMT_FRAME_DESC, 'frame_desc'],
  [FMT_BOXES_F32, 'boxes_f32'],
]);

// header offsets
const O_SLOT_SIZE = 8;
const O_SLOT_COUNT = 12;
const O_COMMITTED = 24;
const O_DROP_COUNT = 32;
const O_TOPICS = 64;

// slot header offsets
const S_SEQ = 0;
const S_LEN = 8;
const S_TOPIC = 12;
const S_TS = 16;
const S_FMT = 24;

// typed attach failure codes (Law 4 decision order)
export const E_SHORT = 1;
export const E_MAGIC = 2;
export const E_VERSION = 3;
export const E_HEADER = 4;
export const E_GEOMETRY = 5;

export class Rng1Error extends Error {
  constructor(code) {
    super(`RNG1 attach failed: E_${code}`);
    this.code = code;
  }
}

function validateHeader(dv, byteLength) {
  if (byteLength < RNG1_HEADER_SIZE) return E_SHORT;
  if (dv.getUint32(0, true) !== 0x31474e52) return E_MAGIC; // "RNG1" LE
  if (dv.getUint16(4, true) !== 1) return E_VERSION;
  if (dv.getUint16(6, true) !== RNG1_HEADER_SIZE) return E_HEADER;
  const slotSize = dv.getUint32(O_SLOT_SIZE, true);
  const slotCount = dv.getUint32(O_SLOT_COUNT, true);
  if (slotSize < 256 || (slotSize & (slotSize - 1)) !== 0) return E_GEOMETRY;
  if (slotCount === 0 || (slotCount & (slotCount - 1)) !== 0) return E_GEOMETRY;
  if (byteLength < RNG1_HEADER_SIZE + slotSize * slotCount) return E_SHORT;
  return 0;
}

const U64_DIV = 4294967296;

/** u64 read as exact JS number (fits: seqs/ts stay < 2^53). */
function readU64(dv, off) {
  return dv.getUint32(off + 4, true) * U64_DIV + dv.getUint32(off, true);
}

/** Flyweight over one RNG1 slot. Bound by the reader; payload views are
 *  windows over the RING bytes — valid until the next acquire. */
export class RecordView {
  constructor() {
    this.dv = null;
    this.u8 = null;
    this.base = 0;
    this.seq = 0;
    this.topicId = 0;
    this.tsNs = 0;
    this.fmt = 0;
    this.payloadOff = 0;
    this.payloadLen = 0;
  }

  fmtName() {
    return FMT_NAMES.get(this.fmt) ?? `fmt${this.fmt}`;
  }

  /** POINTS_F32 -> Float32Array window (0-copy, N*3 xyz triples). */
  pointsView() {
    const n = (this.payloadLen / 12) | 0;
    return new Float32Array(this.u8.buffer, this.u8.byteOffset + this.payloadOff, n * 3);
  }

  /** BOXES_F32 -> Float32Array window (0-copy, M*6 x,y,z,w,h,score). */
  boxesView() {
    const n = (this.payloadLen / 24) | 0;
    return new Float32Array(this.u8.buffer, this.u8.byteOffset + this.payloadOff, n * 6);
  }

  /** IMU6DOF -> preallocated 8-slot out array [ts, qw..qz, gx..gz]. */
  imuInto(out) {
    for (let i = 0; i < 8; i++) out[i] = this.dv.getFloat64(this.payloadOff + i * 8, true);
    return out;
  }

  /** FRAME_DESC -> reads FRM1 fields into `out` (fail-closed: 0 width). */
  frm1Into(out) {
    if (this.payloadLen < 32 || this.u8[this.payloadOff + 1] !== 0x52) {
      out.valid = false;
      return out;
    }
    out.valid = true;
    out.width = this.dv.getUint32(this.payloadOff + 4, true);
    out.height = this.dv.getUint32(this.payloadOff + 8, true);
    out.stride = this.dv.getUint32(this.payloadOff + 12, true);
    out.format = this.dv.getUint32(this.payloadOff + 16, true);
    out.handleNs = this.dv.getUint32(this.payloadOff + 20, true);
    out.handleLo = this.dv.getUint32(this.payloadOff + 24, true);
    out.flags = this.dv.getUint32(this.payloadOff + 28, true);
    return out;
  }
}

export function attachRing(buffer, opts) {
  const byteLength = buffer.byteLength;
  const dv = new DataView(buffer, (opts && opts.byteOffset) || 0, byteLength);
  const code = validateHeader(dv, byteLength);
  if (code !== 0) throw new Rng1Error(code);
  return new RingReader(buffer, dv);
}

export class RingReader {
  constructor(buffer, dv) {
    this.dv = dv;
    this.u8 = new Uint8Array(buffer, dv.byteOffset, byteLengthSafe(buffer));
    this.slotSize = dv.getUint32(O_SLOT_SIZE, true);
    this.slotCount = dv.getUint32(O_SLOT_COUNT, true);
    this.view = new RecordView();
    // unboxed counters on preallocated storage (Law 2)
    this.stats = new Int32Array(4); // acquires, torn, skipped, filtered
    this._nextSeq = 1;
    // reusable scratch (imu/frm1 outs)
    this.imuOut = new Float64Array(8);
    this.frm1Out = { valid: false, width: 0, height: 0, stride: 0,
                     format: 0, handleNs: 0, handleLo: 0, flags: 0 };
  }

  committedSeq() { return readU64(this.dv, O_COMMITTED); }
  headerDropCount() { return readU64(this.dv, O_DROP_COUNT); }

  topicTable() {
    const out = [];
    for (let i = 0; i < 8; i++) {
      const v = this.dv.getUint32(O_TOPICS + i * 4, true);
      if (v !== 0) out.push(v);
    }
    return out;
  }

  /** Newest continuous record, or null. topicId filters (counts filtered).
   *  The returned RecordView is REUSED — consume before the next acquire. */
  acquire(topicId) {
    this.stats[0]++;
    const committed = readU64(this.dv, O_COMMITTED);
    if (committed === 0 || this._nextSeq > committed) return null;
    const slot = (committed - 1) % this.slotCount;
    const base = RNG1_HEADER_SIZE + slot * this.slotSize;
    const seq1 = readU64(this.dv, base + S_SEQ);
    if (seq1 !== committed) {
      this.stats[1]++;         // torn: window mismatch (in-flight or crashed)
      this._nextSeq = committed + 1;
      return null;
    }
    const view = this.view;
    view.dv = this.dv;
    view.u8 = this.u8;
    view.base = base;
    view.seq = seq1;
    view.topicId = this.dv.getUint32(base + S_TOPIC, true);
    view.tsNs = readU64(this.dv, base + S_TS);
    view.fmt = this.dv.getUint32(base + S_FMT, true);
    view.payloadOff = base + SLOT_HDR;
    view.payloadLen = this.dv.getUint32(base + S_LEN, true);
    if (topicId !== undefined && topicId !== null && view.topicId !== topicId) {
      this.stats[3]++;
      this._nextSeq = committed + 1;
      return null;
    }
    const seq2 = readU64(this.dv, base + S_SEQ);
    if (seq2 !== seq1) {
      this.stats[1]++;
      this._nextSeq = committed + 1;
      return null;
    }
    this._nextSeq = committed + 1;
    return view;
  }

  /** Walk every continuous committed record (audit/parity lane). */
  drain(visit) {
    const committed = Number(readU64(this.dv, O_COMMITTED));
    for (let s = 1; s <= committed; s++) {
      const slot = (s - 1) % this.slotCount;
      const base = RNG1_HEADER_SIZE + slot * this.slotSize;
      const seq1 = readU64(this.dv, base + S_SEQ);
      if (seq1 !== s) { this.stats[1]++; continue; }
      const view = this.view;
      view.dv = this.dv;
      view.u8 = this.u8;
      view.base = base;
      view.seq = seq1;
      view.topicId = this.dv.getUint32(base + S_TOPIC, true);
      view.tsNs = readU64(this.dv, base + S_TS);
      view.fmt = this.dv.getUint32(base + S_FMT, true);
      view.payloadOff = base + SLOT_HDR;
      view.payloadLen = this.dv.getUint32(base + S_LEN, true);
      if (readU64(this.dv, base + S_SEQ) !== seq1) { this.stats[1]++; continue; }
      this.stats[0]++;
      if (visit(view) === false) return;
    }
  }
}

function byteLengthSafe(buffer) {
  return buffer.byteLength;
}
