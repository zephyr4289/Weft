// router.js — ShardRouter: rendezvous (highest-random-weight) consistent
// hashing for topic -> node routing, per docs/weft-cluster/WIRE-V1.md §4.
//
// score(node, topic) = fnv1a64( u32LE(node_seed) || u64LE(topic_hash) )
// Owner = highest score; ties broken by higher node_id. Deterministic across
// languages; adding/removing a node only moves that node's share.
//
// Law 1: the 64-bit FNV step uses exact double arithmetic (32x32 products
// stay under 2^53) — no BigInt, no objects on the hot path. Results are
// computed into a reusable `out` handle. Steady-state cost per ownerOf call
// is O(nodes) multiplies; callers cache per-topic owners keyed on
// `router.version` (bumped on membership changes) so the publish hot path
// pays ZERO after its first resolve.

const P_LO = 0x1b3;        // FNV prime lo u32 (0x01000001B3)
const P_HI = 0x100;        // FNV prime hi u32

import { WC } from './errors.js';

// Reusable scratch (single-threaded event loop; consumed synchronously).
const S = { hi: 0, lo: 0 };

function fnvReset() { S.hi = 0xcbf29ce4 >>> 0; S.lo = 0x84222325 >>> 0; }

function fnvByte(b) {
  S.lo = (S.lo ^ b) >>> 0;
  const prodLo = S.lo * P_LO;                     // <= (2^32-1)*435 < 2^53 exact
  const newLo = prodLo >>> 0;                     // ToUint32 == mod 2^32, exact
  const carry = (prodLo - newLo) / 4294967296;    // exact (power of two)
  const prodHi = S.lo * P_HI + S.hi * P_LO + carry; // < 2^42 exact
  S.lo = newLo;
  S.hi = prodHi >>> 0;
}

/** Compare score in S against best (bHi, bLo). Returns true if S wins. */
function scoreWins(bHi, bLo) {
  if (S.hi !== bHi) return S.hi > bHi;
  return S.lo > bLo;
}

/**
 * Rendezvous shard router.
 * ```js
 * const router = new ShardRouter();
 * router.addNode(1); router.addNode(2);
 * const out = {};                       // reused handle
 * router.ownerInto(fnv1a64('telemetry'), out);  // -> out.nodeId
 * ```
 */
export class ShardRouter {
  constructor() {
    /** @type {number[]} node ids */
    this.nodes = [];
    /** bumped on membership change — hot paths memo against this */
    this.version = 0;
    this._out = { nodeId: 0, scoreHi: 0, scoreLo: 0 };
  }

  /** Cold path: add a routing node. */
  addNode(nodeId) {
    nodeId = nodeId >>> 0;
    if (!this.nodes.includes(nodeId)) {
      this.nodes.push(nodeId);
      this.nodes.sort((a, b) => a - b); // deterministic iteration order
      this.version++;
    }
    return this;
  }

  /** Cold path: remove a routing node. */
  removeNode(nodeId) {
    const i = this.nodes.indexOf(nodeId >>> 0);
    if (i >= 0) { this.nodes.splice(i, 1); this.version++; }
    return this;
  }

  /** Cold path: replace the node set from a MembershipTable snapshot. */
  syncFromTable(table) {
    const ids = [...table.view.keys()].sort((a, b) => a - b);
    let changed = ids.length !== this.nodes.length;
    if (!changed) {
      for (let i = 0; i < ids.length; i++) {
        if (ids[i] !== this.nodes[i]) { changed = true; break; }
      }
    }
    if (changed) { this.nodes = ids; this.version++; }
    return this;
  }

  nodeCount() { return this.nodes.length; }

  /**
   * Resolve the owner of a topic into `out` (reused). Zero allocation.
   * Returns WC_OK, or WC_E_TOPIC_UNROUTED when no nodes are registered.
   * @param {{lo: number, hi: number}} topicHash cached u32 pair
   * @param {{nodeId: number, scoreHi: number, scoreLo: number}} out
   */
  ownerInto(topicHash, out) {
    const nodes = this.nodes;
    if (nodes.length === 0) return WC.WC_E_TOPIC_UNROUTED;
    // Encode the 12 hash input bytes: node seed u32 LE || topic u64 LE.
    const t = topicHash;
    let bestHi = 0, bestLo = 0, bestNode = 0, first = true;
    for (let i = 0; i < nodes.length; i++) {
      const node = nodes[i];
      fnvReset();
      fnvByte(node & 0xff); fnvByte((node >>> 8) & 0xff);
      fnvByte((node >>> 16) & 0xff); fnvByte((node >>> 24) & 0xff);
      fnvByte(t.lo & 0xff); fnvByte((t.lo >>> 8) & 0xff);
      fnvByte((t.lo >>> 16) & 0xff); fnvByte((t.lo >>> 24) & 0xff);
      fnvByte(t.hi & 0xff); fnvByte((t.hi >>> 8) & 0xff);
      fnvByte((t.hi >>> 16) & 0xff); fnvByte((t.hi >>> 24) & 0xff);
      if (first || scoreWins(bestHi, bestLo) ||
          (S.hi === bestHi && S.lo === bestLo && node > bestNode)) {
        bestHi = S.hi; bestLo = S.lo; bestNode = node; first = false;
      }
    }
    out.nodeId = bestNode;
    out.scoreHi = bestHi;
    out.scoreLo = bestLo;
    return WC.WC_OK;
  }

  /** Cold-path convenience wrapper (allocates). */
  ownerOf(topicHash) {
    const out = {};
    const code = this.ownerInto(topicHash, out);
    if (code !== WC.WC_OK) return null;
    return out.nodeId;
  }
}
