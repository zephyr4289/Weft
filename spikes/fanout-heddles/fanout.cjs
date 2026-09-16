// spikes/fanout-heddles/fanout.js — CONCURRENT multi-consumer fan-out ring.
//
// WHY EXISTS: RFC 0004 specified a 1-writer/N-reader fan-out built from the
// same primitives as the Triad (atomic seqlock slots, pre-allocated payload,
// latest-wins) so N consumers can observe one stream WITHOUT touching the
// frozen 1-writer/1-reader kernel. The original prototype
// (fanout_prototype.ts) validated the SHAPE but was single-threaded: plain
// fields, no atomics, no torn-read possibility, so its 1.27M pub/s claim was
// never earned under real parallelism.
//
// This file is the real thing. SharedArrayBuffer + Atomics (all SeqCst —
// the TS port's documented memory-ordering regime), worker_threads in the
// companion test/bench files, and a differential-pattern integrity gate
// (04-LITMUS §0.6 family) on every claimed frame.
//
// ─── Memory layout (byte-precise, one SharedArrayBuffer) ──────────────────
//
//   Control block (128 B, cache-isolated from slots):
//     [0..3]   u32 latestSeq   0 = nothing published; else last completed frame seq
//     [4..7]   u32 done        harness flag: writer finished (test termination)
//     [8..11]  u32 pubCount    frames the writer completed (Atomics.add)
//     [12..15] u32 tornRetries writer-observed... (reserved; readers count locally)
//     rest: pad
//
//   Slot k (k = 0..S-1), stride = 128 B header + align64(4·payloadWords):
//     [0..3]   u32 latch       seqlock: EVEN = stable, ODD = writer writing.
//                              Equality-compared (never ordered) → u32 wrap-benign.
//     [4..7]   u32 frameSeq    frame seq this slot holds (valid when stable)
//     [128 ..] payload         payloadWords × i32
//
// ─── Writer protocol (frame F, slot s = (F-1) mod S) ──────────────────────
//   1. Atomics.store(latch, odd)          // exclude readers (SC store)
//   2. plain-store frameSeq = F; plain-write pattern words (readers excluded)
//   3. Atomics.store(latch, even)         // release: content complete
//   4. Atomics.store(latestSeq, F)        // THE publication point
//   Steps 3→4 are both SeqCst stores: any reader that observes latestSeq=F
//   (SC load) then observes latch ≥ step-3's even value (SC coherence) —
//   the slot can never appear complete-empty once published.
//
// ─── Reader protocol (bounded-retry, stale-tolerant — NEVER blocks) ───────
//   G = Atomics.load(latestSeq); if 0 → NOT_READY
//   s = (G-1) mod S
//   r1 = Atomics.load(latch); ODD → retry
//   F = frameSeq; copy payload words → caller's target (plain, excluded)
//   r2 = Atomics.load(latch); r1 ≠ r2 → retry (writer reused slot mid-copy)
//   stable ⇒ the copy is an internally-consistent frame F, and F ≥ G is
//   impossible to be < G: slot s holds only frames ≡ s+1 (mod S), strictly
//   increasing, and latestSeq had already reached G. So the snapshot is
//   frame G or a NEWER reuse (G+S, G+2S…) — both are legitimate latest-wins
//   deliveries; integrity verification keys off F, never off G.
//   Freshness: serial-number arithmetic on u32 (after(F, lastSeen));
//   framesBehind = (F − lastSeenPrev − 1) >>> 0 — exact per-reader drop
//   accounting, same semantics as FrameCursor (RFC 0008), u32-wrap safe
//   for gaps < 2^31.
//   Retry bound: 8 attempts, then RETRY_EXHAUSTED (caller keeps its previous
//   frame; nothing is ever blocked — the reader-facing Law 4 boundary).
//
// ─── Law 2 (zero steady-state allocation) ─────────────────────────────────
//   publish() and claimInto() touch only pre-allocated views + numbers.
//   claimInto writes its report into a CALLER-OWNED out array — the hot path
//   allocates nothing (returning an object literal would allocate).
'use strict';

const CACHE_LINE = 64;
const CONTROL_BYTES = 128;
const SLOT_HEADER_BYTES = 128;

const C_LATEST_SEQ = 0;
const C_DONE = 4;
const C_PUB_COUNT = 8;

const H_LATCH = 0;
const H_FRAMESEQ = 4;

