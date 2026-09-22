/**
 * Weft Studio — SREC1 flight recorder + deterministic time-travel replay.
 * (STUDIO-SEAMS-V1 §3.) Append-only 40B records; XOR-delta replay fold over a
 * fixed 8-lane u64 shadow state; checkpoints every 4096 records; scrubbing =
 * seed from nearest checkpoint + fold forward. Byte-identical across passes.
 */

import {
  SREC_MAGIC, SREC_VERSION, SREC_RECORD_SIZE, SREC_HEADER_SIZE,
  CHECKPOINT_INTERVAL, crc32, hex8,
} from './types';

export const SREC_TAG0 = 0x53; // 'S'
export const SBURST_TRAILER_SIZE = 32;

const R_OFF_TS = 0;
const R_OFF_ADDR = 8;
const R_OFF_SEQ_OLD = 16;
const R_OFF_SEQ_NEW = 20;
const R_OFF_BEFORE = 24;
const R_OFF_AFTER = 32;

const GROW_RECORDS = 8192;

/** u64 before/after folds are kept as two u32 lanes (hi, lo) — no BigInt. */
export class FlightRecorder {
  private buf = new Uint8Array(GROW_RECORDS * SREC_RECORD_SIZE);
  private dv = new DataView(this.buf.buffer);
  private count = 0;
  readonly openedNs: number;
  readonly schemaHash: string;

  constructor(schemaHash: string, openedNs = 0) {
    this.schemaHash = schemaHash;
    this.openedNs = openedNs;
  }

  get recordCount(): number { return this.count; }

  /** Record one mutation event. All args scalar — zero allocation. */
  record(tsNs: number, addr: number, seqOld: number, seqNew: number, beforeLo: number, beforeHi: number, afterLo: number, afterHi: number): void {
    if ((this.count + 1) * SREC_RECORD_SIZE > this.buf.length) this.grow();
    const off = this.count * SREC_RECORD_SIZE;
    const dv = this.dv;
    dv.setFloat64(off + R_OFF_TS, tsNs, true);      // ns fits f64 exactly to ~2^53 (≈104 days)
    dv.setUint32(off + R_OFF_ADDR, addr >>> 0, true);
    dv.setUint32(off + R_OFF_SEQ_OLD, seqOld >>> 0, true);
    dv.setUint32(off + R_OFF_SEQ_NEW, seqNew >>> 0, true);
    dv.setUint32(off + R_OFF_BEFORE, beforeLo >>> 0, true);
    dv.setUint32(off + R_OFF_BEFORE + 4, beforeHi >>> 0, true);
    dv.setUint32(off + R_OFF_AFTER, afterLo >>> 0, true);
    dv.setUint32(off + R_OFF_AFTER + 4, afterHi >>> 0, true);
    this.count++;
  }

  private grow(): void {
    const nb = new Uint8Array(this.buf.length + GROW_RECORDS * SREC_RECORD_SIZE);
    nb.set(this.buf);
    this.buf = nb;
    this.dv = new DataView(nb.buffer);
  }

  tsAt(i: number): number { return this.dv.getFloat64(i * SREC_RECORD_SIZE + R_OFF_TS, true); }
  addrAt(i: number): number { return this.dv.getUint32(i * SREC_RECORD_SIZE + R_OFF_ADDR, true); }
  seqOldAt(i: number): number { return this.dv.getUint32(i * SREC_RECORD_SIZE + R_OFF_SEQ_OLD, true); }
  seqNewAt(i: number): number { return this.dv.getUint32(i * SREC_RECORD_SIZE + R_OFF_SEQ_NEW, true); }
  beforeLoAt(i: number): number { return this.dv.getUint32(i * SREC_RECORD_SIZE + R_OFF_BEFORE, true); }
  beforeHiAt(i: number): number { return this.dv.getUint32(i * SREC_RECORD_SIZE + R_OFF_BEFORE + 4, true); }
  afterLoAt(i: number): number { return this.dv.getUint32(i * SREC_RECORD_SIZE + R_OFF_AFTER, true); }
  afterHiAt(i: number): number { return this.dv.getUint32(i * SREC_RECORD_SIZE + R_OFF_AFTER + 4, true); }

