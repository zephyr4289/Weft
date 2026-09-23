// transport/shm.js — LoopbackShmTransport: same-host simulated cluster mesh.
//
// Models WCR1 one-sided RDMA semantics honestly: the owner node WRITES the
// WCN1 datagram directly into each subscriber's registered memory (a per-
// (topic,subscriber) ring of preallocated slots) and commits it with one
// atomic store. The consumer never copies — it decodes the datagram in place.
//
// Slot layout (per ring, single writer):
//   [128B ring header]
//   slot i = [u64 frame_seq commit word (hi @+4, lo @+0 — lo written LAST)]
//            [WCN1 datagram: header + inline payload]
//
// Ring header:
//   0  4  magic "WCRG"     4  2  version=1      6  2  header_size=128
//   8  4  slot_count       12 4  slot_stride     16 4  payload_max
//   64 4  publish_cursor u32 (lo)   68 4  publish_cursor u32 (hi)
//
// Same-host only (SharedArrayBuffer cannot cross processes — use
// transport/udp.js for cross-process nodes). Law 3: primitives only.
// Law 1: publishSlot/tryRead allocate NOTHING — all views are bound once.

import { decodeWcn1Into } from '../wire.js';

export const RING_MAGIC = 0x47524357; // "WCRG" LE
export const RING_HEADER = 128;
export const COMMIT_OFF = 0;
export const DATA_OFF = 16;

function align8(n) { return (n + 7) & ~7; }

/** A single-writer, single-cursor datagram ring living in a SharedArrayBuffer. */
export class ShmRing {
  /**
   * @param {number} slotCount slots per ring (writer laps slow readers)
   * @param {number} payloadMax max inline payload bytes per datagram
   * @param {SharedArrayBuffer} [sab] attach to existing memory
   */
  constructor(slotCount = 256, payloadMax = 1024, sab) {
    const stride = align8(DATA_OFF + 64 + payloadMax);
    this.slotCount = slotCount >>> 0;
    this.payloadMax = payloadMax >>> 0;
    this.stride = stride;
    this.bytes = sab ?? new SharedArrayBuffer(RING_HEADER + this.slotCount * stride);
    this.i32 = new Int32Array(this.bytes);
    this.dv = new DataView(this.bytes);
    if (!sab) {
      this.dv.setUint32(0, RING_MAGIC, true);
      this.dv.setUint16(4, 1, true);
      this.dv.setUint16(6, RING_HEADER, true);
      this.dv.setUint32(8, this.slotCount, true);
      this.dv.setUint32(12, stride, true);
      this.dv.setUint32(16, this.payloadMax, true);
    }
    /** 1-based next frame number this ring will write (single writer). */
    this._nextWrite = 0;
  }

  /** Publish cursor: total frames ever committed (u32 lo — wraps at 2^32). */
  get cursor() { return Atomics.load(this.i32, 16); }

  /**
   * Publish one pre-encoded datagram into slot for frame `n = cursor + 1`.
   * CRITICAL PATH (Law 1): pure typed-array copies + two atomic stores.
   * @param {number} n 1-based frame number (caller tracks per-ring cursor)
   * @param {number} frameHi hi u32 of the frame seq (commit hi)
   * @param {Int32Array} srcI32 u32 view over the source datagram memory
   * @param {number} srcIdx u32-word index of the datagram inside srcI32
   * @param {number} words u32 words to copy (ceil(bytes/4); source zero-padded)
   */
  publishSlot(n, frameHi, srcI32, srcIdx, words) {
    const slotBase = RING_HEADER + ((n - 1) % this.slotCount) * this.stride;
    const d = this.i32;
    const off = (slotBase + DATA_OFF) >> 2;
    for (let i = 0; i < words; i++) d[off + i] = srcI32[srcIdx + i];
    // Commit: hi first, lo (== n) LAST with release-strength seq-cst store.
    Atomics.store(d, (slotBase + COMMIT_OFF + 4) >> 2, frameHi | 0);
    Atomics.store(d, (slotBase + COMMIT_OFF) >> 2, n | 0);
    Atomics.store(d, 16, n | 0); // ring cursor
    return n;
  }

