// l2feed.ts — W6: L2 order-book feed engine (SIMULATED microstructure)
//
// WHY EXISTS: The maintainer's post-v0.1.0 frontier invitation names
// "High-Frequency Financial & Telemetry Feeds (L2/L3 order book streams …
// no GC scan overhead)" as a driver-layer direction. The existing W5
// workload is a stateless cosmetic ladder (fresh random floats per display
// frame — no microstructure, no delta stream, no feed-rate pressure). W6
// closes that gap with a real ingestion model: a synthetic feed emits K
// delta messages per display tick (the feed:display pressure regime where
// naive reactive UIs allocate per message), an order-book engine folds all
// K deltas into one latest-wins frame, and the A/B/C/D mode runners carry
// that frame to the canvas through their four buffer strategies.
//
// HONESTY LABELS (Law 4, F-6/E2 precedent — the workload title and README
// carry these too):
//   - SIMULATED FEED: synthetic microstructure from a deterministic LCG
//     (seed pinned below). No exchange connectivity, no market-data claim.
//   - DEMO SCALE: 64 levels x 6 fields + 64-trade tape = 584 floats; a real
//     L2 snapshot is comparable in size, but nothing here was measured
//     against a real venue feed.
//   - MODEL BOUNDARY: prices live on a fixed 0.01 grid recomputed from a
//     walking mid (integer cents — see below); sizes/counts evolve via
//     add/modify/cancel/trade deltas. Price-time queue positioning (L3) is
//     out of scope. The model exercises ALLOCATION and LATEST-WINS FOLD
//     pressure, not market microstructure fidelity.
//
// INTEGER-CENTS DISCIPLINE: every book quantity is an integer (prices in
// cents, sizes in lots, counts in orders). Float values exist ONLY at the
// export boundary as exact single divisions (cents/100), so all four mode
// runners (including the object-based Mode A) produce BIT-IDENTICAL frames
// from the same seed — the property the matrix cross-mode equivalence test
// asserts. A per-frame checksum (w6Checksum) lets any consumer validate a
// claimed frame end-to-end: the fan-out feed views and the cross-thread
// litmus recompute it on every fresh claim.
//
// ZERO ALLOCATION (Law 2): the engine and the feed allocate ALL state at
// construction. The hot path (nextTick / applyBatch / exportFrame) writes
// into caller-owned buffers through scalar arguments and typed arrays —
// no per-message objects, no closures, no array growth. The measured
// evidence for this claim is the feed GC harness
// (scripts/feed_gc_bench.ts + evidence/feed-gc-bench.log), not an
// assertion here.

/// W6 layout constants — the payload contract shared by every mode runner,
/// the fan-out views, and the cross-thread litmus. Single source of truth.
export const W6_LEVELS = 64;
export const W6_TAPE = 64;
export const W6_HEADER_FLOATS = 8;
export const W6_LADDER_FIELDS = 6; // bidPx, bidSz, bidN, askPx, askSz, askN
export const W6_TAPE_FIELDS = 3; // px, sz, side (+1 buy / -1 sell / 0 pad)
export const W6_FLOAT_COUNT =
  W6_HEADER_FLOATS + W6_LEVELS * W6_LADDER_FIELDS + W6_TAPE * W6_TAPE_FIELDS; // 584

/// Messages folded per display tick — the pinned feed:display ratio. 50:1
/// at 60 Hz display = 3,000 msgs/s in the demo (labeled; a real feed is
/// orders of magnitude faster, which is why the fold is latest-wins).
export const W6_FEED_TO_DISPLAY = 50;

/// LCG seed for the W6 stream. Derived from the W-suite family seed
/// (0x00c0ffee) so W6's sequence never collides with W1–W5's: fixed
/// documentable constant, nothing magic.
const W6_SEED = (0x00c0ffee ^ 0x00000606) >>> 0;

/// Delta message op codes (plain const object — repo style, no enums).
export const L2Op = { Add: 0, Modify: 1, Cancel: 2, Trade: 3 } as const;
export type L2Op = (typeof L2Op)[keyof typeof L2Op];

/// Side codes.
export const L2Side = { Bid: 0, Ask: 1 } as const;
export type L2Side = (typeof L2Side)[keyof typeof L2Side];