  // ------------------------------------------------------------- serialize --

  /** SREC1 stream bytes (header + records). */
  toBytes(out: Uint8Array): number {
    const total = SREC_HEADER_SIZE + this.count * SREC_RECORD_SIZE;
    if (out.length < total) return -1;
    const dv = new DataView(out.buffer, out.byteOffset, out.byteLength);
    dv.setUint32(0, SREC_MAGIC, true);
    dv.setUint16(4, SREC_VERSION, true);
    dv.setUint16(6, SREC_RECORD_SIZE, true);
    dv.setFloat64(8, this.openedNs, true);
    // schema hash: fold two 32-bit halves from the hex string
    const h = this.schemaHash;
    const hi = parseInt(h.slice(0, 8), 16) >>> 0;
    const lo = parseInt(h.slice(8, 16), 16) >>> 0;
    dv.setUint32(16, hi, true);
    dv.setUint32(20, lo, true);
    dv.setUint32(24, this.count, true);
    dv.setUint32(28, crc32(out, 0, 28), true);
    out.set(this.buf.subarray(0, this.count * SREC_RECORD_SIZE), SREC_HEADER_SIZE);
    return total;
  }

  static serializedSize(recordCount: number): number {
    return SREC_HEADER_SIZE + recordCount * SREC_RECORD_SIZE;
  }

  static fromBytes(bytes: Uint8Array): { rec: FlightRecorder; error: string | null } {
    if (bytes.length < SREC_HEADER_SIZE) return { rec: null!, error: 'E_EXPORT: truncated header' };
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (dv.getUint32(0, true) !== SREC_MAGIC) return { rec: null!, error: 'E_EXPORT: bad magic' };
    if (dv.getUint16(4, true) !== SREC_VERSION) return { rec: null!, error: 'E_EXPORT: bad version' };
    const count = dv.getUint32(24, true);
    if (SREC_HEADER_SIZE + count * SREC_RECORD_SIZE > bytes.length) return { rec: null!, error: 'E_EXPORT: truncated records' };
    const crc = dv.getUint32(28, true);
    if (crc !== crc32(bytes, 0, 28)) return { rec: null!, error: 'E_EXPORT: header crc mismatch' };
    const hi = dv.getUint32(16, true);
    const lo = dv.getUint32(20, true);
    const rec = new FlightRecorder(hex8(hi) + hex8(lo), dv.getFloat64(8, true));
    // copy records into the recorder store
    while (rec.buf.length < Math.max(GROW_RECORDS, count) * SREC_RECORD_SIZE) rec.grow();
    rec.buf.set(bytes.subarray(SREC_HEADER_SIZE, SREC_HEADER_SIZE + count * SREC_RECORD_SIZE));
    rec.count = count;
    return { rec, error: null };
  }

  /** One-click crash bundle: SREC1 stream + 32B SBURST trailer. */
  toCrashBundle(out: Uint8Array): number {
    const streamLen = FlightRecorder.serializedSize(this.count);
    const total = streamLen + SBURST_TRAILER_SIZE;
    if (out.length < total) return -1;
    const n = this.toBytes(out);
    if (n !== streamLen) return -1;
    const dv = new DataView(out.buffer, out.byteOffset, out.byteLength);
    dv.setUint32(streamLen, 0x54525542, true);      // "BURT" → read as "TRUB"... spec tag 'SBURST' chunk id 0
    dv.setUint32(streamLen + 4, 0x53525542, true);  // "SBUR" — bundle magic pair
    dv.setFloat64(streamLen + 8, this.openedNs, true);
    const h = this.schemaHash;
    dv.setUint32(streamLen + 16, parseInt(h.slice(0, 8), 16) >>> 0, true);
    dv.setUint32(streamLen + 20, parseInt(h.slice(8, 16), 16) >>> 0, true);
    dv.setUint32(streamLen + 24, this.count, true);
    dv.setUint32(streamLen + 28, crc32(out, 0, streamLen + 28), true);
    return total;
  }
}

