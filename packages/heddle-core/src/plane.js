// src/plane.js — HotPlaneView: the zero-allocation consumer side of HPL1.
//
// Hot-path contract (Law 1):
//   * readHeader/readLane write into CALLER-PROVIDED `out` objects — the view
//     allocates NOTHING after construction (construction is cold path).
//   * seqlock gates use Atomics.load (SeqCst) on the u32 `lo` word when the
//     backing memory is a SharedArrayBuffer; plain loads on non-shared buffers
//     (fixture parsing / same-thread tests) are race-free by construction.
//   * Tears are counted (view.tears) and reported as HPL1_TORN_SEQLOCK — never
//     swallowed, and the partially-read value is NEVER returned (HPL1 §4.2).
//   * No BigInt: u64 fields are consumed as lo/hi u32 pairs.
//
// Law 2: every scalar field read goes through DataView with explicit
// little-endian = true. The ONLY bulk path is readRecent(), which uses a
// Float64Array fast path gated by a runtime host-endianness check, with an
// explicit-LE DataView fallback for big-endian hosts.

import { HPL1, Hpl1Error } from './errors.js';
import { HDR, LANE, validatePlane } from './layout.js';

const MAX_TRIES = 64;

function hostIsLittleEndian() {
  return new Uint8Array(new Float64Array([1.5]).buffer)[0] === 0;
}

export function makeLaneOut() {
  // Cold-path convenience for callers; mutate/reuse it forever after.
  return {
    seqLo: 0, seqHi: 0,
    current: 0, min: 0, max: 0, avg: 0,
    samplesSeenLo: 0, samplesSeenHi: 0,
    head: 0, flags: 0,
    publishNsLo: 0, publishNsHi: 0,
    drops: 0,
  };
}

export function makeHeaderOut() {
  return {
    publishSeqLo: 0, publishSeqHi: 0,
    epochLo: 0, epochHi: 0,
    lastPublishNsLo: 0, lastPublishNsHi: 0,
    framesDroppedLo: 0, framesDroppedHi: 0,
    globalMin: 0, globalMax: 0, globalAvg: 0, globalCurrent: 0,
    tickHz: 0, flags: 0,
  };
}

export class HotPlaneView {
  constructor(buffer, opts = {}) {
    const byteOffset = opts.byteOffset || 0;
    if (byteOffset % 8 !== 0) {
      throw new Hpl1Error(HPL1.PLANE_DETACHED, `byteOffset ${byteOffset} not 8-aligned`);
    }
    const byteLength = opts.byteLength >= 0 ? opts.byteLength : buffer.byteLength - byteOffset;
    this.geo = validatePlane(buffer, byteOffset, byteLength); // throws fail-closed
    this.buffer = buffer;
    this.byteOffset = byteOffset;
    this.byteLength = byteLength;
    this.shared = typeof SharedArrayBuffer !== 'undefined' && buffer instanceof SharedArrayBuffer;
    this.dv = new DataView(buffer, byteOffset, byteLength);
    this.u32 = new Uint32Array(buffer, byteOffset, byteLength >> 2);
    this.leHost = hostIsLittleEndian();
    this.f64 = this.leHost ? new Float64Array(buffer, byteOffset, byteLength >> 3) : null;
    this.maxTries = opts.maxTries || MAX_TRIES;

    // consumer-owned state (preallocated at attach — never on the hot path)
    this.tears = 0;
    this.lastEpochLo = -1; this.lastEpochHi = -1;
    this.lastMask = new Uint32Array(this.geo.dirtyWords);
    this.dirtyCount = 0;
  }

  get laneCount() { return this.geo.laneCount; }
  get samplesPerLane() { return this.geo.samplesPerLane; }

  _loadSeqLo(byteOff) {
    const w = byteOff >> 2;
    return this.shared ? Atomics.load(this.u32, w) : this.u32[w];
  }

  // Header seqlock acquire → fills `out` in place.
  // Returns 0, or HPL1_EPOCH_CHANGED (out still filled with the NEW header),
  // or HPL1_TORN_SEQLOCK (out untouched — value NOT returned, HPL1 §4.2).
  readHeader(out) {
    const tries = this.maxTries;
    for (let t = 0; t < tries; t++) {
      const s1 = this._loadSeqLo(HDR.PUBLISH_SEQ);
      if ((s1 & 1) !== 0) { this.tears++; continue; }
      out.publishSeqLo = s1;
      out.publishSeqHi = this.dv.getUint32(HDR.PUBLISH_SEQ + 4, true);
      out.epochLo = this.dv.getUint32(HDR.EPOCH, true);
      out.epochHi = this.dv.getUint32(HDR.EPOCH + 4, true);
      out.lastPublishNsLo = this.dv.getUint32(HDR.LAST_PUBLISH_NS, true);
      out.lastPublishNsHi = this.dv.getUint32(HDR.LAST_PUBLISH_NS + 4, true);
      out.framesDroppedLo = this.dv.getUint32(HDR.FRAMES_DROPPED, true);
      out.framesDroppedHi = this.dv.getUint32(HDR.FRAMES_DROPPED + 4, true);
      out.globalMin = this.dv.getFloat64(HDR.GLOBAL_MIN, true);
      out.globalMax = this.dv.getFloat64(HDR.GLOBAL_MAX, true);
      out.globalAvg = this.dv.getFloat64(HDR.GLOBAL_AVG, true);
      out.globalCurrent = this.dv.getFloat64(HDR.GLOBAL_CURRENT, true);
      out.tickHz = this.dv.getUint32(HDR.TICK_HZ, true);
      out.flags = this.dv.getUint32(HDR.FLAGS, true);
      const s2 = this._loadSeqLo(HDR.PUBLISH_SEQ);
      if (s1 !== s2) { this.tears++; continue; }
      if (out.epochLo !== this.lastEpochLo || out.epochHi !== this.lastEpochHi) {
        const first = this.lastEpochLo === -1 && this.lastEpochHi === -1;
        this.lastEpochLo = out.epochLo; this.lastEpochHi = out.epochHi;
        if (!first) return HPL1.EPOCH_CHANGED; // restart happened — explicit (Law 4)
      }
      return HPL1.OK;
    }
    return HPL1.TORN_SEQLOCK;
  }

