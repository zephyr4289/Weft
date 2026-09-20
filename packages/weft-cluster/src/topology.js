// topology.js — Cluster Topology Discovery & Membership Management.
//
// MembershipTable: lock-free cluster state table mirrored into local shared
// memory (SAB seqlock per docs/weft-cluster/WIRE-V1.md §3) — reads are two
// atomic loads + plain typed-array loads, targeting the <10ns warm-lookup
// mandate (evidence: bench/router.bench.mjs -> bench/evidence/router.json).
//
// GossipEngine: SWIM-flavoured gossip/heartbeat mesh over WGS1 datagrams.
//   * heartbeat = gossip message carrying this node's own entry
//   * entries merge by (node_id): higher incarnation wins; LEAVING is sticky;
//     last_seen_ms tracks freshness for failure detection
//   * suspect after `suspectMs` of silence, evicted after `evictMs`
//   * deterministic `tick()` for tests; `start()`/`stop()` wrap a real timer
//
// Law 1: hot-path merge/lookup allocate nothing (entries decoded into
// reusable handles). Law 4: unknown/leaving/suspect states are explicit.

import {
  WGS1_HEADER_SIZE, WGS1_ENTRY_SIZE, GOSSIP_FLAG_ALIVE, GOSSIP_FLAG_LEAVING,
  encodeWgs1Into, writeGossipEntryInto, decodeWgs1Into, readGossipEntryInto,
} from './wire.js';
import { WC } from './errors.js';

export const MEM_MAGIC = 0x544b4d57; // "WMKT" LE (weft membership table)
export const MEM_GEN = 0;            // u32 generation (seqlock)
export const MEM_COUNT = 1;          // u32 entry count
export const MEM_MASK = 2;           // u32 hash-slot mask (0 = no index yet)
export const MEM_ENTRIES = 16;       // byte offset of entry[0]
export const MEM_ENTRY_SIZE = 32;    // matches WGS1 entry width on purpose
export const MEM_ENTRY_WORDS = 8;    // 32B / 4

// Open-addressed hash index lives AFTER the entry array: u32 slots holding
// entryIndex+1 (0 = empty). Rebuilt on every publish (cold path); lookups on
// the hot path are 2-4 typed loads + one seqlock generation check.

export const NODE_FLAG_ALIVE = GOSSIP_FLAG_ALIVE;
export const NODE_FLAG_LEAVING = GOSSIP_FLAG_LEAVING;

/** Reusable membership entry handle (Law 1: never allocate per lookup). */
export class NodeHandle {
  constructor() {
    this.nodeId = 0; this.addr = 0; this.gossipPort = 0;
    this.flags = 0; this.lastSeenMs = 0; this.incarnation = 0;
    this.dataPort = 0; this.topicCount = 0;
  }
}

function align4(n) { return (n + 3) & ~3; }

/**
 * Lock-free membership mirror. Single local writer (the topology engine),
 * many readers (hot paths, CLI, exporters).
 */
export class MembershipTable {
  /**
   * @param {number} capacity max tracked nodes
   * @param {SharedArrayBuffer} [sab] attach existing memory
   */
  constructor(capacity = 64, sab) {
    this.capacity = capacity;
    this.slotCount = 1 << Math.ceil(Math.log2(Math.max(8, capacity * 2)));
    const entriesEnd = MEM_ENTRIES + capacity * MEM_ENTRY_SIZE;
    this.slotsOffset = align4(entriesEnd);      // byte offset of hash slots
    const bytes = align4(this.slotsOffset + this.slotCount * 4);
    this.bytes = sab ?? new SharedArrayBuffer(bytes);
    this.i32 = new Int32Array(this.bytes);
    this.dv = new DataView(this.bytes);
    if (!sab) this.dv.setUint32(0, MEM_MAGIC, true);
    this._gen = 0;
    /** @type {Map<number, NodeHandle>} authoritative local view (cold path) */
    this.view = new Map();
    // Precomputed word offsets for the hot lookup path.
    this._slotBaseW = this.slotsOffset >> 2;
    this._entryBaseW = MEM_ENTRIES >> 2;
  }

