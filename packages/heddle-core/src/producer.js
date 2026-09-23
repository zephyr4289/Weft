// src/producer.js — HotPlaneProducer: the single-writer side of HPL1.
//
// Publish protocol (HPL1 §4.1): per-lane seqlock odd → write → even. Statistics
// are maintained IN PLACE on the plane (min/max/avg/current live in the lane
// control block) so UI readers never allocate accumulators (Law 1).
//
// Ordering (Law 2 / HPL1 §4.2): all u64 fields are written hi-first, lo-LAST;
// the `lo` word doubles as the reader freshness gate. All scalar stores are
// explicit little-endian DataView writes. No BigInt anywhere (u64 = lo/hi u32,
// shadows kept as JS numbers — safe below 2^53).
//
// Error paths (Law 4): NaN/±Infinity samples throw HPL1_INVALID_SAMPLE — a
// caller bug, not a runtime condition; it is NEVER silently coerced.

import { HPL1, Hpl1Error } from './errors.js';
import {
  HDR, LANE, LANE_FLAG_ACTIVE, initHeader, deriveGeometry, validatePlane,
} from './layout.js';

export class HotPlaneProducer {
  constructor(buffer, opts = {}) {
    const byteOffset = opts.byteOffset || 0;
    const byteLength = opts.byteLength >= 0 ? opts.byteLength : buffer.byteLength - byteOffset;
    this.geo = validatePlane(buffer, byteOffset, byteLength);
    this.buffer = buffer;
    this.shared = typeof SharedArrayBuffer !== 'undefined' && buffer instanceof SharedArrayBuffer;
    this.dv = new DataView(buffer, byteOffset, byteLength);
    this.u32 = new Uint32Array(buffer, byteOffset, byteLength >> 2);
    const n = this.geo.laneCount;
    // producer-private shadows (preallocated at attach/create — never hot-path)
    this.seq = new Float64Array(n);        // per-lane sequence (even at rest)
    this.samplesSeen = new Float64Array(n);
    this.gSeq = 0;                         // header publishSeq shadow
    this.gEpoch = 0;
    this.gSum = 0; this.gCount = 0;        // global running-avg accumulators
    this.gMin = Infinity; this.gMax = -Infinity;
    this.lastValue = 0;
    // adopt existing session counters (cold path)
    for (let i = 0; i < n; i++) {
      const ctrl = this.geo.laneCtrlBase + i * 64;
      const lo = this.dv.getUint32(ctrl + LANE.SAMPLES_SEEN, true);
      const hi = this.dv.getUint32(ctrl + LANE.SAMPLES_SEEN + 4, true);
      this.samplesSeen[i] = lo + hi * 0x100000000;
      this.seq[i] = this.dv.getUint32(ctrl + LANE.SEQ, true);
    }
    this.gSeq = this.dv.getUint32(HDR.PUBLISH_SEQ, true) +
      this.dv.getUint32(HDR.PUBLISH_SEQ + 4, true) * 0x100000000;
    this.gEpoch = this.dv.getUint32(HDR.EPOCH, true) +
      this.dv.getUint32(HDR.EPOCH + 4, true) * 0x100000000;
    if ((this.gSeq & 1) !== 0) {
      throw new Hpl1Error(HPL1.TORN_SEQLOCK, 'attach under odd header seqlock');
    }
  }

  // Fresh plane in a new SharedArrayBuffer (cold path).
  static create({ laneCount, samplesPerLane, tickHz = 0, epoch = 1 }) {
    const geo = deriveGeometry(laneCount, samplesPerLane);
    const sab = typeof SharedArrayBuffer !== 'undefined'
      ? new SharedArrayBuffer(geo.totalBytes)
      : new ArrayBuffer(geo.totalBytes);
    const dv = new DataView(sab);
    initHeader(dv, laneCount, samplesPerLane, tickHz, epoch);
    const p = new HotPlaneProducer(sab);
    p.gEpoch = epoch;
    return p;
  }

