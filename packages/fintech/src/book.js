// book.js — Level-2/3 aggregated order book with ring-allocated memory pools.
//
// Design contract (docs/adapters/MANAGED-SEAMS-V1.md + Pillar 6 Law 2):
//   - Steady state (message application) performs ZERO heap allocation.
//     All storage is SoA typed arrays allocated once in the constructor.
//   - O(1) order insertion / replacement / cancellation:
//       order identity  -> open-addressed hash over lo/hi u32 pairs
//       order storage   -> preallocated slot pool with LIFO free ring
//       price levels    -> direct-mapped tick ladder (O(1) level lookup)
//   - u64 order refs NEVER materialize as JS doubles (2^53 hazard): the pool
//     and hash operate on lo/hi u32 pairs end to end.
//   - All failures are counted reject codes, never exceptions (Law 4
//     taxonomy W4-04: transparent degradation, fail-closed counters).
//   - No floats on the parity path: prices/sizes are u32, timestamps u48
//     reconstructed via hi*2**32 + lo (exact below 2^53).
//
// Replace semantics (public ITCH 5.0): 'U' removes the original order and
// appends the new order at the BACK of its level (time priority resets).

import { T_SYSTEM, T_ADD, T_ADD_MPID, T_EXEC, T_EXEC_PRICE, T_CANCEL, T_DELETE, T_REPLACE, T_TRADE } from './itch.js';

export const OK = 0;
export const E_DUP_REF = 1;      // add/replace target ref already live
export const E_UNKNOWN_REF = 2;  // execute/cancel/delete/replace origin unknown
export const E_POOL_FULL = 3;    // order slot pool exhausted
export const E_PRICE_RANGE = 4;  // price tick outside configured ladder band
export const E_BAD_SIZE = 5;     // shares larger than resting size (clamped)
export const E_REJECT_CODES = 6;

const SIDE_BUY = 0;
const SIDE_SELL = 1;

function pow2ceil(n) {
  let c = 1;
  while (c < n) c <<= 1;
  return c;
}

// Open-addressed u64-keyed (lo/hi u32 pairs) -> slot index map.
// Capacity is fixed at construction (2x pool, power of two) so the load
// factor never exceeds 0.5 and NO REHASH ever happens (rehash would spike).
class RefHash {
  constructor(capacity) {
    this.mask = capacity - 1;
    this.keyLo = new Int32Array(capacity);
    this.keyHi = new Int32Array(capacity);
    this.val = new Int32Array(capacity).fill(-1); // -1 empty, -2 tombstone
    this.count = 0;
  }
  // Math.imul = exact u32 multiply semantics; Python mirrors with
  // ((lo * 0x9E3779B1) & 0xFFFFFFFF) ^ ((hi * 0x85EBCA77) & 0xFFFFFFFF).
  // Keys are normalized to int32 (|0) so unsigned wire values compare
  // equal to their Int32Array storage (0xdeadbeef stores as -559038241).
  hash(lo, hi) {
    return (Math.imul(lo | 0, 0x9e3779b1) ^ Math.imul(hi | 0, 0x85ebca77)) & this.mask;
  }
  find(lo, hi) {
    lo |= 0; hi |= 0;
    let i = this.hash(lo, hi);
    for (;;) {
      const v = this.val[i];
      if (v === -1) return -1;              // never-used slot ends probe
      if (v >= 0 && this.keyLo[i] === lo && this.keyHi[i] === hi) return v;
      i = (i + 1) & this.mask;
    }
  }
  insert(lo, hi, slot) {
    lo |= 0; hi |= 0;
    let i = this.hash(lo, hi);
    let tomb = -1;
    for (;;) {
      const v = this.val[i];
      if (v === -1) {
        const t = tomb >= 0 ? tomb : i;
        this.keyLo[t] = lo; this.keyHi[t] = hi; this.val[t] = slot;
        this.count++;
        return;
      }
      if (v === -2) { if (tomb < 0) tomb = i; }
      else if (this.keyLo[i] === lo && this.keyHi[i] === hi) {
        this.val[i] = slot; // refresh (defensive; callers pre-check dups)
        return;
      }
      i = (i + 1) & this.mask;
    }
  }
  remove(lo, hi) {
    lo |= 0; hi |= 0;
    let i = this.hash(lo, hi);
    for (;;) {
      const v = this.val[i];
      if (v === -1) return false;
      if (v >= 0 && this.keyLo[i] === lo && this.keyHi[i] === hi) {
        this.val[i] = -2; // tombstone; key bytes stay (probe-chain safety)
        this.count--;
        return true;
      }
      i = (i + 1) & this.mask;
    }
  }
}