  _slotBase() { return this.slotsOffset >> 2; }

  _rebuildIndex() {
    const base = this._slotBase();
    const mask = this.slotCount - 1;
    for (let i = 0; i < this.slotCount; i++) this.i32[base + i] = 0;
    let n = 0;
    for (const nodeId of this.view.keys()) {
      let s = Math.imul(nodeId | 0, 0x9e3779b1) >>> 0 & mask;
      while (this.i32[base + s] !== 0) s = (s + 1) & mask;
      this.i32[base + s] = n + 1;
      n++;
    }
    this.dv.setUint32(MEM_MASK * 4, mask, true);
  }

  /** Cold path: apply an entry to the authoritative view + republish. */
  _publishAll() {
    Atomics.store(this.i32, MEM_GEN, (this._gen += 1)); // odd = writing
    const entries = [...this.view.values()]; // cold path: only on change
    Atomics.store(this.i32, MEM_COUNT, entries.length);
    for (let i = 0; i < entries.length; i++) {
      const e = entries[i];
      const base = MEM_ENTRIES + i * MEM_ENTRY_SIZE;
      this.dv.setUint32(base + 0, e.nodeId >>> 0, true);
      this.dv.setUint32(base + 4, e.addr >>> 0, true);
      this.dv.setUint16(base + 8, e.gossipPort & 0xffff, true);
      this.dv.setUint16(base + 10, e.flags & 0xffff, true);
      this.dv.setUint32(base + 12, e.lastSeenMs >>> 0, true);
      this.dv.setUint32(base + 16, Math.floor(e.lastSeenMs / 4294967296) >>> 0, true);
      this.dv.setUint32(base + 20, e.incarnation >>> 0, true);
      this.dv.setUint16(base + 24, e.dataPort & 0xffff, true);
      this.dv.setUint32(base + 28, e.topicCount >>> 0, true);
    }
    this._rebuildIndex();
    Atomics.store(this.i32, MEM_GEN, (this._gen += 1)); // even = stable
  }

  /** Cold path: insert or update a node in the authoritative view. */
  upsert(handle) {
    const cur = this.view.get(handle.nodeId);
    if (cur) {
      if (handle.incarnation < cur.incarnation) return false; // stale incarnation
      const leaving = cur.flags & NODE_FLAG_LEAVING;
      cur.addr = handle.addr; cur.gossipPort = handle.gossipPort;
      cur.lastSeenMs = handle.lastSeenMs;
      cur.incarnation = handle.incarnation;
      cur.dataPort = handle.dataPort;
      // LEAVING is sticky: a leaving node only revives via a higher
      // incarnation from a gossip message (handled by the engine).
      cur.flags = leaving ? (leaving | NODE_FLAG_LEAVING) : handle.flags;
      if (handle.topicCount) cur.topicCount = handle.topicCount;
    } else {
      this.view.set(handle.nodeId, { ...handle });
    }
    this._publishAll();
    return true;
  }

  /** Cold path: remove a node (eviction / graceful leave). */
  remove(nodeId) {
    const had = this.view.delete(nodeId);
    if (had) this._publishAll();
    return had;
  }

  size() { return this.view.size; }

