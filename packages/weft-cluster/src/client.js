// client.js — ClusterClient: managed publish/subscribe over a pluggable
// transport (loopback-shm today, UDP for cross-process, WCR1 RDMA later).
//
// Law 1 (zero hot-path allocation): publishers encode into ONE staging
// datagram bound at construction; subscribers decode in place through ONE
// FrameHandle bound to their ring at subscribe() time. The per-iteration
// JavaScript async-iterator result objects are the runtime's own machinery —
// everything this module controls is reused.
//
// Law 2: all header writes go through wire.js explicit-LE primitives.
// Law 4: stale sequences, torn slots and CRC failures are counted events
// (staleCount/gapCount/errorCount), never silent.

import {
  FrameHandle, encodeWcn1Into, fnv1a64, WC_FLAG_INLINE_PAYLOAD,
  WC_FLAG_CRC_PRESENT, WCN1_HEADER_SIZE,
} from './wire.js';
import { crc32Range } from './crc32.js';
import { WC } from './errors.js';
import { ShmRing } from './transport/shm.js';
import { CTL_SUB, CTL_UNSUB } from './transport/udp.js';

// Module-level scratch for the hrtime split (consumed synchronously — safe
// under the single-threaded event loop; avoids a per-frame object).
const NOW = { lo: 0, hi: 0 };
function nowSplit() {
  const t = process.hrtime.bigint();
  NOW.lo = Number(t & 0xffffffffn) >>> 0;
  NOW.hi = Number((t >> 32n) & 0xffffffffn) >>> 0;
}

/** Per-topic publisher state (one per registered topic, cold path). */
class TopicState {
  constructor(name, lo, hi) {
    this.name = name; this.lo = lo; this.hi = hi;
    this.seqLo = 0; this.seqHi = 0;
  }
}

/**
 * A live subscription. Async-iterable; the yielded FrameHandle is REUSED —
 * consume it before advancing (copy out anything retained). This is the
 * zero-copy contract: the handle's payload region IS the ring memory.
 */
export class Subscription {
  /** @param {ClusterClient} client */
  constructor(client, topic, t, ring) {
    this.client = client;
    this.topic = topic;
    this.topicLo = t.lo; this.topicHi = t.hi;
    this.ring = ring;
    this.handle = new FrameHandle().bind(ring.bytes, 0);
    this.nextRead = 1;
    this.lastSeqLo = 0; this.lastSeqHi = 0;
    this.delivered = 0;
    this.staleCount = 0;   // Law 4: replayed/out-of-order datagrams dropped
    this.gapCount = 0;     // Law 4: lapped slots (slow consumer) skipped
    this.errorCount = 0;   // Law 4: decode/CRC failures dropped
    this.running = true;
    this._notify = null;
    this._notifyTimer = null;
  }

  /** Wake a waiting iterator (called after in-process delivery). */
  poke() {
    if (this._notify) {
      const r = this._notify; this._notify = null;
      if (this._notifyTimer) { clearTimeout(this._notifyTimer); this._notifyTimer = null; }
      r();
    }
  }

  _wait() {
    return new Promise((res) => {
      this._notify = res;
      // Event-loop friendly fallback: never stall longer than 1 ms even if
      // the producer lives on another thread and cannot poke us.
      this._notifyTimer = setTimeout(res, 1);
    });
  }

  /** Stop the iterator and release fabric resources. */
  stop() {
    this.running = false;
    this.poke();
    this.client._dropSubscription(this);
  }

  /** Read the newest available frame into `h` without advancing the stream.
   *  Returns WC_OK, or -1 when the ring is empty, or a stable WC_* code. */
  tryLatest(h) {
    const cur = Atomics.load(this.ring.i32, 16);
    if (cur === 0) return -1;
    const code = this.ring.tryRead(cur, h);
    if (code !== WC.WC_OK) return code;
    return h.topicLo === this.topicLo && h.topicHi === this.topicHi
      ? WC.WC_OK : 1;
  }