  _storeU64(byteOff, value) {
    const lo = value >>> 0;
    const hi = Math.floor(value / 0x100000000) >>> 0;
    // hi FIRST, lo LAST (lo-last publish ordering — the reader gate)
    if (this.shared) {
      const w = byteOff >> 2;
      Atomics.store(this.u32, w + 1, hi);
      Atomics.store(this.u32, w, lo);
    } else {
      this.dv.setUint32(byteOff + 4, hi, true);
      this.dv.setUint32(byteOff, lo, true);
    }
  }

  // Publish ONE sample on ONE lane. nowNs: monotonic nanoseconds (Number).
  publishLane(lane, value, nowNs) {
    if (value !== value || value === Infinity || value === -Infinity) {
      throw new Hpl1Error(HPL1.INVALID_SAMPLE, `lane ${lane} sample ${value}`);
    }
    if ((lane >>> 0) >= this.geo.laneCount) {
      throw new Hpl1Error(HPL1.LANE_OUT_OF_RANGE, `lane ${lane} of ${this.geo.laneCount}`);
    }
    const ctrl = this.geo.laneCtrlBase + lane * 64;
    const seq = this.seq[lane] + 1;
    this._storeU64(ctrl + LANE.SEQ, seq); // odd
    const seen = this.samplesSeen[lane] + 1;
    const prevAvg = this.dv.getFloat64(ctrl + LANE.AVG, true);
    const wasActive = (this.dv.getUint32(ctrl + LANE.FLAGS, true) & LANE_FLAG_ACTIVE) !== 0;
    let min = this.dv.getFloat64(ctrl + LANE.MIN, true);
    let max = this.dv.getFloat64(ctrl + LANE.MAX, true);
    if (!wasActive || seen === 1) { min = value; max = value; }
    else { if (value < min) min = value; if (value > max) max = value; }
    const avg = wasActive ? prevAvg + (value - prevAvg) / seen : value;
    this.dv.setFloat64(ctrl + LANE.CURRENT, value, true);
    this.dv.setFloat64(ctrl + LANE.MIN, min, true);
    this.dv.setFloat64(ctrl + LANE.MAX, max, true);
    this.dv.setFloat64(ctrl + LANE.AVG, avg, true);
    // ring slot at head & mask
    const head = this.dv.getUint32(ctrl + LANE.HEAD, true);
    const ringByte = this.geo.ringBase + lane * this.geo.samplesPerLane * 8;
    this.dv.setFloat64(ringByte + (head & (this.geo.samplesPerLane - 1)) * 8, value, true);
    this.dv.setUint32(ctrl + LANE.HEAD, (head + 1) >>> 0, true);
    this.samplesSeen[lane] = seen;
    this.dv.setUint32(ctrl + LANE.SAMPLES_SEEN, seen >>> 0, true);
    this.dv.setUint32(ctrl + LANE.SAMPLES_SEEN + 4, Math.floor(seen / 0x100000000) >>> 0, true);
    this.dv.setUint32(ctrl + LANE.FLAGS,
      this.dv.getUint32(ctrl + LANE.FLAGS, true) | LANE_FLAG_ACTIVE, true);
    this._storeU64(ctrl + LANE.PUBLISH_NS, nowNs);
    this.seq[lane] = seq + 1;
    this._storeU64(ctrl + LANE.SEQ, seq + 1); // even — release
    // dirty bit: producer-set (Atomics.or on shared memory)
    const w = (128 >> 2) + (lane >> 5);
    const bit = 1 << (lane & 31);
    if (this.shared) Atomics.or(this.u32, w, bit); else this.u32[w] |= bit;
    this.lastValue = value;
    return seen;
  }