  /**
   * Try to read frame `n` (1-based) in place into a consumer handle bound to
   * this ring (h.bind(ring.bytes, 0) ONCE). Returns:
   *   0  WC_OK      — decoded; h.offset points at the datagram
   *   1  torn       — slot mid-write / lapped; caller re-polls
   *   -1            — not published yet
   *   else          — stable WC_* decode error code
   */
  tryRead(n, h) {
    const cur = Atomics.load(this.i32, 16);
    if (n > cur) return -1;
    const slotBase = RING_HEADER + ((n - 1) % this.slotCount) * this.stride;
    const lo = Atomics.load(this.i32, (slotBase + COMMIT_OFF) >> 2);
    if (lo !== (n >>> 0)) return 1;
    h.offset = slotBase + DATA_OFF;
    return decodeWcn1Into(h, h.offset);
  }
}

/**
 * In-process fabric: topic → subscriber rings. The publisher (owner node)
 * writes into every registered subscriber ring — the simulated equivalent of
 * a WCR1 one-sided put to registered peer memory.
 */
export class ShmFabric {
  constructor() {
    /** @type {Map<string, ShmRing[]>} topic -> rings */
    this.topics = new Map();
    /** @type {Map<string, Set<object>>} topic -> subscriptions to poke */
    this.pokes = new Map();
  }

  addPoke(topic, sub) {
    let s = this.pokes.get(topic);
    if (!s) { s = new Set(); this.pokes.set(topic, s); }
    s.add(sub);
  }

  dropPoke(topic, sub) {
    const s = this.pokes.get(topic);
    if (s) s.delete(sub);
  }

  /** Wake every local subscription of `topic` after a fanout. */
  poke(topic) {
    const s = this.pokes.get(topic);
    if (!s) return;
    for (const sub of s) sub.poke();
  }

  addSubscriber(topic, ring) {
    let rings = this.topics.get(topic);
    if (!rings) { rings = []; this.topics.set(topic, rings); }
    rings.push(ring);
    return ring;
  }

  dropSubscriber(topic, ring) {
    const rings = this.topics.get(topic);
    if (!rings) return false;
    const i = rings.indexOf(ring);
    if (i >= 0) { rings.splice(i, 1); return true; }
    return false;
  }

  subscriberCount(topic) { return this.topics.get(topic)?.length ?? 0; }
}

/**
 * LoopbackShmTransport — wires a ClusterClient into a ShmFabric.
 * `publish` fans the staging datagram into every subscriber ring of the topic.
 */
export class LoopbackShmTransport {
  /** @param {ShmFabric} fabric */
  constructor(fabric) {
    this.fabric = fabric;
    this.kind = 'loopback-shm';
  }

  async start() {}
  async stop() {}

  /**
   * Create + register a subscriber ring for (topic, subscriberId).
   * @returns {ShmRing}
   */
  addSubscriberRing(topic, subscriberId, opts = {}) {
    const ring = new ShmRing(opts.slotCount ?? 256, opts.payloadMax ?? 1024);
    ring._subId = subscriberId;
    this.fabric.addSubscriber(topic, ring);
    return ring;
  }

  dropSubscriberRing(topic, subscriberId) {
    const rings = this.fabric.topics.get(topic);
    if (!rings) return false;
    for (let i = 0; i < rings.length; i++) {
      if (rings[i]._subId === subscriberId) { rings.splice(i, 1); return true; }
    }
    return false;
  }

  /**
   * Publish one datagram to all subscribers of `topic`.
   * @param {string} topic
   * @param {Int32Array} srcI32 u32 view over the client's staging datagram
   * @param {number} srcIdx u32-word index of the datagram inside srcI32
   * @param {number} words u32 words (bytes >> 2, staging zero-padded)
   * @param {number} frameHi
   * @returns {number} rings written
   */
  publish(topic, srcI32, srcIdx, words, frameHi) {
    const rings = this.fabric.topics.get(topic);
    if (!rings) return 0;
    for (let i = 0; i < rings.length; i++) {
      const r = rings[i];
      const n = r._nextWrite + 1;
      r._nextWrite = n;
      r.publishSlot(n, frameHi, srcI32, srcIdx, words);
    }
    this.fabric.poke(topic); // wake local iterators (same-process fabric)
    return rings.length;
  }
}