  async *_iterate() {
    const ring = this.ring;
    const h = this.handle;
    let tornSpins = 0;
    while (this.running) {
      const code = ring.tryRead(this.nextRead, h);
      if (code === WC.WC_OK) {
        tornSpins = 0;
        // NOTE: no seq-equality stale check here — the ring's commit words
        // already guarantee frame-number order, and the datagram-path replay
        // filter lives in ClusterClient._onTransportFrame (lastSeq*). A check
        // against lastSeq here would flag EVERY frame stale on that path,
        // because the transport updates lastSeq before we read the slot.
        this.nextRead++;
        this.delivered++;
        yield h;
        continue;
      }
      if (code === -1) { await this._wait(); continue; }
      if (code === 1) {
        // Torn OR lapped. If the writer is a full ring ahead of us, the slot
        // for nextRead was overwritten — jump forward and count the gap.
        const cur = Atomics.load(ring.i32, 16);
        if (cur - this.nextRead >= ring.slotCount) {
          const lost = cur - ring.slotCount + 2 - this.nextRead;
          if (lost > 0) this.gapCount += lost;
          this.nextRead = cur - ring.slotCount + 2;
        } else if (++tornSpins > 64) {
          await this._wait(); tornSpins = 0;
        }
        continue;
      }
      // Stable decode error on a committed slot — drop and advance (counted).
      this.errorCount++;
      this.nextRead++;
    }
  }

  [Symbol.asyncIterator]() { return this._iterate(); }
}

/**
 * ClusterClient — high-level cluster endpoint.
 *
 * ```js
 * const client = new ClusterClient({ nodeId: 1, transport });
 * await client.start();
 * for await (const frame of client.subscribe('telemetry')) {
 *   // frame.payloadOffset .. +frame.payloadLen — zero-copy view
 * }
 * ```
 */
export class ClusterClient {
  /**
   * @param {{nodeId: number, transport: any, schemaId?: number,
   *          payloadMax?: number, ringSlots?: number, crc?: boolean}} opts
   */
  constructor(opts) {
    this.nodeId = opts.nodeId >>> 0;
    this.transport = opts.transport;
    this.schemaId = opts.schemaId ?? 1;
    this.payloadMax = opts.payloadMax ?? 1024;
    this.ringSlots = opts.ringSlots ?? 256;
    this.crcEnabled = opts.crc ?? true;
    // One staging datagram for the whole client, bound once (Law 1).
    this._staging = new ArrayBuffer((WCN1_HEADER_SIZE + this.payloadMax + 3) & ~3);
    this._stagingDv = new DataView(this._staging);
    this._stagingU8 = new Uint8Array(this._staging);
    this._stagingI32 = new Int32Array(this._staging);
    this._fields = {
      flags: 0, srcNode: this.nodeId, topicLo: 0, topicHi: 0,
      seqLo: 0, seqHi: 0, tsLo: 0, tsHi: 0,
      payloadLen: 0, schemaId: this.schemaId, crc: 0, rdmaKey: 0,
    };
    /** @type {Map<string, TopicState>} */
    this.topics = new Map();
    /** @type {Map<number, Map<number, Subscription[]>>} lo -> hi -> subs */
    this._subsByHash = new Map();
    /** @type {Set<Subscription>} */
    this.subs = new Set();
    this._nextSubId = 1;
    this.published = 0;
    this.txBytes = 0;
  }

  async start() { await this.transport.start(); }
  async stop() { await this.transport.stop(); }

  /** Preallocated Buffer VIEW over the staging datagram (datagram modes).
   *  Created lazily on first use — Buffer.from(ArrayBuffer) is a view, never
   *  a copy. Only called on Node/Deno/Bun (dgram is unavailable in browsers). */
  _txBuf() {
    if (this._tx === undefined) this._tx = Buffer.from(this._staging);
    return this._tx;
  }