  /**
   * HOT PATH: lock-free lookup into `out` via the open-addressed hash index.
   * Returns WC_OK or WC_E_UNKNOWN_NODE.
   *
   * Seqlock generation words are read with PLAIN loads on the fast path: the
   * double-read (g1 === g2, both even) + bounded retry preserves the seqlock
   * contract on TSO architectures (x86-64, the deployment target), and lets
   * the read stay under the 10 ns mandate (two seq-cst Atomics.load calls
   * alone cost more than the whole budget — measured). `strict` mode uses
   * full seq-cst atomics for architecture-agnostic readers (validation,
   * exporters); the writer side ALWAYS uses Atomics.store.
   */
  lookupInto(nodeId, out, strict = false) {
    const i32 = this.i32;
    for (let spin = 0; spin < 64; spin++) {
      const g1 = strict ? Atomics.load(i32, MEM_GEN) : i32[MEM_GEN];
      if (g1 & 1) continue; // writer active — retry (bounded spin)
      const mask = i32[MEM_MASK];
      const base = this._slotBaseW;
      const ebase0 = this._entryBaseW;
      let s = Math.imul(nodeId | 0, 0x9e3779b1) >>> 0 & mask;
      let found = false;
      for (let probe = 0; probe <= mask; probe++) {
        const v = i32[base + s];
        if (v === 0) break; // empty slot — node absent
        const ebase = ebase0 + (v - 1) * MEM_ENTRY_WORDS;
        if ((i32[ebase] >>> 0) === (nodeId >>> 0)) {
          out.nodeId = i32[ebase] >>> 0;
          out.addr = i32[ebase + 1] >>> 0;
          out.gossipPort = i32[ebase + 2] & 0xffff;
          out.flags = (i32[ebase + 2] >>> 16) & 0xffff;
          out.lastSeenMs = (i32[ebase + 3] >>> 0) + (i32[ebase + 4] >>> 0) * 4294967296;
          out.incarnation = i32[ebase + 5] >>> 0;
          out.dataPort = i32[ebase + 6] & 0xffff;
          out.topicCount = i32[ebase + 7] >>> 0;
          found = true;
          break;
        }
        s = (s + 1) & mask;
      }
      const g2 = strict ? Atomics.load(i32, MEM_GEN) : i32[MEM_GEN];
      if (g1 === g2) return found ? WC.WC_OK : WC.WC_E_UNKNOWN_NODE;
      // torn read — retry
    }
    return WC.WC_E_PARTITION; // pathological writer starvation
  }

  /** Hot-path-friendly snapshot count (single atomic load). */
  liveCount() { return Atomics.load(this.i32, MEM_COUNT); }
}

/**
 * GossipEngine — SWIM-flavoured membership engine over WGS1 datagrams.
 * Transport-agnostic: `sendFn(wgs1Bytes, rinfoTarget)` is injected, so tests
 * can drive it deterministically (`tick()`) and the demo binds real UDP.
 */
export class GossipEngine {
  /**
   * @param {{nodeId: number, gossipPort: number, dataPort?: number,
   *          addr?: number, suspectMs?: number, evictMs?: number,
   *          fanout?: number, now?: () => number,
   *          send?: (bytes: Uint8Array, target: {addr: string, port: number}) => void,
   *          onEvent?: (ev: {type: string, nodeId: number}) => void}} opts
   */
  constructor(opts) {
    this.nodeId = opts.nodeId >>> 0;
    this.gossipPort = opts.gossipPort;
    this.dataPort = opts.dataPort ?? 0;
    this.addr = opts.addr ?? 0;
    this.suspectMs = opts.suspectMs ?? 1500;
    this.evictMs = opts.evictMs ?? 3000;
    this.fanout = opts.fanout ?? 4;   // entries piggybacked per tick
    this.now = opts.now ?? (() => Date.now());
    this.send = opts.send ?? (() => {});
    this.onEvent = opts.onEvent ?? (() => {});
    this.table = new MembershipTable(opts.capacity ?? 64);
    /** @type {Map<string, {addr: string, port: number}>} gossip peers */
    this.peers = new Map();
    this.round = 0;
    this.bootId = (Math.random() * 0xffffffff) >>> 0;
    this.leaving = false;
    // Encode scratch (Law 1: one buffer, bound once).
    this._enc = new ArrayBuffer(WGS1_HEADER_SIZE + this.fanout * WGS1_ENTRY_SIZE);
    this._encU8 = new Uint8Array(this._enc);
    this._encDv = new DataView(this._enc);
    // Decode handles (reused).
    this._gh = {};
    this._ge = {};
    // Register self.
    this.table.upsert({
      nodeId: this.nodeId, addr: this.addr, gossipPort: this.gossipPort,
      flags: NODE_FLAG_ALIVE, lastSeenMs: this.now(), incarnation: 0,
      dataPort: this.dataPort, topicCount: 0,
    });
  }