// claimInto() status codes (written to out[0])
const ST_NOT_READY = 0;
const ST_FRESH = 1;
const ST_STALE = 2;      // stable frame but not newer than reader's lastSeen
const ST_EXHAUSTED = 3;  // bounded retries hit; keep previous frame (stale-tolerant)

// out[] indices
const OUT_STATUS = 0;
const OUT_SEQ = 1;
const OUT_BEHIND = 2;
const OUT_FIRST = 3;

function align64(n) { return Math.ceil(n / CACHE_LINE) * CACHE_LINE; }

function mix32(x) {
  x = x ^ (x >>> 16);
  x = Math.imul(x, 0x7FEB352D);
  x = x ^ (x >>> 15);
  x = Math.imul(x, 0x846CA68B);
  x = x ^ (x >>> 16);
  return x >>> 0;
}

// Fanout differential pattern — full-width sibling of the 04-LITMUS §0.6
// fixture pat(): same multiply constants, full 32-bit residue so every
// payload word carries 32 bits of integrity instead of 8.
function patWord(seq, i) {
  const x = (Math.imul(seq >>> 0, 2654435761) + Math.imul(i >>> 0, 2246822519)) >>> 0;
  return mix32(x) | 0;
}

// u32 serial-number comparison: is `a` strictly after `b`? (wrap-safe for gaps < 2^31)
function after(a, b) {
  const d = (a - b) >>> 0;
  return d > 0 && d < 0x80000000;
}

class FanoutRing {
  // Build a ring VIEW over `sab` (SharedArrayBuffer). Construct one view per
  // thread; the writer's view keeps its private latch bookkeeping, readers
  // never touch it.
  constructor(sab, slotCount, payloadWords, byteOffset) {
    this.slotCount = slotCount;
    this.payloadWords = payloadWords;
    this.byteOffset = byteOffset || 0;
    this.slotStride = SLOT_HEADER_BYTES + align64(payloadWords * 4);
    const need = this.byteOffset + CONTROL_BYTES + slotCount * this.slotStride;
    if (sab.byteLength < need) {
      throw new Error(`FanoutRing: SAB too small (${sab.byteLength} < ${need} bytes)`);
    }
    this.ctrl = new Int32Array(sab, this.byteOffset, CONTROL_BYTES / 4);
    this.slots = [];
    for (let k = 0; k < slotCount; k++) {
      const base = this.byteOffset + CONTROL_BYTES + k * this.slotStride;
      this.slots.push({
        latch: new Int32Array(sab, base + H_LATCH, 1),
        frameSeq: new Int32Array(sab, base + H_FRAMESEQ, 1),
        payload: new Int32Array(sab, base + SLOT_HEADER_BYTES, payloadWords),
      });
    }
    // Writer-private bookkeeping: last latch value stored per slot (parity
    // source). NOT shared — readers derive stability from loads only.
    this._wLatch = new Int32Array(slotCount);
  }

  static bytesFor(slotCount, payloadWords) {
    return CONTROL_BYTES + slotCount * (SLOT_HEADER_BYTES + align64(payloadWords * 4));
  }

  // ── Writer hot path ────────────────────────────────────────────────────
  // Frame F (1..): fill slot (F-1)%S with the differential pattern, then
  // publish. No allocation, no branch heavier than the latch protocol.
  publish(F) {
    const s = this.slots[(F - 1) % this.slotCount];
    const v = this._wLatch[(F - 1) % this.slotCount];
    Atomics.store(s.latch, 0, v + 1);              // ODD — writers exclude readers
    s.frameSeq[0] = F;
    const p = s.payload;
    for (let i = 0; i < this.payloadWords; i++) {
      p[i] = patWord(F, i);
    }
    this._wLatch[(F - 1) % this.slotCount] = v + 2;
    Atomics.store(s.latch, 0, v + 2);              // EVEN — content complete
    Atomics.store(this.ctrl, C_LATEST_SEQ >> 2, F); // publication point
    Atomics.add(this.ctrl, C_PUB_COUNT >> 2, 1);
  }

