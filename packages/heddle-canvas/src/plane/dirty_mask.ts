// dirty_mask.ts — @weft/heddle-canvas split-word dirty-mask protocol.
//
// WHY EXISTS: the Hot-Plane's per-lane 64-bit dirty mask is the entire
// invalidation signal for the 240 FPS engine (deliverable C): a clear mask
// skips BOTH the buffer re-upload and the draw call; a set mask narrows the
// upload to the mutated element range and clips rasterization to the dirty
// rectangle. It is also the place where naive implementations quietly
// destroy Law 1 — the obvious `Atomics.exchange(bigU64, i, 0n)` on
// BigUint64Array returns a BigInt, and V8 heap-allocates that BigInt on
// EVERY call. A "zero-allocation" loop that read-clears through BigInts is
// a lie the heap audit would catch and the static scanner cannot see.
//
// THE SPLIT-WORD PROTOCOL (RFC-0022 §3 — the load-bearing trick):
//   The 64-bit dirty word lives in the plane; both sides alias the SAME
//   bytes with two views:
//     producer: BigUint64Array — a single atomic 64-bit OR sets bit(s)
//     consumer: Int32Array     — TWO atomic 32-bit exchanges read-clear it
//   Consumer:
//     lo = Atomics.exchange(i32, dirtyLoI32, 0)   // captures+clears bits 0..31
//     hi = Atomics.exchange(i32, dirtyHiI32, 0)   // captures+clears bits 32..63
//   Both results are int32 SMIs — zero heap allocation, provably.
//
// NO-LOST-UPDATE ARGUMENT (why two 32-bit exchanges are safe):
//   Producers only ever SET bits (OR), never clear. A bit set before its
//   half's exchange is returned by that exchange (consumed this frame). A
//   bit set after its half's exchange leaves the word non-zero and is
//   consumed by the NEXT frame's exchange. There is no interleaving in
//   which a set bit is erased without being observed: clearing happens
//   only inside the two exchanges, each of which RETURNS everything it
//   cleared. The sole effect of the split is that a bit racing the frame
//   boundary may be drawn one frame late — the documented, bounded price
//   (this is exactly Pillar 3's "capability discovery costs a syscall,
//   never a frame" discipline applied to the UI plane).
//
// The CI battery (test/dirty_mask.test.ts) proves the protocol three ways:
// single-thread semantics, a Worker hammering 64-bit producer ORs while the
// main thread consumes (no lost bit after quiescence), and equivalence of
// the split-u32 producer road with the 64-bit producer road.

import type { LaneView } from './hot_plane.ts';

/**
 * Reusable dirty-frame record. ONE instance is carved at engine init and
 * passed into takeDirtyBits every frame (Law 1: the record is recycled, so
 * reading dirty state allocates nothing). Fields are overwritten in place.
 */
export interface DirtyBits {
  lo: number;
  hi: number;
}

/** Carve the recycled record (init-time only). */
export function createDirtyBits(): DirtyBits {
  return { lo: 0, hi: 0 };
}

/**
 * Read-clear a lane's 64-bit dirty mask into `out` (split-word protocol).
 * Returns true when ANY bit was set (the lane must be drawn this frame).
 * Hot path: two atomic exchanges, zero allocation, zero BigInt.
 */
export function takeDirtyBits(i32: Int32Array, lane: LaneView, out: DirtyBits): boolean {
  out.lo = Atomics.exchange(i32, lane.dirtyLoI32, 0) | 0;
  out.hi = Atomics.exchange(i32, lane.dirtyHiI32, 0) | 0;
  return out.lo !== 0 || out.hi !== 0;
}

/** Non-destructive peek (diagnostics/HUD paths only — not the frame loop). */
export function peekDirtyBits(i32: Int32Array, lane: LaneView, out: DirtyBits): boolean {
  out.lo = Atomics.load(i32, lane.dirtyLoI32) | 0;
  out.hi = Atomics.load(i32, lane.dirtyHiI32) | 0;
  return out.lo !== 0 || out.hi !== 0;
}

/**
 * The element range covered by the set bits in `out`, written into the
 * pre-allocated `range` record (again: recycled, never allocated per use).
 *   elemStart .. elemEndExclusive  (clamped to [0, capacity])
 * `bits` receives the popcount (how many of the 64 blocks mutated — the
 * dirty-rectangle coarseness metric the report quotes).
 */
export interface DirtyRange {
  elemStart: number;
  elemEndExclusive: number;
  bits: number;
}

export function createDirtyRange(): DirtyRange {
  return { elemStart: 0, elemEndExclusive: 0, bits: 0 };
}

