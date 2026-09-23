// lib/node_app.mjs — shared node runtime for the distributed cluster feed demo.
//
// One process per node. Each node:
//   * joins the gossip mesh (ClusterMesh) — membership converges across the 4
//     processes over real localhost UDP,
//   * runs its role: the producer paces bursts of frames per topic; consumers
//     subscribe, verify per-topic sequence monotonicity from the payload and
//     measure one-way hop latency (CLOCK_MONOTONIC — same base across
//     processes on the same host),
//   * serves /metrics (Prometheus) from a lock-free registry,
//   * prints `#STATUS {...}` JSON lines to stdout for the orchestrator.
//
// Law 1: publish/subscribe hot paths reuse the client's staging datagram and
// per-subscription rings. Law 4: gaps/stale/decode errors are counted and
// reported; the orchestrator fails the run on monotonicity violations.

import process from 'node:process';
import {
  ClusterMesh, MetricsRegistry, startMetricsServer,
} from '../../../packages/weft-cluster/src/index.js';

const arg = (name, dflt) => {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : dflt;
};

const nodeId = parseInt(arg('id', '1'), 10);
const role = arg('role', 'producer');
const gossipPort = parseInt(arg('gossip-port', '0'), 10);
const seeds = (arg('seeds', '') || '').split(',').filter(Boolean)
  .map((s) => { const [addr, port] = s.split(':'); return { addr, port: +port }; });
const fps = parseInt(arg('fps', '5000'), 10);
const seconds = parseFloat(arg('seconds', '8'));
const topics = ['market.trades', 'market.quotes'];

const mesh = new ClusterMesh({ nodeId, gossipPort });
const reg = new MetricsRegistry({ labels: { node: String(nodeId), role } });
const framesTotal = reg.counter('weft_cluster_frames_total', 'Frames published or consumed.');
const hopHist = reg.histogram('weft_cluster_hop_latency_ns', 'One-way hop latency.');
const gapsCounter = reg.counter('weft_cluster_sequence_gaps_total', 'Detected sequence gaps.');
const staleCounter = reg.counter('weft_cluster_stale_frames_total', 'Stale/replayed frames dropped.');

const stats = {
  node: nodeId, role, members: 0, fps: 0, total: 0, gaps: 0, stale: 0,
  decodeErrors: 0, p50: 0, p99: 0, metricsPort: 0,
  routes: {}, topics,
};

const emitStatus = () => process.stdout.write(`#STATUS ${JSON.stringify(stats)}\n`);

if (role === 'producer') {
  await mesh.start();
  for (const s of seeds) mesh.seed(s.addr, s.port);
  for (const t of topics) mesh.client.registerTopic(t);
  const srv = await startMetricsServer(reg, { port: 0 });
  stats.metricsPort = srv.port;

  const payload = new Uint8Array(96);
  const view = new DataView(payload.buffer);
  const seqByTopic = new Map();
  let windowCount = 0;
  let windowT0 = performance.now();

  const burstMs = 20;
  const perBurst = Math.max(1, Math.floor(fps * burstMs / 1000));
  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline) {
    const b0 = performance.now();
    for (let i = 0; i < perBurst; i++) {
      for (const topic of topics) {
        const seq = (seqByTopic.get(topic) ?? 0) + 1;
        seqByTopic.set(topic, seq);
        view.setBigUint64(0, BigInt(seq), true);            // u64 seq
        view.setBigUint64(8, process.hrtime.bigint(), true); // u64 send ns
        mesh.publish(topic, payload);
        framesTotal.add(1);
        windowCount++;
      }
      // Yield every 64 sends so libuv retires TX-ring slots (deferred sends
      // mean a sync burst larger than the TX ring would drop-tail).
      if ((i & 63) === 63) await new Promise((r) => setImmediate(r));
    }
    const spent = performance.now() - b0;
    const wait = Math.max(1, burstMs - spent);
    await new Promise((r) => setTimeout(r, wait));
    const now = performance.now();
    if (now - windowT0 >= 500) {
      stats.fps = Math.round(windowCount / ((now - windowT0) / 1000) / topics.length);
      stats.total = [...seqByTopic.values()].reduce((a, b) => a + b, 0);
      stats.members = mesh.nodes().length;
      stats.routes = Object.fromEntries(topics.map((t) => [t, mesh.route(t)]));
      windowCount = 0; windowT0 = now;
      emitStatus();
    }
  }
  emitStatus();
  await mesh.leave();
} else {
  await mesh.start();
  for (const s of seeds) mesh.seed(s.addr, s.port);
  const srv = await startMetricsServer(reg, { port: 0 });
  stats.metricsPort = srv.port;

  // One persistent consumer task per topic — for-await handles idle waits via
  // the subscription's poke/1ms-fallback wake, and no request ever queues
  // behind a timed-out race.
  const deadline = Date.now() + seconds * 1000;
  const lastSeq = new Map();
  let attachCatchup = 0;
  let windowCount = 0;
  let windowT0 = performance.now();
  // Data-plane attachment: gossip gives us every peer's dataPort; SUB to any
  // node we haven't attached to yet (retries survive early-datagram loss).
  const connected = new Set();
  const peerTimer = setInterval(() => {
    for (const n of mesh.nodes()) {
      if (n.nodeId !== nodeId && n.dataPort > 0 && !connected.has(n.nodeId)) {
        connected.add(n.nodeId);
        mesh.client.connectPeer('127.0.0.1', n.dataPort);
      }
    }
  }, 50);
  const consumers = topics.map((topic) => {
    const sub = mesh.subscribe(topic, { slotCount: 8192 });
    return (async () => {
      for await (const f of sub) {
        const seq = f.dv.getBigUint64(f.payloadOffset, true);
        const sendNs = f.dv.getBigUint64(f.payloadOffset + 8, true);
        const prev = lastSeq.get(topic);
        if (prev === undefined) {
          // Attach baseline: a late joiner starts mid-stream — the catch-up
          // to the live head is not a monotonicity violation.
          attachCatchup += Number(seq) - 1;
        } else if (seq !== prev + 1n) {
          if (seq <= prev) { staleCounter.add(1); stats.stale++; }
          else { const g = Number(seq - prev - 1n); gapsCounter.add(g); stats.gaps += g; }
        }
        lastSeq.set(topic, seq);
        const hop = Number(process.hrtime.bigint() - sendNs);
        if (hop > 0 && hop < 1e9) hopHist.record(hop);
        stats.total++;
        windowCount++;
        framesTotal.add(1);
        if (Date.now() > deadline) break;
      }
    })();
  });

  const statusTimer = setInterval(() => {
    const now = performance.now();
    stats.fps = Math.round(windowCount / ((now - windowT0) / 1000) / topics.length);
    const h = reg.snapshot().histograms[0];
    stats.p50 = h.p50; stats.p99 = h.p99;
    stats.members = mesh.nodes().length;
    stats.routes = Object.fromEntries(topics.map((t) => [t, mesh.route(t)]));
    stats.decodeErrors = mesh.transport.decodeErrors ?? 0;
    windowCount = 0; windowT0 = now;
    emitStatus();
  }, 500);

  await Promise.all(consumers);
  clearInterval(peerTimer);
  clearInterval(statusTimer);
  stats.members = mesh.nodes().length;
  stats.routes = Object.fromEntries(topics.map((t) => [t, mesh.route(t)]));
  emitStatus();
  await mesh.leave();
}
