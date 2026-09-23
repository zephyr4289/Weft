// transport/udp.js — UdpTransport: cross-process / cross-language cluster
// datagrams (node:dgram). This is the wire-mode transport the 4-node demo and
// the cross-language parity tests run on; a WCR1 RDMA/XDP transport from the
// substrate engineers drops in behind the same 4-method surface:
//
//   start() / stop() / publish(topic, srcI32, words, frameHi) / onFrame
//
// RX path models an XDP AF_XDP umem ring: each kernel datagram is copied once
// into a preallocated slot of a fixed ring (the only copy — the socket stack
// owns its own buffers), then decoded in place. Law 1: the per-frame JS cost
// is one memcpy + typed reads; zero allocations of ours (the msg Buffer is
// Node-internal and dies immediately).

import dgram from 'node:dgram';
import { WC_FLAG_CONTROL, decodeWcn1Into, FrameHandle } from '../wire.js';
import { ShmRing } from './shm.js';

export const CTL_SUB = 1;
export const CTL_UNSUB = 2;

// libuv defers UDP sends: a shared staging Buffer mutated between send()
// calls transmits WRONG contents (proven by probe: 5 sends -> all deliver
// the last frame's bytes). The tx ring gives every in-flight datagram its
// own preallocated slot; drop-tail when saturated (Law 4: drops are counted).
const TX_SLOTS = 1024;

function peerKey(addr, port) { return `${addr}:${port}`; }

export class UdpTransport {
  /**
   * @param {{host?: string, port?: number, maxDatagram?: number, rxSlots?: number}} [opts]
   */
  constructor(opts = {}) {
    this.host = opts.host ?? '127.0.0.1';
    this.port = opts.port ?? 0;
    this.maxDatagram = opts.maxDatagram ?? 1280;
    this.rxSlots = opts.rxSlots ?? 1024;
    this.kind = 'udp';
    this.socket = null;
    this.dataPort = 0;
    /** @type {Set<string>} */
    this.peers = new Set();
    /** topic identity `${lo}:${hi}` -> Set<peerKey> */
    this._subs = new Map();
    /** @type {Map<string, {addr: string, port: number}>} */
    this.peerAddrs = new Map();
    /** set by ClusterClient: (frameLo, frameHi, srcI32, words) => void */
    this.onFrame = null;
    // RX ring: preallocated slots; slot stride 16B slack + maxDatagram.
    this.rx = new ShmRing(this.rxSlots, this.maxDatagram);
    this._rxNext = 0;
    this._rxHandle = new FrameHandle().bind(this.rx.bytes, 0);
    this._rxI32 = new Int32Array(this.rx.bytes);
    // TX ring: one preallocated slot per in-flight datagram (see TX_SLOTS).
    this.tx = new ShmRing(TX_SLOTS, this.maxDatagram);
    this._txRingBuf = Buffer.from(this.tx.bytes); // ONE Buffer view, no copies
    this._txNext = 0;
    this.txOutstanding = 0;
    this.txDrops = 0;      // Law 4: tx ring saturated -> drop-tail
    this.drops = 0;        // rx slot overwrite (slow pump)
    this.decodeErrors = 0; // Law 4: malformed inbound datagrams rejected
    /** cold-path stats for tests/metrics */
    this.rxCount = 0;
    this.txCount = 0;
  }

  async start() {
    this.socket = dgram.createSocket({ type: 'udp4', reuseAddr: false });
    await new Promise((res, rej) => {
      this.socket.once('error', rej);
      this.socket.bind(this.port, this.host, () => {
        this.socket.removeListener('error', rej);
        const a = this.socket.address();
        this.dataPort = a.port;
        res();
      });
    });
    // Size the kernel receive buffer like an XDP umem would be provisioned:
    // default ~212KB overruns at sustained burst rates and silently drops
    // datagrams (observed as Law 4 seq gaps in the demo).
    try { this.socket.setRecvBufferSize(4 * 1024 * 1024); } catch { /* best effort */ }
    this.socket.on('message', (msg, rinfo) => this._onMessage(msg, rinfo));
  }

  async stop() {
    if (this.socket) {
      await new Promise((res) => this.socket.close(() => res()));
      this.socket = null;
    }
  }

  addPeer(addr, port) {
    const key = peerKey(addr, port);
    this.peers.add(key);
    this.peerAddrs.set(key, { addr, port });
    return key;
  }

  dropPeer(addr, port) {
    const key = peerKey(addr, port);
    this.peers.delete(key);
    this.peerAddrs.delete(key);
  }

