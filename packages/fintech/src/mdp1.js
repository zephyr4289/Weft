// mdp1.js — MDP1 aggregated book snapshot wire (docs/adapters/MANAGED-SEAMS-V1.md §4).
//
// Fixed 304-byte little-endian record. Double duty:
//   1. Cross-language parity artifact (sha256 over checkpoints is the proof)
//   2. Direct UI binding target: <WeftOrderBook /> reads this record via
//      DataView — book state NEVER crosses into React state (Law 4 / W4-06).
//
// Layout:
//   0   magic "MDP1" | 4 version u16 | 6 flags u16
//   8   seq u64 | 16 last_ts_ns u64 | 24 best_bid u32 | 28 best_ask u32
//   32  bid_levels[10] x12 {price,size,orders} | 152 ask_levels[10] x12
//   272 msg_count u64 | 280 trade_count u64 | 288 last_match u64
//   296 crc32 u32 (poly 0xEDB88320, init/final 0xFFFFFFFF, over [0,296))
//   300 reserved u32
//
// All-integer state — NO floats on the parity path (bit-determinism).

export const MDP1_SIZE = 304;
export const MDP1_MAGIC = 0x3150444d; // "MDP1" little-endian read
export const MDP1_VERSION = 1;
export const MDP1_TOP_LEVELS = 10;

export const F_BOOK_VALID = 1 << 0;
export const F_CROSSED = 1 << 1;
export const F_LOCKED = 1 << 2;

let _table = null;
export function crcTable() {
  if (_table) return _table;
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  _table = t;
  return t;
}

export function mdp1Crc32(u8, start = 0, end = 296) {
  const table = _table ?? crcTable();
  let crc = -1; // 0xFFFFFFFF
  for (let i = start; i < end; i++) {
    crc = (crc >>> 8) ^ table[(crc ^ u8[i]) & 0xff];
  }
  return (crc ^ -1) >>> 0;
}

// Pack the book state into `out` (Uint8Array/Buffer, length >= 304).
// Scratch arrays are caller- or book-owned (reused across snapshots).
export function packSnapshot(book, out, scratchBids, scratchAsks) {
  const dv = out instanceof DataView ? out : new DataView(out.buffer, out.byteOffset, out.byteLength);
  const u8 = out instanceof Uint8Array ? out : new Uint8Array(out.buffer, out.byteOffset, out.byteLength);
  dv.setUint32(0, MDP1_MAGIC, true);
  dv.setUint16(4, MDP1_VERSION, true);
  let flags = 0;
  if (book.msgsApplied > 0) flags |= F_BOOK_VALID;
  const bb = book.bestBid(), ba = book.bestAsk();
  if (bb > 0 && ba > 0) {
    if (bb > ba) flags |= F_CROSSED;
    else if (bb === ba) flags |= F_LOCKED;
  }
  dv.setUint16(6, flags, true);
  dv.setUint32(8, book.msgsApplied >>> 0, true);          // seq lo
  dv.setUint32(12, Math.floor(book.msgsApplied / 4294967296), true); // seq hi (small)
  writeU64(dv, 16, book.lastTs);
  dv.setUint32(24, bb >>> 0, true);
  dv.setUint32(28, ba >>> 0, true);

  const nBids = book.topLevelsOf(0, scratchBids);
  const nAsks = book.topLevelsOf(1, scratchAsks);
  const buy = book.buy, sell = book.sell, base = book.baseTick;
  for (let i = 0; i < MDP1_TOP_LEVELS; i++) {
    const o = 32 + i * 12;
    if (i < nBids) {
      const t = scratchBids[i] - base;
      dv.setUint32(o, scratchBids[i] >>> 0, true);
      dv.setUint32(o + 4, buy.aggSize[t], true);
      dv.setUint32(o + 8, buy.orderCount[t], true);
    } else {
      dv.setUint32(o, 0, true); dv.setUint32(o + 4, 0, true); dv.setUint32(o + 8, 0, true);
    }
    const a = 152 + i * 12;
    if (i < nAsks) {
      const t = scratchAsks[i] - base;
      dv.setUint32(a, scratchAsks[i] >>> 0, true);
      dv.setUint32(a + 4, sell.aggSize[t], true);
      dv.setUint32(a + 8, sell.orderCount[t], true);
    } else {
      dv.setUint32(a, 0, true); dv.setUint32(a + 4, 0, true); dv.setUint32(a + 8, 0, true);
    }
  }
  writeU64(dv, 272, book.msgsApplied);
  writeU64(dv, 280, book.tradeCount);
  writeU64(dv, 288, book.lastMatch);
  for (let i = 300; i < 304; i++) u8[i] = 0;
  dv.setUint32(296, mdp1Crc32(u8), true);
  return out;
}

export function writeU64(dv, off, value) {
  // value < 2^53 exact; split arithmetically (no BigInt on parity path)
  const hi = Math.floor(value / 4294967296);
  dv.setUint32(off, value - hi * 4294967296, true);
  dv.setUint32(off + 4, hi, true);
}

// Read-only flyweight over an MDP1 record (UI binding side).
export class Mdp1View {
  constructor(buffer) {
    if (buffer instanceof DataView) {
      this.dv = buffer;
    } else if (buffer instanceof ArrayBuffer) {
      this.dv = new DataView(buffer);
    } else {
      // typed-array / Buffer view — window over the underlying ArrayBuffer
      this.dv = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
    }
    this.u8 = new Uint8Array(this.dv.buffer, this.dv.byteOffset, this.dv.byteLength);
  }
  valid() { return this.dv.getUint32(0, true) === MDP1_MAGIC; }
  get version() { return this.dv.getUint16(4, true); }
  get flags() { return this.dv.getUint16(6, true); }
  get seq() { return this.dv.getUint32(8, true); }
  get lastTs() {
    return this.dv.getUint32(20, true) * 4294967296 + this.dv.getUint32(16, true);
  }
  get bestBid() { return this.dv.getUint32(24, true); }
  get bestAsk() { return this.dv.getUint32(28, true); }
  bidPrice(i) { return this.dv.getUint32(32 + i * 12, true); }
  bidSize(i) { return this.dv.getUint32(36 + i * 12, true); }
  bidOrders(i) { return this.dv.getUint32(40 + i * 12, true); }
  askPrice(i) { return this.dv.getUint32(152 + i * 12, true); }
  askSize(i) { return this.dv.getUint32(156 + i * 12, true); }
  askOrders(i) { return this.dv.getUint32(160 + i * 12, true); }
  get msgCount() { return this.dv.getUint32(272, true); }
  get tradeCount() { return this.dv.getUint32(280, true); }
  crcOk() { return this.dv.getUint32(296, true) === mdp1Crc32(this.u8); }
}
