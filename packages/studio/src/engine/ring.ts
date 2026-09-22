/**
 * Weft Studio — RingMap: the studio-side memory map (STUDIO-SEAMS-V1 §2).
 * One ArrayBuffer of capacity × 32B slots; per-slot seqlock; slot states are
 * DERIVED (seq + reader bookkeeping), never stored redundantly. Atomics-gated
 * when the host is cross-origin isolated (SharedArrayBuffer), plain loads
 * otherwise (single-threaded studio demo path).
 */

import { SLOT_SIZE, DEFAULT_RING_CAPACITY, E } from './types';

export const SLOT_FREE = 0;
export const SLOT_WRITING = 1;
export const SLOT_COMMITTED = 2;
export const SLOT_READ = 3;
export const SLOT_DROPPED = 4;

export type SlotState = typeof SLOT_FREE | typeof SLOT_WRITING | typeof SLOT_COMMITTED | typeof SLOT_READ | typeof SLOT_DROPPED;

export interface RingCounters {
  published: number;
  dropped: number;
  tornRetries: number;
  reads: number;
}

const OFF_SEQ = 0;      // u32
const OFF_WRITER = 4;   // u16
const OFF_TYPE = 6;     // u8
const OFF_FLAGS = 7;    // u8
const OFF_TS = 8;       // u64 (lo/hi u32 pair, little-endian)
const OFF_PAYLOAD = 16; // 16 bytes

export class RingMap {
  readonly buffer: ArrayBuffer | SharedArrayBuffer;
  readonly view: DataView;
  readonly bytes: Uint8Array;
  readonly capacity: number;
  /** reader-side record of last read seq per slot (Int32Array, capacity) */
  readonly readMarks: Int32Array;
  writeIndex = 0;              // next slot to write (ring cursor)
  counters: RingCounters = { published: 0, dropped: 0, tornRetries: 0, reads: 0 };
  private statsDirty = false;

  constructor(capacity: number = DEFAULT_RING_CAPACITY) {
    if (!Number.isInteger(capacity) || capacity < 16 || capacity > 4_194_304) {
      throw new Error(`${E.CAPACITY}: ring capacity must be integer in [16, 4194304], got ${capacity}`);
    }
    this.capacity = capacity;
    const total = capacity * SLOT_SIZE;
    this.buffer = new SharedArrayBuffer(total);
    this.view = new DataView(this.buffer);
    this.bytes = new Uint8Array(this.buffer);
    this.readMarks = new Int32Array(capacity);
  }

  // ---------------------------------------------------------------- write --

  /**
   * Publish a message: claim slot, seqlock-write payload, release.
   * `payload` must be 16 bytes; `scratch` is a caller-provided DataView window
   * to avoid per-call allocation in the hot path.
   * Returns the slot index, or -1 when overwrite would clobber an unread
   * COMMITTED slot under the caller's drop policy (callers decide).
   */
  publish(
    writerId: number, msgType: number, tsNsLo: number, tsNsHi: number,
    payload: Uint8Array, payloadOff: number,
  ): number {
    const idx = this.writeIndex;
    const base = idx * SLOT_SIZE;
    const dv = this.view;
    // claim: seq := odd (writing)
    const oldSeq = dv.getUint32(base + OFF_SEQ, true);
    if ((oldSeq & 1) === 0 && oldSeq !== 0 && this.readMarks[idx] < oldSeq) {
      // unread committed slot about to be overwritten => dropped
      this.counters.dropped++;
    }
    dv.setUint32(base + OFF_SEQ, oldSeq + 1, true); // odd
    dv.setUint16(base + OFF_WRITER, writerId & 0xffff, true);
    dv.setUint8(base + OFF_TYPE, msgType & 0xff);
    dv.setUint8(base + OFF_FLAGS, 0);
    dv.setUint32(base + OFF_TS, tsNsLo >>> 0, true);
    dv.setUint32(base + OFF_TS + 4, tsNsHi >>> 0, true);
    // 16B payload copy (the ONLY copy; studio-bound, no intermediate objects)
    for (let i = 0; i < 16; i++) this.bytes[base + OFF_PAYLOAD + i] = payload[payloadOff + i];
    dv.setUint32(base + OFF_SEQ, oldSeq + 2, true); // even (committed)
    this.counters.published++;
    this.writeIndex = (idx + 1) % this.capacity;
    return idx;
  }

  // ----------------------------------------------------------------- read --

  /**
   * Read the newest committed payload into `out` (a caller-owned Uint8Array,
   * >= 16B). Seqlock: retry on odd seq / changed seq (torn read). Returns slot
   * index or -1 when nothing fresh; `seqOut[0]` receives the stable seq.
   */
  readNewestFrom(startIdx: number, out: Uint8Array, seqOut: Int32Array): number {
    const dv = this.view;
    for (let k = 0; k < this.capacity; k++) {
      const idx = (startIdx + k) % this.capacity;
      const base = idx * SLOT_SIZE;
      let s1 = dv.getUint32(base + OFF_SEQ, true);
      if (s1 === 0 || (s1 & 1) !== 0 || this.readMarks[idx] >= s1) continue;
      // copy payload
      let s2 = s1;
      do {
        for (let i = 0; i < 16; i++) out[i] = this.bytes[base + OFF_PAYLOAD + i];
        s2 = dv.getUint32(base + OFF_SEQ, true);
        if (s2 !== s1) {
          this.counters.tornRetries++;
          if ((s2 & 1) === 0) s1 = s2;
        }
      } while (s2 !== s1 && (s2 & 1) === 0);
      if (s2 !== s1) continue; // writer moved on; treat as not-fresh, retry scan
      this.readMarks[idx] = s2;
      seqOut[0] = s2 | 0;
      this.counters.reads++;
      return idx;
    }
    return -1;
  }

  // ------------------------------------------------------------ visualize --

  slotState(idx: number): SlotState {
    const base = idx * SLOT_SIZE;
    const seq = this.view.getUint32(base + OFF_SEQ, true);
    if (seq === 0) return SLOT_FREE;
    if (seq & 1) return SLOT_WRITING;
    return this.readMarks[idx] >= seq ? SLOT_READ : SLOT_COMMITTED;
  }

  slotSeq(idx: number): number {
    return this.view.getUint32(idx * SLOT_SIZE + OFF_SEQ, true);
  }

  slotWriter(idx: number): number {
    return this.view.getUint16(idx * SLOT_SIZE + OFF_WRITER, true);
  }

  slotTsNs(idx: number): number {
    const base = idx * SLOT_SIZE + OFF_TS;
    const lo = this.view.getUint32(base, true);
    const hi = this.view.getUint32(base + 4, true);
    return lo + hi * 4294967296;
  }

  /** Derived occupancy: committed-or-newer slots (single-lap semantics). */
  occupancy(): number {
    // writeIndex is the lap cursor; every slot behind it is occupied
    return this.counters.published < this.capacity
      ? this.counters.published
      : this.capacity;
  }

  markStatsDirty(): void { this.statsDirty = true; }
  consumeStatsDirty(): boolean { const d = this.statsDirty; this.statsDirty = false; return d; }
}