  /** Cold path: register a topic (FNV-1a 64 hash, cached u32 pair). */
  registerTopic(topic) {
    let t = this.topics.get(topic);
    if (!t) {
      const { lo, hi } = fnvCache(topic);
      t = new TopicState(topic, lo, hi);
      this.topics.set(topic, t);
    }
    return t;
  }

  /**
   * Publish one frame on `topic`. Hot path — zero allocation in this module.
   *
   * Two payload forms:
   *   publish(topic, payloadU8)               — copies bytes into staging
   *   publish(topic, len, fill(stagingU8))    — fill writes directly at the
   *                                             payload offset (64), zero
   *                                             user-side copy
   * @returns {{seqLo: number, seqHi: number}} the frame's sequence
   */
  publish(topic, payloadOrLen, fill) {
    const t = this.topics.get(topic) ?? this.registerTopic(topic);
    // seq++ with u32 carry
    let lo = (t.seqLo + 1) >>> 0;
    let hi = t.seqHi;
    if (lo === 0) hi = (hi + 1) >>> 0;
    t.seqLo = lo; t.seqHi = hi;
    nowSplit();
    const f = this._fields;
    f.topicLo = t.lo; f.topicHi = t.hi;
    f.seqLo = lo; f.seqHi = hi;
    f.tsLo = NOW.lo; f.tsHi = NOW.hi;
    let payloadLen;
    if (typeof payloadOrLen === 'number') {
      payloadLen = payloadOrLen;
      if (fill) fill(this._stagingU8, WCN1_HEADER_SIZE);
    } else {
      payloadLen = payloadOrLen.length;
      const src = payloadOrLen;
      const dst = this._stagingU8;
      for (let i = 0; i < payloadLen; i++) dst[WCN1_HEADER_SIZE + i] = src[i];
    }
    f.payloadLen = payloadLen;
    f.flags = WC_FLAG_INLINE_PAYLOAD | (this.crcEnabled ? WC_FLAG_CRC_PRESENT : 0);
    f.crc = this.crcEnabled
      ? crc32Range(this._stagingU8, WCN1_HEADER_SIZE, WCN1_HEADER_SIZE + payloadLen)
      : 0;
    encodeWcn1Into(this._stagingDv, 0, f);
    const bytes = WCN1_HEADER_SIZE + payloadLen;
    const words = (bytes + 3) >> 2;
    this.transport.publish(t.name, this._stagingI32, 0, words, hi,
                           this._txBuf(), bytes, t.lo, t.hi);
    this.published++;
    this.txBytes += bytes;
    // Wake local shm subscribers (same-process fabric delivers via rings).
    this._pokeSubscribers(t.lo, t.hi);
    return seqPair(lo, hi);
  }

  /**
   * Subscribe to a topic. Returns an async-iterable Subscription.
   * @param {string} topic
   * @param {{slotCount?: number, payloadMax?: number}} [opts]
   */
  subscribe(topic, opts = {}) {
    const t = this.topics.get(topic) ?? this.registerTopic(topic);
    const subId = this._nextSubId++;
    let ring;
    if (this.transport.kind === 'loopback-shm') {
      ring = this.transport.addSubscriberRing(
        topic, subId, { slotCount: opts.slotCount ?? this.ringSlots,
                        payloadMax: opts.payloadMax ?? this.payloadMax });
    } else {
      ring = new ShmRing(opts.slotCount ?? this.ringSlots,
                         opts.payloadMax ?? this.payloadMax);
      // Tell connected peers we want this topic (datagram transports).
      if (typeof this.transport.control === 'function') {
        for (const key of this.transport.peers) {
          this.transport.control(key, CTL_SUB, t.lo, t.hi, this.nodeId,
                                 this._stagingDv, this._txBuf());
        }
      }
    }
    const sub = new Subscription(this, topic, t, ring);
    this.subs.add(sub);
    if (this.transport.kind === 'loopback-shm') {
      this.transport.fabric.addPoke(topic, sub);
    }
    let byHi = this._subsByHash.get(t.lo);
    if (!byHi) { byHi = new Map(); this._subsByHash.set(t.lo, byHi); }
    let arr = byHi.get(t.hi);
    if (!arr) { arr = []; byHi.set(t.hi, arr); }
    arr.push(sub);
    sub._subId = subId;
    return sub;
  }