/** ctz-free lowest-set-bit via de Bruijn (deterministic, allocation-free). */
const DEBRUIJN_CTZ = (() => {
  const t = new Int32Array(32);
  let b = 0x077cb531;
  for (let i = 0; i < 32; i++) {
    t[((b << i) >>> 27) as number] = i;
  }
  return t;
})();

function lowestBitIndex(lo: number, hi: number): number {
  if (lo !== 0) {
    const v = lo >>> 0;
    return DEBRUIJN_CTZ[(((v & -v) * 0x077cb531) >>> 27) as number];
  }
  const v = hi >>> 0;
  return 32 + DEBRUIJN_CTZ[(((v & -v) * 0x077cb531) >>> 27) as number];
}

function highestBitIndex(lo: number, hi: number): number {
  if (hi !== 0) {
    let v = hi >>> 0;
    let r = 32;
    while (v > 1) { v = v >>> 1; r++; }
    return r; // 32..63
  }
  let v = lo >>> 0;
  let r = -1;
  while (v > 0) { v = v >>> 1; r++; }
  return r; // 0..31
}

function popcount32(v: number): number {
  v = (v >>> 0) - (((v >>> 1) & 0x55555555) >>> 0);
  v = (v & 0x33333333) + (((v >>> 2) & 0x33333333) >>> 0);
  v = (v + (v >>> 4)) & 0x0f0f0f0f;
  return (v * 0x01010101) >>> 24;
}

/**
 * Compute [elemStart, elemEndExclusive) + popcount for the bits in `out`.
 * Hot path — all arithmetic on numbers (SMI/double, no allocation).
 */
export function dirtyRangeOf(
  bits: DirtyBits,
  lane: LaneView,
  range: DirtyRange,
): void {
  if (bits.lo === 0 && bits.hi === 0) {
    range.elemStart = 0;
    range.elemEndExclusive = 0;
    range.bits = 0;
    return;
  }
  const gran = lane.dirtyGranularity;
  const first = lowestBitIndex(bits.lo, bits.hi);
  const last = highestBitIndex(bits.lo, bits.hi);
  range.elemStart = first * gran;
  range.elemEndExclusive = Math.min((last + 1) * gran, lane.capacity);
  range.bits = popcount32(bits.lo) + popcount32(bits.hi);
}

// ---------------------------------------------------------------------------
// PRODUCER SIDE (reference roads — the real producer is Engineer 1's; these
// exist so tests, the bench and the browser rig can drive the plane exactly
// as a conforming producer would, through BOTH roads).
// ---------------------------------------------------------------------------

/**
 * Producer road A — the contract road: a single atomic 64-bit OR on the
 * BigUint64Array alias. This is the road Engineer 1's plane documents.
 * The dirty word is 8-byte aligned (descriptor base is 128-aligned, field
 * at +0x30), so its big64 index is exactly dirtyLoI32 / 2.
 */
export function markDirty64(
  big64: BigUint64Array,
  lane: LaneView,
  firstElem: number,
  count: number,
): void {
  const gran = lane.dirtyGranularity;
  const firstBit = Math.floor(firstElem / gran);
  const lastBit = Math.floor((firstElem + count - 1) / gran);
  let mask = 0n;
  for (let b = firstBit; b <= lastBit; b++) mask |= 1n << BigInt(b);
  Atomics.or(big64, lane.dirtyLoI32 >>> 1, mask);
}

/**
 * Producer road B — the split-u32 road: two 32-bit ORs on the Int32Array
 * alias. Bit-equivalent to road A for every mask a conforming producer
 * builds (proven by the CI battery); exists for runtimes without 64-bit
 * atomics and as the cross-check oracle.
 */
export function markDirty32(
  i32: Int32Array,
  lane: LaneView,
  firstElem: number,
  count: number,
): void {
  const gran = lane.dirtyGranularity;
  const firstBit = Math.floor(firstElem / gran);
  const lastBit = Math.floor((firstElem + count - 1) / gran);
  for (let b = firstBit; b <= lastBit; b++) {
    if (b < 32) Atomics.or(i32, lane.dirtyLoI32, 1 << b);
    else Atomics.or(i32, lane.dirtyHiI32, 1 << (b - 32));
  }
}

/** Producer commit: write_pos + sequence bump after marking dirty. */
export function producerCommit(
  i32: Int32Array,
  lane: LaneView,
  writePos: number,
): void {
  Atomics.store(i32, lane.writePosI32, writePos | 0);
  Atomics.add(i32, lane.seqI32, 1);
}