/// Message tuple width in the batch buffer: [op, side, level, qty, n, pxC].
export const W6_MSG_FIELDS = 6;

/// Frame checksum modulus (2^23 - 1): keeps the accumulator below 2^24 so
/// every intermediate is exactly representable in float32 — the checksum
/// round-trips through the payload bit-exactly.
const CHECK_MOD = 8388607;

// ---------------------------------------------------------------------------
// Frame checksum — pure function of the payload. Covers every float except
// index [7] (itself). Integer-exact by construction: every covered field is
// either an integer (tick, folded, depths, sizes, counts, tape size/side)
// or a cents/100 price re-quantized by Math.round(v * 100) — float32's
// ~1e-7 relative error at price magnitude <= ~105 gives ~1e-5 absolute
// error, far below the 0.5 rounding threshold. The engine computes it after
// export; readers recompute it on fresh claims; Mode A computes it over its
// snapshot. ONE function, all validators.
// ---------------------------------------------------------------------------

export function w6Checksum(p: Float32Array): number {
  let h = 7;
  h = (h * 131 + Math.round(p[0])) % CHECK_MOD; // tick
  h = (h * 131 + Math.round(p[1])) % CHECK_MOD; // folded
  h = (h * 131 + Math.round(p[2] * 100)) % CHECK_MOD; // mid (cents)
  h = (h * 131 + Math.round(p[3] * 100)) % CHECK_MOD; // spread (cents)
  h = (h * 131 + Math.round(p[4])) % CHECK_MOD; // bid depth (lots)
  h = (h * 131 + Math.round(p[5])) % CHECK_MOD; // ask depth (lots)
  h = (h * 131 + Math.round(p[6] * 100)) % CHECK_MOD; // last trade (cents)
  const lad = W6_HEADER_FLOATS;
  for (let i = 0; i < W6_LEVELS; i++) {
    const o = lad + i * W6_LADDER_FIELDS;
    h = (h * 131 + Math.round(p[o] * 100)) % CHECK_MOD; // bidPx
    h = (h * 131 + Math.round(p[o + 1])) % CHECK_MOD; // bidSz
    h = (h * 131 + Math.round(p[o + 2])) % CHECK_MOD; // bidN
    h = (h * 131 + Math.round(p[o + 3] * 100)) % CHECK_MOD; // askPx
    h = (h * 131 + Math.round(p[o + 4])) % CHECK_MOD; // askSz
    h = (h * 131 + Math.round(p[o + 5])) % CHECK_MOD; // askN
  }
  const tape = lad + W6_LEVELS * W6_LADDER_FIELDS;
  for (let j = 0; j < W6_TAPE; j++) {
    const o = tape + j * W6_TAPE_FIELDS;
    h = (h * 131 + Math.round(p[o] * 100)) % CHECK_MOD; // tape px
    h = (h * 131 + Math.round(p[o + 1])) % CHECK_MOD; // tape sz
    h = (h * 131 + Math.round(p[o + 2]) + 1) % CHECK_MOD; // side+1: {-1,0,1} -> {0,1,2}
  }
  return h;
}

// ---------------------------------------------------------------------------
// SyntheticL2Feed — the deterministic message source.
//
// One LCG (the W-suite generator discipline: s = s*1664525 + 1013904223
// mod 2^32) drives everything: the mid's random walk, the per-tick quote
// grid offsets, and every message. Same seed => same stream => all four
// mode runners ingest identical deltas => cross-mode byte equivalence.
//
// Per tick: draw bidOff/askOff in [1..4] cents (the top-of-book spread
// breathes in [2..8] cents), then emit K messages with mix ~10% trades,
// 20% adds, 55% modifies, 15% cancels — the shape that makes sizes evolve
// continuously (the workload's visual texture).
//
// The mid walks +/- 1 cent with p ~ 0.25 per MESSAGE, clamped to
// [9500, 10500] cents. All output values are integers.
// ---------------------------------------------------------------------------

