// itch.js — NASDAQ TotalView-ITCH 5.0 flyweight parser + feed engine.
//
// Zero-allocation contract (docs/adapters/MANAGED-SEAMS-V1.md §2):
//   - One ItchView binds to the feed buffer once; `bind(offset)` re-points it.
//     No slices, no strings, no objects, no closures per message.
//   - All multi-byte fields are BIG-endian (network order) per the public
//     ITCH 5.0 spec. u48 timestamps reconstruct as hi*2**32+lo (exact: < 2^53).
//   - Order refs are exposed as lo/hi u32 PAIRS — never materialized as
//     doubles (refs may exceed 2^53; golden fixtures deliberately include
//     hi = 0x00C0FFEE refs).
//   - Unknown message types are SKIPPED by the u16 length prefix (counted).
//     Corrupt framing (length beyond buffer) fails closed with E_TRUNC.
//
// The view's semantic getters are TYPE-AWARE: message type is known at bind
// time, so `shares`/`price`/`match` resolve to the correct wire offsets for
// that message (e.g. replace shares at +27, trade match at +36). The book
// only ever sees semantic names.

export const E_TRUNC = 1;

export const T_SYSTEM = 0x53;      // 'S'
export const T_ADD = 0x41;         // 'A'
export const T_ADD_MPID = 0x46;    // 'F'
export const T_EXEC = 0x45;        // 'E'
export const T_EXEC_PRICE = 0x43;  // 'C'
export const T_CANCEL = 0x58;      // 'X'
export const T_DELETE = 0x44;      // 'D'
export const T_REPLACE = 0x55;     // 'U'
export const T_TRADE = 0x50;       // 'P'

export class ItchView {
  constructor(buffer) {
    this.dv = buffer instanceof DataView ? buffer : new DataView(buffer);
    this.off = 0;
    this.type = 0;
  }
  bind(off) {
    this.off = off;
    this.type = this.dv.getUint8(off);
    return this;
  }
  get locate() { return this.dv.getUint16(this.off + 1, false); }
  get tracking() { return this.dv.getUint16(this.off + 3, false); }
  get ts() {
    return this.dv.getUint16(this.off + 5, false) * 4294967296 +
      this.dv.getUint32(this.off + 7, false);
  }
  // u64 order ref as lo/hi u32 pair (BE wire order: hi at +11, lo at +15)
  get refLo() { return this.dv.getUint32(this.off + 15, false); }
  get refHi() { return this.dv.getUint32(this.off + 11, false); }
  // second ref (replace new-order id: hi at +19, lo at +23)
  get ref2Lo() { return this.dv.getUint32(this.off + 23, false); }
  get ref2Hi() { return this.dv.getUint32(this.off + 19, false); }
  get side() { return this.dv.getUint8(this.off + 19); } // 0x42 buy / 0x53 sell
  get shares() {
    const o = this.off;
    const t = this.type;
    // 'U' at +27; executed/cancelled shares ('E','C','X') at +19;
    // add/trade shares ('A','F','P') at +20
    if (t === T_REPLACE) return this.dv.getUint32(o + 27, false);
    if (t === T_EXEC || t === T_EXEC_PRICE || t === T_CANCEL)
      return this.dv.getUint32(o + 19, false);
    return this.dv.getUint32(o + 20, false);
  }
  get price() {
    const o = this.off;
    // 'A'/'F'/'P'/'C' at +32; 'U' at +31
    return this.type === T_REPLACE
      ? this.dv.getUint32(o + 31, false)
      : this.dv.getUint32(o + 32, false);
  }
  get match() {
    const o = this.off;
    if (this.type === T_TRADE) {
      return this.dv.getUint32(o + 36, false) * 4294967296 +
        this.dv.getUint32(o + 40, false);
    }
    return this.dv.getUint32(o + 23, false) * 4294967296 +
      this.dv.getUint32(o + 27, false);
  }
  eventCode() { return String.fromCharCode(this.dv.getUint8(this.off + 11)); }
  // Stock symbol as raw bytes into caller-owned scratch (no string alloc).
  stockInto(u8scratch) {
    const off = this.off + 24;
    for (let i = 0; i < 8; i++) u8scratch[i] = this.dv.getUint8(off + i);
    return u8scratch;
  }
}

// Walk framing and return the view-relative byte offset of each message
// START (payload start, after the 2-byte length prefix). Cold path.
export function frameOffsets(buffer, byteLength, maxCount = 1 << 20) {
  const dv = buffer instanceof DataView ? buffer : new DataView(buffer.buffer ?? buffer, buffer.byteOffset ?? 0, byteLength);
  const out = new Int32Array(maxCount);
  let off = 0;
  let n = 0;
  while (off + 2 <= byteLength && n < maxCount) {
    const len = dv.getUint16(off, false);
    out[n++] = off + 2;
    off += 2 + len;
  }
  return out.subarray(0, n);
}

// Feed engine: walks `[u16 BE len][payload]` framing, dispatches known
// messages into the book. One allocation total (the view), at construction.
export class ItchEngine {
  constructor(book, opts = {}) {
    this.book = book;
    this.view = new ItchView(new DataView(opts.buffer ?? new ArrayBuffer(64)));
    this.truncated = 0;
    this.bytesProcessed = 0;
    this.parseNs = 0; // accumulated parse+apply ns when a clock is supplied
    this._clock = opts.clock ?? null;
    this._resume = 0; // view-relative byte offset to resume from
  }

  // Returns E_TRUNC on corrupt framing, else 0. Counters live on the book.
  // `input` may be an ArrayBuffer, a DataView (windowed), or a typed-array
  // view (Buffer/subarray) — byte offsets are VIEW-RELATIVE. Processing
  // resumes from the previous end byte (engine keeps `_resume`), so a feed
  // can be pumped incrementally for checkpoint snapshots.
  process(input, endByte) {
    let dv;
    let len;
    if (input instanceof DataView) {
      dv = input;
      len = endByte ?? input.byteLength;
    } else if (input instanceof ArrayBuffer) {
      dv = new DataView(input);
      len = endByte ?? input.byteLength;
    } else {
      dv = new DataView(input.buffer, input.byteOffset, input.byteLength);
      len = endByte ?? input.byteLength;
    }
    const view = this.view;
    view.dv = dv;
    // Resume semantics apply ONLY to the same underlying buffer (incremental
    // checkpoint pumping over one stream). A new chunk buffer restarts at 0.
    if (this._lastBuffer !== dv.buffer || this._lastOffset !== dv.byteOffset) {
      this._resume = 0;
      this._lastBuffer = dv.buffer;
      this._lastOffset = dv.byteOffset;
    }
    const t0 = this._clock !== null ? this._clock() : 0;
    const book = this.book;
    let off = this._resume;
    let result = 0;
    while (off + 2 <= len) {
      const msgLen = dv.getUint16(off, false);
      if (off + 2 + msgLen > len) {
        this.truncated++;
        result = E_TRUNC;
        break;
      }
      view.bind(off + 2);
      book.applyView(view);
      this.bytesProcessed += 2 + msgLen;
      off += 2 + msgLen;
    }
    this._resume = off;
    if (this._clock !== null) this.parseNs += this._clock() - t0;
    return result;
  }
}
