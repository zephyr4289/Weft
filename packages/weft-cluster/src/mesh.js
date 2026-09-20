// mesh.js — ClusterMesh: one-stop facade wiring a ClusterClient (data plane),
// a GossipEngine + MembershipTable (control plane) and a ShardRouter
// (topic -> node placement) behind a single object for apps, the CLI and the
// demo. Data-plane Law 1 guarantees are inherited unchanged — this layer only
// adds tick-rate (cold) work.
//
//   const mesh = new ClusterMesh({ nodeId: 1, gossipPort: 5101 });
//   await mesh.start();
//   mesh.seed('127.0.0.1', 5102);
//   for await (const f of mesh.subscribe('telemetry')) { ... }

import dgram from 'node:dgram';
import { ClusterClient } from './client.js';
import { UdpTransport, LoopbackShmTransport, ShmFabric } from './transport/index.js';
import { GossipEngine, NODE_FLAG_LEAVING } from './topology.js';
import { ShardRouter } from './router.js';
import { fnv1a64 } from './wire.js';

export class ClusterMesh {
  /**
   * @param {{nodeId: number, mode?: 'udp'|'loopback-shm',
   *          gossipPort?: number, dataPort?: number, addr?: string,
   *          payloadMax?: number, heartbeatMs?: number,
   *          onEvent?: (ev: any) => void}} opts
   */
  constructor(opts) {
    this.nodeId = opts.nodeId >>> 0;
    this.mode = opts.mode ?? 'udp';
    this.heartbeatMs = opts.heartbeatMs ?? 200;
    this.addr = opts.addr ?? '127.0.0.1';
    if (this.mode === 'udp') {
      this.transport = new UdpTransport({ port: opts.dataPort ?? 0 });
    } else {
      this.fabric = opts.fabric ?? new ShmFabric();
      this.transport = new LoopbackShmTransport(this.fabric);
    }
    this.client = new ClusterClient({
      nodeId: this.nodeId,
      transport: this.transport,
      payloadMax: opts.payloadMax ?? 1024,
    });
    // Datagram transports deliver raw frames; the mesh wires the pump into
    // the client (the shm fabric delivers through rings directly).
    if (typeof this.transport.addPeer === 'function') {
      this.transport.onFrame = (h, base, bytes, i32) =>
        this.client._onTransportFrame(h, base, bytes, i32);
    }
    this.router = new ShardRouter();
    this.router.addNode(this.nodeId);
    this._routeMemo = new Map(); // topic -> {version, nodeId}
    this.gossip = null;
    this._gossipSocket = null;
    this._timer = null;
    this._onEvent = opts.onEvent ?? null;
    if (this.mode === 'udp') {
      this.gossip = new GossipEngine({
        nodeId: this.nodeId,
        gossipPort: opts.gossipPort ?? 0,
        dataPort: 0, // patched in start() once the data socket binds
        send: (bytes, target) => this._gossipSend(bytes, target),
        onEvent: (ev) => this._onTopologyEvent(ev),
      });
    }
  }

  /** Cold path: seed a gossip peer. */
  seed(addr, port) { this.gossip?.join(addr, port); }

  async start() {
    await this.client.start();
    if (this.gossip) {
      // Patch the data port into our self entry so peers learn where to send.
      const me = this.gossip.table.view.get(this.nodeId);
      if (me) me.dataPort = this.transport.dataPort;
      this._gossipSocket = dgram.createSocket('udp4');
      await new Promise((res) => this._gossipSocket.bind(this.gossip.gossipPort, this.addr, res));
      this.gossip.gossipPort = this._gossipSocket.address().port;
      this._gossipSocket.on('message', (msg, rinfo) => {
        this.gossip.onDatagram(new Uint8Array(msg.buffer, msg.byteOffset, msg.byteLength), rinfo);
      });
      this._tick(); // announce immediately
      this._timer = setInterval(() => this._tick(), this.heartbeatMs);
    }
  }

  _tick() { this.gossip?.tick(); }

  _gossipSend(bytes, target) {
    if (!this._gossipSocket) return;
    const buf = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    this._gossipSocket.send(buf, target.port, target.addr);
  }

  _onTopologyEvent(ev) {
    this.router.syncFromTable(this.gossip.table);
    this._onEvent?.(ev);
  }

  /** Publish passthrough (zero-cost; see ClusterClient.publish). */
  publish(topic, payloadOrLen, fill) {
    return this.client.publish(topic, payloadOrLen, fill);
  }

  /** Subscribe passthrough (async-iterable Subscription). */
  subscribe(topic, opts) { return this.client.subscribe(topic, opts); }

  /**
   * Resolve (and memoize) the owner node of a topic via rendezvous hashing.
   * Memo invalidated automatically when membership changes.
   * @returns {number|null} nodeId or null when unrouted
   */
  route(topic) {
    const memo = this._routeMemo.get(topic);
    if (memo && memo.version === this.router.version) return memo.nodeId;
    const h = fnv1a64(topic);
    const out = {};
    const code = this.router.ownerInto(h, out);
    const nodeId = code === 0 ? out.nodeId : null;
    this._routeMemo.set(topic, { version: this.router.version, nodeId });
    return nodeId;
  }

  /** Cold-path membership snapshot. */
  nodes() { return this.gossip ? this.gossip.members() : []; }

  /** Graceful shutdown: announce LEAVING, stop heartbeats, close sockets. */
  async leave() {
    if (this._timer) { clearInterval(this._timer); this._timer = null; }
    this.gossip?.leave();
    await new Promise((r) => setTimeout(r, this.heartbeatMs)); // let it propagate
    if (this._gossipSocket) {
      await new Promise((r) => this._gossipSocket.close(() => r()));
      this._gossipSocket = null;
    }
    await this.client.stop();
  }
}