  // Start of a multi-lane tick: clear the dirty mask (HPL1 §3 set→publish→hold).
  beginBatch() {
    const words = this.geo.dirtyWords;
    for (let w = 0; w < words; w++) {
      if (this.shared) Atomics.store(this.u32, (128 >> 2) + w, 0);
      else this.u32[(128 >> 2) + w] = 0;
    }
  }

  // One producer tick across the first `count` lanes of `values` (caller-owned,
  // preallocated — Law 1). Clears the mask, publishes each lane, then takes the
  // header seqlock to publish the global stats block.
  publishTick(values, count, nowNs) {
    this.beginBatch();
    for (let lane = 0; lane < count; lane++) this.publishLane(lane, values[lane], nowNs);
    // global accumulators (producer-private, session-wide running window)
    for (let lane = 0; lane < this.geo.laneCount; lane++) {
      const ctrl = this.geo.laneCtrlBase + lane * 64;
      if ((this.dv.getUint32(ctrl + LANE.FLAGS, true) & LANE_FLAG_ACTIVE) === 0) continue;
      const c = this.dv.getFloat64(ctrl + LANE.CURRENT, true);
      const s = this.dv.getUint32(ctrl + LANE.SAMPLES_SEEN, true);
      if (s === 0) continue;
      if (c < this.gMin) this.gMin = c;
      if (c > this.gMax) this.gMax = c;
      this.gSum += c; this.gCount += 1;
    }
    const gSeq = this.gSeq + 1;
    this._storeU64(HDR.PUBLISH_SEQ, gSeq); // odd
    this.dv.setFloat64(HDR.GLOBAL_MIN, this.gMin, true);
    this.dv.setFloat64(HDR.GLOBAL_MAX, this.gMax, true);
    this.dv.setFloat64(HDR.GLOBAL_AVG, this.gCount > 0 ? this.gSum / this.gCount : 0, true);
    this.dv.setFloat64(HDR.GLOBAL_CURRENT, this.lastValue, true);
    this._storeU64(HDR.LAST_PUBLISH_NS, nowNs);
    this.gSeq = gSeq + 1;
    this._storeU64(HDR.PUBLISH_SEQ, gSeq + 1); // even — release
  }

  addDrops(n) {
    const cur = this.dv.getUint32(HDR.FRAMES_DROPPED, true) +
      this.dv.getUint32(HDR.FRAMES_DROPPED + 4, true) * 0x100000000;
    this._storeU64(HDR.FRAMES_DROPPED, cur + n);
  }

  // Producer restart: bump epoch FIRST (HPL1 §4.3) so every consumer sees an
  // explicit HPL1_EPOCH_CHANGED, then reset the session-window stats.
  epochRestart(nextEpoch) {
    const epoch = nextEpoch !== undefined ? nextEpoch : this.gEpoch + 1;
    const gSeq = this.gSeq + 1;
    this._storeU64(HDR.PUBLISH_SEQ, gSeq);
    this._storeU64(HDR.EPOCH, epoch);
    for (let lane = 0; lane < this.geo.laneCount; lane++) {
      const ctrl = this.geo.laneCtrlBase + lane * 64;
      this.dv.setFloat64(ctrl + LANE.MIN, 0, true);
      this.dv.setFloat64(ctrl + LANE.MAX, 0, true);
      this.dv.setFloat64(ctrl + LANE.AVG, 0, true);
      // clear ACTIVE so the next publish starts a FRESH min/max/avg window
      this.dv.setUint32(ctrl + LANE.FLAGS,
        this.dv.getUint32(ctrl + LANE.FLAGS, true) & ~LANE_FLAG_ACTIVE, true);
    }
    this.gMin = Infinity; this.gMax = -Infinity; this.gSum = 0; this.gCount = 0;
    this.gEpoch = epoch;
    this.gSeq = gSeq + 1;
    this._storeU64(HDR.PUBLISH_SEQ, gSeq + 1);
    return epoch;
  }

  // Expose the backing buffer (post to a Worker / hand to Engineer 1's bridge).
  get planeBuffer() { return this.buffer; }
}