  /** Register interest of a connected peer for an EXISTING subscription (udp). */
  resubscribePeers() {
    if (typeof this.transport.control !== 'function') return;
    for (const sub of this.subs) {
      for (const key of this.transport.peers) {
        this.transport.control(key, CTL_SUB, sub.topicLo, sub.topicHi,
                               this.nodeId, this._stagingDv, this._txBuf());
      }
    }
  }

  /**
   * Called by datagram transports for every received frame.
   * @param {FrameHandle} h transport-owned handle (rx ring bound)
   * @param {number} base byte offset of the datagram inside the rx buffer
   * @param {number} bytes datagram length
   * @param {Int32Array} srcI32 u32 view over the rx buffer
   */
  _onTransportFrame(h, base, bytes, srcI32) {
    // CONTROL frames were consumed by the transport itself — never here.
    let byHi = this._subsByHash.get(h.topicLo);
    if (!byHi) return;
    const subs = byHi.get(h.topicHi);
    if (!subs) return;
    const words = (bytes + 3) >> 2;
    const srcIdx = base >> 2;
    for (let i = 0; i < subs.length; i++) {
      const sub = subs[i];
      // Law 4: per-subscription staleness — drop replays, accept new epochs.
      if ((h.seqHi < sub.lastSeqHi) ||
          (h.seqHi === sub.lastSeqHi &&
           h.seqLo <= sub.lastSeqLo &&
           (sub.lastSeqLo - h.seqLo) < 0x80000000)) {
        sub.staleCount++;
        continue;
      }
      sub.lastSeqLo = h.seqLo; sub.lastSeqHi = h.seqHi;
      const n = sub.ring._nextWrite + 1;
      sub.ring._nextWrite = n;
      sub.ring.publishSlot(n, h.seqHi, srcI32, srcIdx, words);
      sub.delivered++;
      sub.poke();
    }
  }

  _pokeSubscribers(topicLo, topicHi) {
    const byHi = this._subsByHash.get(topicLo);
    if (!byHi) return;
    const subs = byHi.get(topicHi);
    if (!subs) return;
    for (let i = 0; i < subs.length; i++) subs[i].poke();
  }

  _dropSubscription(sub) {
    this.subs.delete(sub);
    if (this.transport.kind === 'loopback-shm') {
      this.transport.fabric.dropPoke(sub.topic, sub);
    }
    const byHi = this._subsByHash.get(sub.topicLo);
    if (!byHi) return;
    const arr = byHi.get(sub.topicHi);
    if (!arr) return;
    const i = arr.indexOf(sub);
    if (i >= 0) arr.splice(i, 1);
    if (this.transport.kind === 'loopback-shm') {
      this.transport.dropSubscriberRing(sub.topic, sub._subId);
    } else if (typeof this.transport.control === 'function') {
      for (const key of this.transport.peers) {
        this.transport.control(key, CTL_UNSUB, sub.topicLo, sub.topicHi,
                               this.nodeId, this._stagingDv, this._txBuf());
      }
    }
  }
}

// --- cold-path helpers (never touched by the hot loop) ----------------------

const _fnvCache = new Map();
function fnvCache(topic) {
  let v = _fnvCache.get(topic);
  if (!v) { v = fnv1a64(topic); _fnvCache.set(topic, v); }
  return v;
}
const _pair = { seqLo: 0, seqHi: 0 };
function seqPair(lo, hi) { _pair.seqLo = lo; _pair.seqHi = hi; return _pair; }