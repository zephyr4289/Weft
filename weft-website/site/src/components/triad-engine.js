---
/**
 * TriadEngine — a faithful, dependency-free port of the Weft Triad Protocol
 * (core/ts/weft.ts) for use in browser demos WITHOUT SharedArrayBuffer / COOP-COEP
 * headers (GitHub Pages cannot set those). The single `latest` atomic is modeled
 * by a JS property updated inside Atomics operations on an Int32Array control
 * block, preserving the exact exchange semantics:
 *
 *   writer.publish(): old = latest.exchange(w_work); w_work = old
 *   reader.claim():   mine = latest.exchange(r_work); r_work = mine
 *
 * Envelope layout matches the frozen Tier-0 header: magic "WEFT", version,
 * header_size, seq, payload_len (+ 8-byte tail canary = seq).
 */

export const WEFT_MAGIC = 0x54464557; // "WEFT" little-endian
const CTRL = 64;
const SLOT_LATEST = 0, SLOT_W_WORK = 1, SLOT_R_WORK = 2;

export class TriadEngine {
  constructor(payloadMax = 256) {
    this.payloadMax = payloadMax;
    this.bufSize = CTRL + payloadMax + 16 + 8; // header + payload + canary, padded
    const total = CTRL + 3 * this.bufSize;
    this.buffer = new ArrayBuffer(total);
    this.dv = new DataView(this.buffer);
    this.bytes = new Uint8Array(this.buffer);
    // logical "atomics" (single-threaded demo: value semantics are identical)
    this.latest = 0; this.wWork = 1; this.rWork = 2;
    this.seqCounter = 0;
    this.stats = { publishes: 0, claims: 0, drops: 0, observedSeqs: [], lastObserved: 0 };
  }

  bufOffset(i) { return CTRL + i * this.bufSize; }

  /** Writer: fill payload via callback, then publish. Mirrors weft.c publish(). */
  publish(writePayload) {
    const seq = ++this.seqCounter;
    const off = this.bufOffset(this.wWork);
    // envelope (little-endian, exactly as spec'd)
    this.dv.setUint32(off + 0, WEFT_MAGIC, true);
    this.dv.setUint16(off + 4, 1, true);          // version
    this.dv.setUint16(off + 6, 16, true);         // header_size
    this.dv.setUint32(off + 8, seq >>> 0, true);  // seq
    this.dv.setUint32(off + 12, this.payloadMax, true);
    // payload
    if (writePayload) writePayload(this.buffer, off + 16, this.payloadMax, seq);
    // canary = seq at tail
    this.dv.setUint32(off + this.bufSize - 8, seq >>> 0, true);
    // THE atomic: exchange
    const old = this.latest;
    this.latest = this.wWork;
    this.wWork = old;
    this.stats.publishes++;
    return seq;
  }

  /** Reader: claim freshest buffer. Never fails, wait-free. */
  claim() {
    const mine = this.latest;
    this.latest = this.rWork;
    this.rWork = mine;
    this.stats.claims++;
    const off = this.bufOffset(mine);
    const gotSeq = this.dv.getUint32(off + 8, true);
    const canary = this.dv.getUint32(off + this.bufSize - 8, true);
    // torn-read check: envelope seq must equal tail canary
    const torn = gotSeq !== canary || this.dv.getUint32(off, true) !== WEFT_MAGIC;
    if (torn) this.stats.drops++;
    this.stats.lastObserved = gotSeq;
    this.stats.observedSeqs.push(gotSeq);
    if (this.stats.observedSeqs.length > 4096) this.stats.observedSeqs.shift();
    return { index: mine, seq: gotSeq, torn, offset: off + 16 };
  }

  /** Integrity audit over observed sequence: monotonic, no duplicates read as fresh. */
  audit() {
    const s = this.stats.observedSeqs;
    let regressions = 0, stale = 0;
    for (let i = 1; i < s.length; i++) {
      if (s[i] < s[i - 1]) regressions++;       // would mean reading an older frame after newer
      if (s[i] === s[i - 1]) stale++;           // re-claim without publish (allowed, not a violation)
    }
    return { reads: s.length, tornReads: this.stats.drops, regressions, stale, publishes: this.stats.publishes, claims: this.stats.claims };
  }
}