// Direct-mapped tick ladder. Level lookup by arithmetic (no hashing at all).
class Ladder {
  constructor(baseTick, tickCount) {
    this.baseTick = baseTick;
    this.tickCount = tickCount;
    this.aggSize = new Uint32Array(tickCount);
    this.orderCount = new Uint32Array(tickCount);
    this.head = new Int32Array(tickCount).fill(-1);
    this.tail = new Int32Array(tickCount).fill(-1);
  }
}

export class OrderBook {
  constructor(opts = {}) {
    const poolCapacity = opts.poolCapacity ?? 8192;
    const baseTick = opts.baseTick ?? 900_000;
    const tickCount = opts.tickCount ?? 262_144;
    this.topLevels = opts.topLevels ?? 10;
    this.poolCapacity = poolCapacity;
    this.baseTick = baseTick;
    this.tickCount = tickCount;

    // --- order slot pool (SoA) ---
    this.refLo = new Uint32Array(poolCapacity);
    this.refHi = new Uint32Array(poolCapacity);
    this.price = new Uint32Array(poolCapacity);
    this.size = new Uint32Array(poolCapacity);
    this.side = new Uint8Array(poolCapacity);
    this.locate = new Uint16Array(poolCapacity);
    this.prev = new Int32Array(poolCapacity);
    this.next = new Int32Array(poolCapacity);
    this.lvlIdx = new Int32Array(poolCapacity); // ladder index per slot
    // LIFO free ring: slots allocated/freed in O(1); deterministic order
    this.free = new Int32Array(poolCapacity);
    for (let i = 0; i < poolCapacity; i++) this.free[i] = poolCapacity - 1 - i;
    this.freeTop = poolCapacity;
    this.liveOrders = 0;

    // --- ref -> slot hash (never rehashes) ---
    this.hash = new RefHash(pow2ceil(poolCapacity * 2));

    // --- price ladders ---
    this.buy = new Ladder(baseTick, tickCount);
    this.sell = new Ladder(baseTick, tickCount);
    this.bestBidTick = -1;
    this.bestAskTick = -1;

    // --- counters (all deterministic, parity-visible) ---
    this.msgsApplied = 0;
    this.skipped = 0;        // unknown message types
    this.lastTs = 0;
    this.lastMatch = 0;
    this.lastExecPrice = 0;
    this.tradeCount = 0;     // E / C / P match events
    this.adds = 0;
    this.executes = 0;
    this.cancels = 0;
    this.deletes = 0;
    this.replaces = 0;
    this.rejects = new Uint32Array(E_REJECT_CODES);

    // scratch for snapshot walk (reused; never reallocated)
    this._snapIdx = new Int32Array(this.topLevels);
  }