export class SyntheticL2Feed {
  /// LCG state (uint32). MUST carry an initializer: under ES2022
  /// define-semantics (tsc useDefineForClassFields / node type stripping)
  /// an initializer-less field is DEFINED as undefined, pinning TAGGED
  /// representation — and every write of a uint32 above the SMI range
  /// (half of all LCG steps) would box a HeapNumber: ~1 allocation per
  /// two RNG calls on the feed's hot path. `= 0` starts the field as SMI
  /// so V8 migrates it to a raw double field on first wide value — zero
  /// boxing in steady state. Measured before/after in the GC harness
  /// (evidence/feed-gc-bench.log): this one line was ~1.8 KB/tick.
  private s = 0;
  /// Current mid price in integer cents (grid: multiples of 1 cent).
  midC = 10000;
  /// Top-of-book offsets in cents, redrawn each tick (read by the runner
  /// after nextTick to set the engine's quote grid).
  bidOff = 2;
  askOff = 2;
  /// Total messages emitted (advisory).
  emitted = 0;

  constructor(seed: number = W6_SEED) {
    this.s = seed >>> 0;
  }

  private rng(): number {
    this.s = (Math.imul(this.s, 1664525) + 1013904223) >>> 0;
    return this.s / 4294967296;
  }

  /// Fill `dst` (length >= K * W6_MSG_FIELDS) with the next tick's K
  /// messages as [op, side, level, qty, n, pxC] tuples. Mutates the walk
  /// state. Zero allocation: writes only into the caller's buffer.
  nextTick(dst: Float64Array): void {
    this.bidOff = 1 + Math.floor(this.rng() * 4);
    this.askOff = 1 + Math.floor(this.rng() * 4);
    const K = W6_FEED_TO_DISPLAY;
    for (let m = 0; m < K; m++) {
      // Mid walk (per message): +/- 1 cent, clamped.
      if (this.rng() < 0.25) {
        this.midC += this.rng() < 0.5 ? -1 : 1;
        if (this.midC < 9500) this.midC = 9500;
        if (this.midC > 10500) this.midC = 10500;
      }
      const u = this.rng();
      const side = this.rng() < 0.5 ? L2Side.Bid : L2Side.Ask;
      const o = m * W6_MSG_FIELDS;
      if (u < 0.1) {
        // TRADE: aggressor consumes `side`'s book at `level`.
        const level = Math.floor(this.rng() * 4);
        const qty = 1 + Math.floor(this.rng() * 8);
        const pxC =
          side === L2Side.Ask
            ? this.midC + this.askOff + level
            : this.midC - this.bidOff - level;
        dst[o] = L2Op.Trade; dst[o + 1] = side; dst[o + 2] = level;
        dst[o + 3] = qty; dst[o + 4] = 0; dst[o + 5] = pxC;
      } else if (u < 0.3) {
        // ADD: fresh quote replaces the level's size and count.
        const level = Math.floor(this.rng() * W6_LEVELS);
        const qty = 5 + Math.floor(this.rng() * 95);
        const n = 1 + Math.floor(this.rng() * 9);
        dst[o] = L2Op.Add; dst[o + 1] = side; dst[o + 2] = level;
        dst[o + 3] = qty; dst[o + 4] = n; dst[o + 5] = 0;
      } else if (u < 0.85) {
        // MODIFY: signed size delta; 0 is remapped to +1 (deterministic).
        let delta = Math.floor(this.rng() * 25) - 12;
        if (delta === 0) delta = 1;
        const level = Math.floor(this.rng() * W6_LEVELS);
        dst[o] = L2Op.Modify; dst[o + 1] = side; dst[o + 2] = level;
        dst[o + 3] = delta; dst[o + 4] = 0; dst[o + 5] = 0;
      } else {
        // CANCEL: remove quantity (and one order) from the level.
        const level = Math.floor(this.rng() * W6_LEVELS);
        const qty = 1 + Math.floor(this.rng() * 6);
        dst[o] = L2Op.Cancel; dst[o + 1] = side; dst[o + 2] = level;
        dst[o + 3] = qty; dst[o + 4] = 0; dst[o + 5] = 0;
      }
      this.emitted++;
    }
  }
}