  /**
   * Publish the staged datagram to every peer subscribed to the topic.
   * Uniform transport signature: same as LoopbackShmTransport.publish with
   * extra datagram-mode params (txBuf view is preallocated by the client).
   * @param {string} topic
   * @param {Int32Array} srcI32 u32 view over the staging datagram (unused here)
   * @param {number} srcIdx (unused)
   * @param {number} words (unused)
   * @param {number} frameHi (unused — seq already in the header)
   * @param {Buffer} txBuf preallocated Buffer VIEW over the staging datagram
   * @param {number} bytes total datagram bytes
   * @param {number} topicLo @param {number} topicHi
   */
  publish(topic, srcI32, srcIdx, words, frameHi, txBuf, bytes, topicLo, topicHi) {
    const set = this._subs.get(`${topicLo}:${topicHi}`);
    if (!set || set.size === 0) { this.txCount++; return 0; }
    if (this.txOutstanding >= TX_SLOTS - 1) {
      // Every tx slot is in flight — drop-tail honestly instead of sending
      // torn datagrams (the slot WILL be reused by an upcoming frame).
      this.txDrops++;
      this.txCount++;
      return 0;
    }
    // Snapshot the staged datagram into its own tx slot, send from there.
    const idx = (this._txNext = (this._txNext + 1) >>> 0) % TX_SLOTS;
    const slotBase = 128 + idx * this.tx.stride + 16;
    const dstI32 = this.tx.i32;
    const dstOff = slotBase >> 2;
    for (let i = 0; i < words; i++) dstI32[dstOff + i] = srcI32[srcIdx + i];
    this.txOutstanding += set.size;
    for (const key of set) {
      const p = this.peerAddrs.get(key);
      if (p) {
        this.socket.send(this._txRingBuf, slotBase, bytes, p.port, p.addr,
                         () => { this.txOutstanding--; });
      }
    }
    this.txCount++;
    return set.size;
  }

  /** Send a SUB/UNSUB control datagram for a topic to a peer. */
  control(peerKeyStr, ctl, topicLo, topicHi, srcNode, stagingDv, txBuf) {
    // Encode a minimal WCN1 control header directly into the staging buffer.
    stagingDv.setUint8(0, 0x57); stagingDv.setUint8(1, 0x43);
    stagingDv.setUint8(2, 0x4e); stagingDv.setUint8(3, 0x31);
    stagingDv.setUint16(4, 1, true);
    stagingDv.setUint16(6, 64, true);
    stagingDv.setUint32(8, WC_FLAG_CONTROL, true);
    stagingDv.setUint32(12, srcNode >>> 0, true);
    stagingDv.setUint32(16, topicLo >>> 0, true);
    stagingDv.setUint32(20, topicHi >>> 0, true);
    stagingDv.setUint32(24, 0, true); stagingDv.setUint32(28, 0, true); // seq
    stagingDv.setUint32(32, 0, true); stagingDv.setUint32(36, 0, true); // ts
    stagingDv.setUint32(40, 1, true);                                    // payload_len 1
    stagingDv.setUint32(44, 0, true);                                    // schema
    stagingDv.setUint32(48, 0, true);                                    // crc
    stagingDv.setUint32(52, 0, true);                                    // rdma key
    stagingDv.setUint32(56, 0, true); stagingDv.setUint32(60, 0, true);
    stagingDv.setUint8(64, ctl);
    const p = this.peerAddrs.get(peerKeyStr);
    if (!p) return;
    // Controls are rare — but still snapshot into a tx slot (deferred sends!).
    const idx = (this._txNext = (this._txNext + 1) >>> 0) % TX_SLOTS;
    const slotBase = 128 + idx * this.tx.stride + 16;
    const dstI32 = this.tx.i32;
    const srcI32 = new Int32Array(stagingDv.buffer, stagingDv.byteOffset, 17); // 68B
    const off = slotBase >> 2;
    for (let i = 0; i < 17; i++) dstI32[off + i] = srcI32[i];
    this.txOutstanding++;
    this.socket.send(this._txRingBuf, slotBase, 65, p.port, p.addr,
                     () => { this.txOutstanding--; });
  }

  _onMessage(msg, rinfo) {
    const slot = (this._rxNext = (this._rxNext + 1) >>> 0) - 1;
    const base = 128 + (slot % this.rx.slotCount) * this.rx.stride + 16; // ring slot data area
    const u8 = this._rxHandle.u8;
    const n = msg.length;
    if (n > this.maxDatagram) { this.drops++; return; }
    for (let i = 0; i < n; i++) u8[base + i] = msg[i];
    this.rxCount++;
    const h = this._rxHandle;
    const code = decodeWcn1Into(h, base);
    if (code !== 0) { this.decodeErrors++; return; }
    // Control plane: SUB/UNSUB from remote subscribers (rinfo = the peer).
    if ((h.flags & WC_FLAG_CONTROL) !== 0) {
      const ctl = u8[base + h.headerSize];
      const key = peerKey(rinfo.address, rinfo.port);
      const tk = `${h.topicLo}:${h.topicHi}`;
      if (ctl === 1) {
        // SUB = connect + subscribe: register the peer for data fan-out.
        this.peers.add(key);
        this.peerAddrs.set(key, { addr: rinfo.address, port: rinfo.port });
        let set = this._subs.get(tk);
        if (!set) { set = new Set(); this._subs.set(tk, set); }
        set.add(key);
      } else if (ctl === 2) {
        const set = this._subs.get(tk);
        if (set) set.delete(key);
      }
      return;
    }
    if (this.onFrame) this.onFrame(h, base, n, this._rxI32);
  }
}

// Re-export for the client to build subscription rings.
export { ShmRing };