// ------------------------------------------------------------------ replay --

export const REPLAY_LANES = 8;

/**
 * Deterministic time-travel replayer. State = 8-lane u32-pair shadow vector.
 * fold(record): S[addr % 8] ^= before; S[addr % 8] ^= after  (per 32-bit lane)
 * Checkpoints every CHECKPOINT_INTERVAL records (full state snapshot).
 */
export class TimeTravelReplayer {
  private readonly checkpoints: Uint32Array;
  private readonly state = new Uint32Array(REPLAY_LANES * 2);
  private readonly scratch = new Uint32Array(REPLAY_LANES * 2);
  private checkpointCount = 0;

  constructor(private readonly rec: FlightRecorder) {
    const n = Math.floor(rec.recordCount / CHECKPOINT_INTERVAL) + 1;
    this.checkpoints = new Uint32Array(n * REPLAY_LANES * 2);
    // checkpoint 0 = zero state
    this.checkpointCount = 1;
    for (let c = 1; c < n; c++) {
      this.foldRange((c - 1) * CHECKPOINT_INTERVAL, c * CHECKPOINT_INTERVAL);
      this.checkpoints.set(this.state, c * REPLAY_LANES * 2);
      this.checkpointCount = c + 1;
    }
  }

  private foldRange(from: number, to: number): void {
    const rec = this.rec;
    const toC = Math.min(to, rec.recordCount);
    for (let i = from; i < toC; i++) {
      const lane = (rec.addrAt(i) % REPLAY_LANES) * 2;
      this.state[lane] ^= rec.beforeLoAt(i);
      this.state[lane + 1] ^= rec.beforeHiAt(i);
      this.state[lane] ^= rec.afterLoAt(i);
      this.state[lane + 1] ^= rec.afterHiAt(i);
    }
  }

  /** Replay to record index i (state AFTER applying records [0, i)). */
  scrubTo(i: number, out: Uint32Array): void {
    const cp = Math.min(Math.floor(i / CHECKPOINT_INTERVAL), this.checkpointCount - 1);
    out.set(this.checkpoints.subarray(cp * REPLAY_LANES * 2, (cp + 1) * REPLAY_LANES * 2));
    const tmp = this.scratch;
    tmp.set(out);
    const rec = this.rec;
    const end = Math.min(i, rec.recordCount);
    for (let k = cp * CHECKPOINT_INTERVAL; k < end; k++) {
      const lane = (rec.addrAt(k) % REPLAY_LANES) * 2;
      tmp[lane] ^= rec.beforeLoAt(k);
      tmp[lane + 1] ^= rec.beforeHiAt(k);
      tmp[lane] ^= rec.afterLoAt(k);
      tmp[lane + 1] ^= rec.afterHiAt(k);
    }
    out.set(tmp);
  }

  /** Deterministic state hash after scrub (hex string). */
  static stateHash(state: Uint32Array): string {
    let h = 0x811c9dc5 | 0;
    for (let i = 0; i < state.length; i++) {
      const v = state[i] | 0;
      h = Math.imul(h ^ (v & 0xffff), 16777619) | 0;
      h = Math.imul(h ^ (v >>> 16), 16777619) | 0;
    }
    return (h >>> 0).toString(16).padStart(8, '0');
  }
}

/** Diff two replay states (mutation viewer rows, caller-owned out). */
export function diffStates(a: Uint32Array, b: Uint32Array, out: Int32Array): number {
  let n = 0;
  for (let lane = 0; lane < REPLAY_LANES; lane++) {
    if (a[lane * 2] !== b[lane * 2] || a[lane * 2 + 1] !== b[lane * 2 + 1]) {
      if (n * 3 + 2 < out.length) {
        out[n * 3] = lane;
        out[n * 3 + 1] = lane * 2;
        out[n * 3 + 2] = lane * 2 + 1;
      }
      n++;
    }
  }
  return n;
}