// ---------------------------------------------------------------------------
// L2BookEngine — the SoA (structure-of-arrays) book. Used by Modes B, C,
// and D and by the fan-out feed views. All state is integer typed arrays
// allocated in the constructor; the hot path never allocates.
//
// Semantics (mirrored exactly by ObjectL2Book below — the equivalence test
// asserts the mirror bit-for-bit):
//   Add:    sz = qty;            n = nIn            (fresh quote)
//   Modify: sz += delta (>= 0);  n unchanged        (delta may be negative)
//   Cancel: sz -= qty  (>= 0);   n = max(0, n - 1); if sz == 0 -> n = 0
//   Trade:  sz -= qty  (>= 0);   n = max(0, n - 1); if sz == 0 -> n = 0;
//           tape <- (pxC, qty, side === Ask ? +1 : -1); lastTradePxC = pxC
// ---------------------------------------------------------------------------

export class L2BookEngine {
  readonly levels = W6_LEVELS;
  readonly tapeCap = W6_TAPE;

  /// Quote grid (set by the runner from the feed before each batch).
  midC = 10000;
  bidOff = 2;
  askOff = 2;

  private bidSz: Int32Array;
  private bidN: Int32Array;
  private askSz: Int32Array;
  private askN: Int32Array;
  /// Tape as three parallel rings (oldest-first window, copyWithin slide).
  private tapePxC: Int32Array;
  private tapeSzA: Int32Array;
  private tapeSideA: Int32Array;
  private tapeCount = 0;

  /// Advisory counters.
  msgSeq = 0;
  lastTradePxC = 0;
  private foldedThisTick = 0;

  constructor() {
    this.bidSz = new Int32Array(W6_LEVELS);
    this.bidN = new Int32Array(W6_LEVELS);
    this.askSz = new Int32Array(W6_LEVELS);
    this.askN = new Int32Array(W6_LEVELS);
    this.tapePxC = new Int32Array(W6_TAPE);
    this.tapeSzA = new Int32Array(W6_TAPE);
    this.tapeSideA = new Int32Array(W6_TAPE);
    // Deterministic initial book (pure formula — no LCG, so every mode and
    // every test constructs the identical starting state).
    for (let i = 0; i < W6_LEVELS; i++) {
      this.bidSz[i] = 20 + ((i * 7) % 61);
      this.askSz[i] = 20 + ((i * 11) % 61);
      this.bidN[i] = 1 + ((i * 3) % 7);
      this.askN[i] = 1 + ((i * 5) % 7);
    }
  }

  /// Set the quote grid for the coming batch (from the feed's tick state).
  setGrid(midC: number, bidOff: number, askOff: number): void {
    this.midC = midC;
    this.bidOff = bidOff;
    this.askOff = askOff;
  }

  /// Apply ONE delta message (scalar args — zero allocation).
  apply(op: number, side: number, level: number, qty: number, n: number, pxC: number): void {
    const szArr = side === L2Side.Bid ? this.bidSz : this.askSz;
    const nArr = side === L2Side.Bid ? this.bidN : this.askN;
    if (level < 0 || level >= W6_LEVELS) return; // defensive: drop malformed
    if (op === L2Op.Add) {
      szArr[level] = qty;
      nArr[level] = n;
    } else if (op === L2Op.Modify) {
      const v = szArr[level] + qty;
      szArr[level] = v < 0 ? 0 : v;
      if (szArr[level] === 0) nArr[level] = 0;
    } else if (op === L2Op.Cancel) {
      const v = szArr[level] - qty;
      szArr[level] = v < 0 ? 0 : v;
      nArr[level] = nArr[level] > 0 ? nArr[level] - 1 : 0;
      if (szArr[level] === 0) nArr[level] = 0;
    } else if (op === L2Op.Trade) {
      const v = szArr[level] - qty;
      szArr[level] = v < 0 ? 0 : v;
      nArr[level] = nArr[level] > 0 ? nArr[level] - 1 : 0;
      if (szArr[level] === 0) nArr[level] = 0;
      // Tape append with in-place slide when full (copyWithin — no alloc).
      if (this.tapeCount === this.tapeCap) {
        this.tapePxC.copyWithin(0, 1);
        this.tapeSzA.copyWithin(0, 1);
        this.tapeSideA.copyWithin(0, 1);
        this.tapeCount--;
      }
      this.tapePxC[this.tapeCount] = pxC;
      this.tapeSzA[this.tapeCount] = qty;
      this.tapeSideA[this.tapeCount] = side === L2Side.Ask ? 1 : -1;
      this.tapeCount++;
      this.lastTradePxC = pxC;
    }
    this.msgSeq++;
    this.foldedThisTick++;
  }