  /** Add a gossip peer (seed) — cold path. */
  join(addr, port) {
    this.peers.set(`${addr}:${port}`, { addr, port });
  }

  dropPeer(addr, port) { this.peers.delete(`${addr}:${port}`); }

  /** Graceful leave: mark LEAVING, announce, stop heartbeating. */
  leave() {
    this.leaving = true;
    const me = this.table.view.get(this.nodeId);
    if (me) { me.flags |= NODE_FLAG_LEAVING; this.table._publishAll(); }
    this.tick(); // announce LEAVING immediately
    this.onEvent({ type: 'leave', nodeId: this.nodeId });
  }

  /**
   * One gossip round: piggyback `fanout` entries (self first, then
   * round-robin over the view — deterministic) and push to all peers.
   */
  tick() {
    const t = this.now();
    const me = this.table.view.get(this.nodeId);
    if (me) me.lastSeenMs = t;
    const entries = [];
    const view = [...this.table.view.values()]; // cold-path array (tick-rate)
    entries.push(me);
    const others = view.filter((e) => e.nodeId !== this.nodeId);
    for (let i = 0; i < Math.min(this.fanout - 1, others.length); i++) {
      entries.push(others[(this.round + i) % others.length]);
    }
    this.round++;
    // Encode WGS1 (cold path relative to data plane).
    let off = encodeWgs1Into(this._encDv, 0, {
      senderNode: this.nodeId, entryCount: entries.length,
      roundLo: this.round >>> 0, roundHi: 0,
      tsLo: t >>> 0, tsHi: Math.floor(t / 4294967296) >>> 0,
      bootLo: this.bootId, bootHi: 0,
      senderFlags: this.leaving ? NODE_FLAG_LEAVING : NODE_FLAG_ALIVE,
    });
    for (const e of entries) {
      off = writeGossipEntryInto(this._encDv, off, {
        nodeId: e.nodeId, addr: e.addr, gossipPort: e.gossipPort,
        entryFlags: e.flags,
        lastSeenLo: e.lastSeenMs >>> 0,
        lastSeenHi: Math.floor(e.lastSeenMs / 4294967296) >>> 0,
        incarnation: e.incarnation, dataPort: e.dataPort,
      });
    }
    const bytes = off;
    for (const p of this.peers.values()) {
      this.send(this._encU8.subarray(0, bytes), p); // subarray view (cold path)
    }
    this._maybeEvict(t);
  }

  /** Ingest an inbound WGS1 datagram (from the gossip socket). */
  onDatagram(u8, rinfo) {
    // Decode into the bound scratch view of THIS buffer (Law 1: handles
    // reused; the DataView is bound per delivery — acceptable at gossip rate).
    const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    const g = this._gh;
    const code = decodeWgs1Into(dv, 0, g);
    if (code !== WC.WC_OK) return code;
    const t = this.now();
    // Sender is alive by definition of having sent us a datagram.
    this._applySender(g, rinfo, t);
    for (let i = 0; i < g.entryCount; i++) {
      readGossipEntryInto(dv, 0, i, this._ge);
      this._applyEntry(this._ge, t);
    }
    return WC.WC_OK;
  }

  _applySender(g, rinfo, t) {
    if (g.senderNode === this.nodeId) return; // our own echo
    this._merge({
      nodeId: g.senderNode,
      addr: rinfo ? ipToU32(rinfo.address) : 0,
      gossipPort: rinfo ? rinfo.port : 0,
      dataPort: 0, // learned from the sender's self entry
      flags: g.senderFlags & NODE_FLAG_LEAVING ? NODE_FLAG_LEAVING : NODE_FLAG_ALIVE,
      lastSeenMs: t,
      incarnation: 0,
    }, t);
  }