  // -- level bookkeeping ----------------------------------------------------
  #levelAppend(ladder, idx, slot) {
    const oldTail = ladder.tail[idx];
    if (oldTail >= 0) {
      this.next[oldTail] = slot;
      this.prev[slot] = oldTail;
    } else {
      ladder.head[idx] = slot;
      this.prev[slot] = -1;
    }
    this.next[slot] = -1;
    ladder.tail[idx] = slot;
    ladder.aggSize[idx] += this.size[slot];
    ladder.orderCount[idx] += 1;
  }

  #levelRemove(ladder, idx, slot) {
    ladder.aggSize[idx] -= this.size[slot];
    ladder.orderCount[idx] -= 1;
    const p = this.prev[slot], n = this.next[slot];
    if (p >= 0) this.next[p] = n; else ladder.head[idx] = n;
    if (n >= 0) this.prev[n] = p; else ladder.tail[idx] = p;
  }

  #bestAfterRemoval(side, tick) {
    if (side === SIDE_BUY) {
      if (tick !== this.bestBidTick) return;
      let t = tick;
      while (t >= this.baseTick && this.buy.orderCount[t - this.baseTick] === 0) t--;
      this.bestBidTick = t >= this.baseTick ? t : -1;
    } else {
      if (tick !== this.bestAskTick) return;
      let t = tick;
      const top = this.baseTick + this.tickCount;
      while (t < top && this.sell.orderCount[t - this.baseTick] === 0) t++;
      this.bestAskTick = t < top ? t : -1;
    }
  }

  #slotIdxForTick(tick) {
    const idx = tick - this.baseTick;
    if (idx < 0 || idx >= this.tickCount) return -1;
    return idx;
  }

  // -- operations (all O(1)) -------------------------------------------------
  #applyAdd(refHi, refLo, sideByte, shares, price, locate, ts) {
    // ITCH wire side is ASCII 0x42 'B' / 0x53 'S' — normalize to 0/1 once,
    // here, so every downstream ladder check sees SIDE_BUY(0)/SIDE_SELL(1).
    const side = sideByte === 0x42 ? SIDE_BUY : SIDE_SELL;
    if (this.hash.find(refLo, refHi) >= 0) {
      this.rejects[E_DUP_REF]++; return;
    }
    const idx = this.#slotIdxForTick(price);
    if (idx < 0) { this.rejects[E_PRICE_RANGE]++; return; }
    if (this.freeTop === 0) { this.rejects[E_POOL_FULL]++; return; }
    const slot = this.free[--this.freeTop];
    this.refLo[slot] = refLo; this.refHi[slot] = refHi;
    this.price[slot] = price; this.size[slot] = shares;
    this.side[slot] = side; this.locate[slot] = locate;
    const ladder = side === SIDE_BUY ? this.buy : this.sell;
    this.lvlIdx[slot] = idx;
    this.#levelAppend(ladder, idx, slot);
    if (side === SIDE_BUY) { if (price > this.bestBidTick) this.bestBidTick = price; }
    else if (this.bestAskTick < 0 || price < this.bestAskTick) this.bestAskTick = price;
    this.hash.insert(refLo, refHi, slot);
    this.liveOrders++; this.adds++;
    if (ts > 0) this.lastTs = ts;
  }

  #reduce(slot, shares, isCancel, match, price) {
    // shared by execute / cancel; caller resolved the slot already
    if (shares > this.size[slot]) { this.rejects[E_BAD_SIZE]++; shares = this.size[slot]; }
    const ladder = this.side[slot] === SIDE_BUY ? this.buy : this.sell;
    const idx = this.lvlIdx[slot];
    this.size[slot] -= shares;
    ladder.aggSize[idx] -= shares;
    let removed = false;
    if (this.size[slot] === 0) {
      this.#levelRemove(ladder, idx, slot);
      const tick = this.price[slot];
      this.hash.remove(this.refLo[slot], this.refHi[slot]);
      this.free[this.freeTop++] = slot;
      this.liveOrders--;
      removed = true;
      this.#bestAfterRemoval(this.side[slot], tick);
    }
    if (!isCancel) {
      this.tradeCount++;
      if (match > 0) this.lastMatch = match;
      if (price > 0) this.lastExecPrice = price;
    }
    return removed;
  }

  #applyExecute(refHi, refLo, shares, match, price) {
    const slot = this.hash.find(refLo, refHi);
    if (slot < 0) { this.rejects[E_UNKNOWN_REF]++; return; }
    this.#reduce(slot, shares, false, match, price);
    this.executes++;
  }

  #applyCancel(refHi, refLo, shares) {
    const slot = this.hash.find(refLo, refHi);
    if (slot < 0) { this.rejects[E_UNKNOWN_REF]++; return; }
    this.#reduce(slot, shares, true, 0, 0);
    this.cancels++;
  }

  #applyDelete(refHi, refLo) {
    const slot = this.hash.find(refLo, refHi);
    if (slot < 0) { this.rejects[E_UNKNOWN_REF]++; return; }
    const ladder = this.side[slot] === SIDE_BUY ? this.buy : this.sell;
    const idx = this.lvlIdx[slot];
    const tick = this.price[slot];
    this.#levelRemove(ladder, idx, slot);
    this.hash.remove(this.refLo[slot], this.refHi[slot]);
    this.free[this.freeTop++] = slot;
    this.liveOrders--;
    this.deletes++;
    this.#bestAfterRemoval(this.side[slot], tick);
  }

  #applyReplace(oHi, oLo, nHi, nLo, shares, price, ts) {
    const orig = this.hash.find(oLo, oHi);
    if (orig < 0) { this.rejects[E_UNKNOWN_REF]++; return; }
    if (this.hash.find(nLo, nHi) >= 0) { this.rejects[E_DUP_REF]++; return; }
    const idx = this.#slotIdxForTick(price);
    if (idx < 0) { this.rejects[E_PRICE_RANGE]++; return; }
    const side = this.side[orig];
    const locate = this.locate[orig];
    // remove original first (frees exactly one slot -> alloc below succeeds)
    const ladder = side === SIDE_BUY ? this.buy : this.sell;
    const oIdx = this.lvlIdx[orig];
    const oTick = this.price[orig];
    this.#levelRemove(ladder, oIdx, orig);
    this.hash.remove(oLo, oHi);
    this.free[this.freeTop++] = orig;
    this.liveOrders--;
    this.#bestAfterRemoval(side, oTick);
    // insert replacement at BACK of its level (priority resets — ITCH 'U')
    if (this.freeTop === 0) { this.rejects[E_POOL_FULL]++; return; }
    const slot = this.free[--this.freeTop];
    this.refLo[slot] = nLo; this.refHi[slot] = nHi;
    this.price[slot] = price; this.size[slot] = shares;
    this.side[slot] = side; this.locate[slot] = locate;
    this.lvlIdx[slot] = idx;
    this.#levelAppend(ladder, idx, slot);
    if (side === SIDE_BUY) { if (price > this.bestBidTick) this.bestBidTick = price; }
    else if (this.bestAskTick < 0 || price < this.bestAskTick) this.bestAskTick = price;
    this.hash.insert(nLo, nHi, slot);
    this.liveOrders++;
    this.replaces++;
    if (ts > 0) this.lastTs = ts;
  }

  // -- dispatch (called by ItchEngine with a bound ItchView) -----------------
  applyView(view) {
    switch (view.type) {
      case T_SYSTEM: // 'S': state advance, never touches resting orders
        this.msgsApplied++;
        if (view.ts > this.lastTs) this.lastTs = view.ts;
        return true;
      case T_ADD: // 'A'
      case T_ADD_MPID: // 'F' (MPID attribution ignored)
        this.#applyAdd(view.refHi, view.refLo, view.side, view.shares, view.price, view.locate, view.ts);
        break;
      case T_EXEC: // 'E'
        this.#applyExecute(view.refHi, view.refLo, view.shares, view.match, 0);
        break;
      case T_EXEC_PRICE: // 'C' executed with price
        this.#applyExecute(view.refHi, view.refLo, view.shares, view.match, view.price);
        break;
      case T_CANCEL: // 'X'
        this.#applyCancel(view.refHi, view.refLo, view.shares);
        break;
      case T_DELETE: // 'D'
        this.#applyDelete(view.refHi, view.refLo);
        break;
      case T_REPLACE: // 'U'
        this.#applyReplace(view.refHi, view.refLo, view.ref2Hi, view.ref2Lo, view.shares, view.price, view.ts);
        break;
      case T_TRADE: // 'P' non-cross trade: reported, never rests on the book
        this.tradeCount++;
        this.lastMatch = view.match;
        this.lastExecPrice = view.price;
        this.msgsApplied++;
        return true;
      default:
        this.skipped++;
        return false; // unknown type — skipped by the engine, never fatal
    }
    this.msgsApplied++;
    return true;
  }

  // -- introspection ---------------------------------------------------------
  bestBid() { return this.bestBidTick < 0 ? 0 : this.bestBidTick; }
  bestAsk() { return this.bestAskTick < 0 ? 0 : this.bestAskTick; }
  totalRejects() {
    let s = 0;
    for (let i = 0; i < this.rejects.length; i++) s += this.rejects[i];
    return s;
  }

  // Walk the ladder collecting the best `out.length` level indices.
  // Returns count written. Cold path (snapshot cadence), O(window scan).
  topLevelsOf(side, out) {
    const ladder = side === SIDE_BUY ? this.buy : this.sell;
    let t = side === SIDE_BUY ? this.bestBidTick : this.bestAskTick;
    const step = side === SIDE_BUY ? -1 : 1;
    const base = this.baseTick;
    const count = ladder.orderCount;
    let n = 0;
    while (t >= base && t < base + this.tickCount && n < out.length) {
      if (count[t - base] > 0) { out[n++] = t; }
      t += step;
    }
    return n;
  }
}