  /// Apply a whole batch from the feed buffer ([op, side, level, qty, n,
  /// pxC] tuples). Zero allocation.
  applyBatch(batch: Float64Array, count: number): void {
    for (let m = 0; m < count; m++) {
      const o = m * W6_MSG_FIELDS;
      this.apply(batch[o], batch[o + 1], batch[o + 2], batch[o + 3], batch[o + 4], batch[o + 5]);
    }
  }

  /// Reset the per-tick fold counter — call BEFORE applying the tick's
  /// batch (the runner calls it once per tick; exportFrame then reports
  /// the count of messages actually folded).
  beginTick(): void {
    this.foldedThisTick = 0;
  }

  /// Export the folded book state into `out` (caller-owned Float32Array of
  /// length W6_FLOAT_COUNT) as tick `tick`. Layout:
  ///   [0] tick  [1] folded  [2] mid  [3] spread  [4] bidDepth  [5] askDepth
  ///   [6] lastTradePx  [7] checksum   then ladder (6/level), tape (3/entry)
  /// Prices are exact single divisions (cents/100); depths are integer sums
  /// accumulated in level order 0..L-1 (order pinned for cross-mode bit
  /// equality). The checksum is computed LAST over the finished payload.
  exportFrame(out: Float32Array, tick: number): void {
    out[0] = tick;
    out[1] = this.foldedThisTick;
    out[2] = this.midC / 100;
    out[3] = (this.bidOff + this.askOff) / 100;
    let bidDepth = 0;
    let askDepth = 0;
    for (let i = 0; i < W6_LEVELS; i++) {
      bidDepth += this.bidSz[i];
      askDepth += this.askSz[i];
    }
    out[4] = bidDepth;
    out[5] = askDepth;
    out[6] = this.lastTradePxC / 100;
    const lad = W6_HEADER_FLOATS;
    for (let i = 0; i < W6_LEVELS; i++) {
      const o = lad + i * W6_LADDER_FIELDS;
      out[o] = (this.midC - this.bidOff - i) / 100;
      out[o + 1] = this.bidSz[i];
      out[o + 2] = this.bidN[i];
      out[o + 3] = (this.midC + this.askOff + i) / 100;
      out[o + 4] = this.askSz[i];
      out[o + 5] = this.askN[i];
    }
    const tape = lad + W6_LEVELS * W6_LADDER_FIELDS;
    for (let j = 0; j < W6_TAPE; j++) {
      const o = tape + j * W6_TAPE_FIELDS;
      if (j < this.tapeCount) {
        out[o] = this.tapePxC[j] / 100;
        out[o + 1] = this.tapeSzA[j];
        out[o + 2] = this.tapeSideA[j];
      } else {
        out[o] = 0;
        out[o + 1] = 0;
        out[o + 2] = 0;
      }
    }
    out[7] = w6Checksum(out);
  }
}

// ---------------------------------------------------------------------------
// ObjectL2Book — the NAIVE object-graph mirror used by Mode A only.
//
// WHY EXISTS: Mode A models the reactive baseline honestly: every feed
// message is materialized as an event object, the book is an object graph
// (array of {sz, n} per side), the tape is an array of trade objects, and
// the snapshot is a FRESH Float32Array per tick. That is the allocation
// profile the GC harness measures against the SoA path. It is a MODEL of
// per-event reactive state management — not a measurement of any specific
// framework (stated in the README and the bench log).
//
// The arithmetic mirrors L2BookEngine exactly (same integer state, same
// export expressions, same level-order sums) so the cross-mode equivalence
// test can assert bit-identical frames.
// ---------------------------------------------------------------------------

interface ObjLevel {
  sz: number;
  n: number;
}
interface ObjTrade {
  pxC: number;
  sz: number;
  side: number;
}

export class ObjectL2Book {
  midC = 10000;
  bidOff = 2;
  askOff = 2;
  private bid: ObjLevel[];
  private ask: ObjLevel[];
  private tape: ObjTrade[];
  msgSeq = 0;
  lastTradePxC = 0;
  private foldedThisTick = 0;