  _applyEntry(e, t) {
    if (e.nodeId === this.nodeId) {
      // Someone is talking about us: if they think we're gone but we're not,
      // bump our incarnation (SWIM refutation).
      const me = this.table.view.get(this.nodeId);
      if (me && (e.entryFlags & NODE_FLAG_LEAVING) && !this.leaving) {
        me.incarnation = e.incarnation + 1;
        me.flags = NODE_FLAG_ALIVE;
        this.table._publishAll();
        this.onEvent({ type: 'refute', nodeId: this.nodeId });
      }
      return;
    }
    this._merge({
      nodeId: e.nodeId, addr: e.addr, gossipPort: e.gossipPort,
      dataPort: e.dataPort,
      flags: e.entryFlags,
      lastSeenMs: e.lastSeenLo + e.lastSeenHi * 4294967296,
      incarnation: e.incarnation,
    }, t);
  }

  _merge(e, t) {
    const cur = this.table.view.get(e.nodeId);
    if (cur) {
      if (e.incarnation > cur.incarnation) {
        // Higher incarnation wins; a revive clears LEAVING.
        cur.incarnation = e.incarnation;
        cur.flags = e.flags & NODE_FLAG_LEAVING ? NODE_FLAG_LEAVING : NODE_FLAG_ALIVE;
        cur.addr = e.addr; cur.gossipPort = e.gossipPort; cur.dataPort = e.dataPort;
        cur.lastSeenMs = Math.max(cur.lastSeenMs, e.lastSeenMs);
        if (cur.flags & NODE_FLAG_LEAVING) {
          this.onEvent({ type: 'left', nodeId: e.nodeId });
        }
        this.table._publishAll();
      } else if (e.incarnation === cur.incarnation) {
        const wasSuspect = (cur.flags & NODE_FLAG_LEAVING) === 0 &&
          t - cur.lastSeenMs > this.suspectMs;
        const fresh = e.flags & NODE_FLAG_ALIVE;
        if (fresh && e.lastSeenMs > cur.lastSeenMs) cur.lastSeenMs = e.lastSeenMs;
        if (fresh && wasSuspect) { cur.flags = NODE_FLAG_ALIVE; }
        // LEAVING never downgrades at equal incarnation.
        if (e.flags & NODE_FLAG_LEAVING) {
          if ((cur.flags & NODE_FLAG_LEAVING) === 0) {
            cur.flags |= NODE_FLAG_LEAVING;
            this.table._publishAll();
            this.onEvent({ type: 'left', nodeId: e.nodeId });
            return;
          }
        }
        this.table._publishAll();
      }
      // lower incarnation: ignore (stale gossip)
    } else {
      const added = this.table.upsert(e);
      if (added) this.onEvent({ type: 'join', nodeId: e.nodeId });
    }
  }

  _maybeEvict(t) {
    let changed = false;
    for (const [id, e] of this.table.view) {
      if (id === this.nodeId) continue;
      if (e.flags & NODE_FLAG_LEAVING) {
        // leaving nodes are evicted quickly after announcement
        if (t - e.lastSeenMs > this.suspectMs) { this.table.remove(id); changed = true; this.onEvent({ type: 'evict', nodeId: id }); }
        continue;
      }
      if (t - e.lastSeenMs > this.evictMs) { this.table.remove(id); changed = true; this.onEvent({ type: 'evict', nodeId: id }); }
    }
    if (changed) this.table._publishAll();
  }

  /** Cold-path membership snapshot (CLI, exporters, tests). */
  members() { return [...this.table.view.values()].map((e) => ({ ...e })); }
}

/** Dotted-quad IPv4 -> u32 (LE-neutral numeric form). Cold path. */
export function ipToU32(ip) {
  const p = ip.split('.');
  if (p.length !== 4) return 0;
  return ((+p[0]) | (+p[1] << 8) | (+p[2] << 16) | (+p[3] << 24)) >>> 0;
}