  // Variant used by the daisy-chain broadcaster: publish frame F whose
  // payload is copied from a caller-owned byte view (e.g. the kernel's
  // rLive() Uint8Array). Reads `payloadWords` little-endian u32 words.
  // Same latch protocol and publication point as publish(); no allocation.
  publishBytes(F, bytes) {
    const k = (F - 1) % this.slotCount;
    const s = this.slots[k];
    const v = this._wLatch[k];
    Atomics.store(s.latch, 0, v + 1);              // ODD
    s.frameSeq[0] = F;
    const p = s.payload;
    for (let i = 0; i < this.payloadWords; i++) {
      const o = i * 4;
      p[i] = (bytes[o] | (bytes[o + 1] << 8) | (bytes[o + 2] << 16) | (bytes[o + 3] << 24)) | 0;
    }
    this._wLatch[k] = v + 2;
    Atomics.store(s.latch, 0, v + 2);              // EVEN
    Atomics.store(this.ctrl, C_LATEST_SEQ >> 2, F);
    Atomics.add(this.ctrl, C_PUB_COUNT >> 2, 1);
  }

  // ── Reader hot path ────────────────────────────────────────────────────
  // Copies the freshest stable frame into `target` (Int32Array, caller-owned,
  // ≥ payloadWords), writes the report into `out` (Int32Array(4)), returns
  // void — zero allocation. The reader's lastSeen/first state lives in
  // `state` (Int32Array(3): [lastSeen, hasClaimed, tornRetries]) so the whole
  // claim path stays allocation-free across calls.
  claimInto(target, state, out) {
    out[OUT_BEHIND] = 0;
    for (let attempt = 0; attempt < 8; attempt++) {
      const G = Atomics.load(this.ctrl, C_LATEST_SEQ >> 2);
      if (G === 0) { out[OUT_STATUS] = ST_NOT_READY; out[OUT_SEQ] = 0; out[OUT_FIRST] = state[1] === 0 ? 1 : 0; return; }
      const s = this.slots[(G - 1) % this.slotCount];
      const r1 = Atomics.load(s.latch, 0);
      if ((r1 & 1) === 1) continue;                // writer mid-write
      const F = s.frameSeq[0];
      const p = s.payload;
      const n = this.payloadWords;
      for (let i = 0; i < n; i++) target[i] = p[i]; // plain copy — reader excluded by latch
      const r2 = Atomics.load(s.latch, 0);
      if (r1 !== r2) { state[2]++; continue; }      // torn: slot reused mid-copy
      // Stable snapshot of frame F (≥ G — see header proof). Freshness:
      if (state[1] === 0) {
        // First claim: baseline (FrameCursor semantics — nothing "behind").
        state[0] = F; state[1] = 1;
        out[OUT_STATUS] = ST_FRESH; out[OUT_SEQ] = F; out[OUT_BEHIND] = 0; out[OUT_FIRST] = 1;
        return;
      }
      const last = state[0] >>> 0;
      if (after(F >>> 0, last)) {
        out[OUT_BEHIND] = ((F >>> 0) - last - 1) >>> 0;
        state[0] = F;
        out[OUT_STATUS] = ST_FRESH; out[OUT_SEQ] = F; out[OUT_FIRST] = 0;
        return;
      }
      out[OUT_STATUS] = ST_STALE; out[OUT_SEQ] = F; out[OUT_FIRST] = 0;
      return;
    }
    state[2]++;
    out[OUT_STATUS] = ST_EXHAUSTED; out[OUT_SEQ] = 0; out[OUT_FIRST] = 0;
  }

  // Integrity check for a claimed frame (test mode): every word must match
  // patWord(F, i). Writes 0/1 into out[0]-style verdict to avoid alloc.
  static verify(target, F, payloadWords) {
    for (let i = 0; i < payloadWords; i++) {
      if (target[i] !== patWord(F, i)) return false;
    }
    return true;
  }

  latestSeq() { return Atomics.load(this.ctrl, C_LATEST_SEQ >> 2) >>> 0; }
  pubCount() { return Atomics.load(this.ctrl, C_PUB_COUNT >> 2) >>> 0; }
  setDone(v) { Atomics.store(this.ctrl, C_DONE >> 2, v); }
  isDone() { return Atomics.load(this.ctrl, C_DONE >> 2) === 1; }
}

module.exports = {
  FanoutRing, patWord, mix32, after,
  ST_NOT_READY, ST_FRESH, ST_STALE, ST_EXHAUSTED,
  OUT_STATUS, OUT_SEQ, OUT_BEHIND, OUT_FIRST,
  C_LATEST_SEQ, C_DONE, C_PUB_COUNT,
  CONTROL_BYTES, SLOT_HEADER_BYTES, CACHE_LINE,
};