  constructor() {
    this.bid = [];
    this.ask = [];
    this.tape = [];
    for (let i = 0; i < W6_LEVELS; i++) {
      this.bid.push({ sz: 20 + ((i * 7) % 61), n: 1 + ((i * 3) % 7) });
      this.ask.push({ sz: 20 + ((i * 11) % 61), n: 1 + ((i * 5) % 7) });
    }
  }

  setGrid(midC: number, bidOff: number, askOff: number): void {
    this.midC = midC;
    this.bidOff = bidOff;
    this.askOff = askOff;
  }

  /// Apply ONE message OBJECT (Mode A materializes one per feed message —
  /// the allocation the naive path pays on purpose).
  applyMsg(ev: { op: number; side: number; level: number; qty: number; n: number; pxC: number }): void {
    const arr = ev.side === L2Side.Bid ? this.bid : this.ask;
    const lvl = arr[ev.level];
    if (lvl === undefined) return;
    if (ev.op === L2Op.Add) {
      lvl.sz = ev.qty;
      lvl.n = ev.n;
    } else if (ev.op === L2Op.Modify) {
      const v = lvl.sz + ev.qty;
      lvl.sz = v < 0 ? 0 : v;
      if (lvl.sz === 0) lvl.n = 0;
    } else if (ev.op === L2Op.Cancel) {
      const v = lvl.sz - ev.qty;
      lvl.sz = v < 0 ? 0 : v;
      lvl.n = lvl.n > 0 ? lvl.n - 1 : 0;
      if (lvl.sz === 0) lvl.n = 0;
    } else if (ev.op === L2Op.Trade) {
      const v = lvl.sz - ev.qty;
      lvl.sz = v < 0 ? 0 : v;
      lvl.n = lvl.n > 0 ? lvl.n - 1 : 0;
      if (lvl.sz === 0) lvl.n = 0;
      this.tape.push({ pxC: ev.pxC, sz: ev.qty, side: ev.side === L2Side.Ask ? 1 : -1 });
      if (this.tape.length > W6_TAPE) this.tape.shift();
      this.lastTradePxC = ev.pxC;
    }
    this.msgSeq++;
    this.foldedThisTick++;
  }

  beginTick(): void {
    this.foldedThisTick = 0;
  }

  /// Export into a FRESH Float32Array (Mode A's per-tick snapshot
  /// allocation). Identical expressions to L2BookEngine.exportFrame.
  exportFrame(tick: number): Float32Array {
    const out = new Float32Array(W6_FLOAT_COUNT);
    out[0] = tick;
    out[1] = this.foldedThisTick;
    out[2] = this.midC / 100;
    out[3] = (this.bidOff + this.askOff) / 100;
    let bidDepth = 0;
    let askDepth = 0;
    for (let i = 0; i < W6_LEVELS; i++) {
      bidDepth += this.bid[i].sz;
      askDepth += this.ask[i].sz;
    }
    out[4] = bidDepth;
    out[5] = askDepth;
    out[6] = this.lastTradePxC / 100;
    const lad = W6_HEADER_FLOATS;
    for (let i = 0; i < W6_LEVELS; i++) {
      const o = lad + i * W6_LADDER_FIELDS;
      out[o] = (this.midC - this.bidOff - i) / 100;
      out[o + 1] = this.bid[i].sz;
      out[o + 2] = this.bid[i].n;
      out[o + 3] = (this.midC + this.askOff + i) / 100;
      out[o + 4] = this.ask[i].sz;
      out[o + 5] = this.ask[i].n;
    }
    const tape = lad + W6_LEVELS * W6_LADDER_FIELDS;
    for (let j = 0; j < W6_TAPE; j++) {
      const o = tape + j * W6_TAPE_FIELDS;
      const t = this.tape[j];
      if (t !== undefined) {
        out[o] = t.pxC / 100;
        out[o + 1] = t.sz;
        out[o + 2] = t.side;
      } else {
        out[o] = 0;
        out[o + 1] = 0;
        out[o + 2] = 0;
      }
    }
    out[7] = w6Checksum(out);
    return out;
  }
}