  // Lane seqlock acquire → fills `out` in place.
  // Returns 0 | HPL1_TORN_SEQLOCK (out untouched). Throws on caller bugs only.
  readLane(lane, out) {
    if ((lane >>> 0) >= this.geo.laneCount) {
      throw new Hpl1Error(HPL1.LANE_OUT_OF_RANGE, `lane ${lane} of ${this.geo.laneCount}`);
    }
    const ctrl = this.geo.laneCtrlBase + lane * 64; // stride pinned (HPL1 §4)
    for (let t = 0; t < this.maxTries; t++) {
      const s1 = this._loadSeqLo(ctrl + LANE.SEQ);
      if ((s1 & 1) !== 0) { this.tears++; continue; }
      out.seqLo = s1;
      out.seqHi = this.dv.getUint32(ctrl + LANE.SEQ + 4, true);
      out.current = this.dv.getFloat64(ctrl + LANE.CURRENT, true);
      out.min = this.dv.getFloat64(ctrl + LANE.MIN, true);
      out.max = this.dv.getFloat64(ctrl + LANE.MAX, true);
      out.avg = this.dv.getFloat64(ctrl + LANE.AVG, true);
      out.samplesSeenLo = this.dv.getUint32(ctrl + LANE.SAMPLES_SEEN, true);
      out.samplesSeenHi = this.dv.getUint32(ctrl + LANE.SAMPLES_SEEN + 4, true);
      out.head = this.dv.getUint32(ctrl + LANE.HEAD, true);
      out.flags = this.dv.getUint32(ctrl + LANE.FLAGS, true);
      out.publishNsLo = this.dv.getUint32(ctrl + LANE.PUBLISH_NS, true);
      out.publishNsHi = this.dv.getUint32(ctrl + LANE.PUBLISH_NS + 4, true);
      out.drops = this.dv.getUint32(ctrl + LANE.DROPS, true);
      const s2 = this._loadSeqLo(ctrl + LANE.SEQ);
      if (s1 !== s2) { this.tears++; continue; }
      return HPL1.OK;
    }
    return HPL1.TORN_SEQLOCK;
  }

  // Copy the k NEWEST samples of a lane into outF64 (index 0 = newest).
  // Returns the number of samples written (< k ⇒ underrun: ring holds fewer),
  // or -1 when torn (retry externally). Zero allocation.
  readRecent(lane, k, outF64) {
    if ((lane >>> 0) >= this.geo.laneCount) {
      throw new Hpl1Error(HPL1.LANE_OUT_OF_RANGE, `lane ${lane} of ${this.geo.laneCount}`);
    }
    const ctrl = this.geo.laneCtrlBase + lane * 64;
    const ringByte = this.geo.ringBase + lane * this.geo.samplesPerLane * 8;
    const mask = this.geo.samplesPerLane - 1;
    for (let t = 0; t < this.maxTries; t++) {
      const s1 = this._loadSeqLo(ctrl + LANE.SEQ);
      if ((s1 & 1) !== 0) { this.tears++; continue; }
      const head = this.dv.getUint32(ctrl + LANE.HEAD, true);
      const seen = this.dv.getUint32(ctrl + LANE.SAMPLES_SEEN, true);
      const s2 = this._loadSeqLo(ctrl + LANE.SEQ);
      if (s1 !== s2) { this.tears++; continue; }
      const avail = seen < this.geo.samplesPerLane ? seen : this.geo.samplesPerLane;
      const n = k < avail ? k : avail;
      const f64w = ringByte >> 3;
      if (this.leHost) {
        const f = this.f64;
        for (let j = 0; j < n; j++) outF64[j] = f[f64w + ((head - 1 - j) & mask)];
      } else {
        for (let j = 0; j < n; j++) {
          outF64[j] = this.dv.getFloat64(ringByte + ((head - 1 - j) & mask) * 8, true);
        }
      }
      return n;
    }
    return -1;
  }

  // Dirty-mask transition scan (producer-set bits, HPL1 §3 — consumers NEVER
  // write). Fills changedOut with lane indices newly dirty since last scan;
  // returns how many were written. view.dirtyCount = currently-set bit count.
  scanDirty(changedOut) {
    const words = this.geo.dirtyWords;
    const base = 128 >> 2; // mask region starts at byte 128
    let written = 0, count = 0;
    for (let w = 0; w < words; w++) {
      const cur = this.shared ? Atomics.load(this.u32, base + w) : this.u32[base + w];
      const fresh = cur & ~this.lastMask[w];
      this.lastMask[w] = cur;
      if (fresh !== 0 && changedOut) {
        let bits = fresh;
        while (bits !== 0) {
          const bit = bits & -bits;
          changedOut[written++] = (w << 5) + (31 - Math.clz32(bit));
          bits ^= bit;
        }
      }
      let c = cur;
      while (c !== 0) { c &= c - 1; count++; }
    }
    this.dirtyCount = count;
    return written;
  }
}
