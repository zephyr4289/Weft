#!/usr/bin/env node
// weft-cluster — CLI for the weft-cluster managed plane (Pillar 3 deliverable 3).
//
//   weft-cluster join   --id 7 --gossip-port 5101 --seed 127.0.0.1:5102
//   weft-cluster status --seed 127.0.0.1:5102 [--observe-ms 3000]
//   weft-cluster bench  [--seconds 3] [--payload 64]
//
// `join`    — run a gossip+data node; Ctrl-C announces LEAVING gracefully.
// `status`  — observe the gossip mesh for a window, dump membership + routes.
// `bench`   — self-contained loopback-shm pub/sub burst: achieved fps and
//             publish->consume hop latency (p50/p99) as one JSON line.

import process from 'node:process';
import { pathToFileURL } from 'node:url';
const CLUSTER_PKG = new URL('../../../packages/weft-cluster/src/index.js', import.meta.url);
const { ClusterClient, ClusterMesh, LoopbackShmTransport, ShmFabric,
        MetricsRegistry } = await import(pathToFileURL(CLUSTER_PKG.pathname));

const argv = process.argv.slice(2);
const cmd = argv[0] ?? 'help';
const opt = (name, dflt) => {
  const i = argv.indexOf(`--${name}`);
  if (i < 0) return dflt;
  const v = argv[i + 1];
  return v === undefined || String(v).startsWith('--') ? true : v;
};

function seedsFromOpt() {
  const raw = opt('seed', null);
  return raw ? String(raw).split(',').map((s) => {
    const [addr, port] = s.split(':');
    return { addr, port: parseInt(port, 10) };
  }) : [];
}

async function join() {
  const id = parseInt(opt('id', String(1 + (process.pid % 1000))), 10);
  const gossipPort = parseInt(opt('gossip-port', '0'), 10);
  const mesh = new ClusterMesh({ nodeId: id, gossipPort });
  mesh.onEvent = (ev) => console.log(`[event] ${ev.type} node=${ev.nodeId}`);
  await mesh.start();
  for (const s of seedsFromOpt()) mesh.seed(s.addr, s.port);
  console.log(`weft-cluster node ${id} joined (gossip :${mesh.gossip.gossipPort}, data :${mesh.transport.dataPort})`);
  console.log('heartbeating... Ctrl-C to leave gracefully');
  process.on('SIGINT', async () => {
    console.log('\nannouncing LEAVING...');
    await mesh.leave();
    process.exit(0);
  });
  setInterval(() => {
    const nodes = mesh.nodes().map((n) => n.nodeId).sort((a, b) => a - b);
    console.log(`[${new Date().toISOString()}] view: ${JSON.stringify(nodes)} (${nodes.length} nodes)`);
  }, parseInt(opt('report-ms', '2000'), 10));
}

async function status() {
  const observeMs = parseInt(opt('observe-ms', '3000'), 10);
  const mesh = new ClusterMesh({ nodeId: parseInt(opt('id', '999'), 10), gossipPort: 0 });
  await mesh.start();
  for (const s of seedsFromOpt()) mesh.seed(s.addr, s.port);
  console.log(`observing gossip mesh for ${observeMs} ms...`);
  await new Promise((r) => setTimeout(r, observeMs));
  const nodes = mesh.nodes();
  console.log(JSON.stringify({
    observer: mesh.nodeId,
    membershipSize: nodes.length,
    nodes: nodes.map((n) => ({
      nodeId: n.nodeId,
      gossipPort: n.gossipPort,
      dataPort: n.dataPort,
      flags: n.flags,
      incarnation: n.incarnation,
      lastSeenMs: n.lastSeenMs,
    })),
    routes: String(opt('routes', '')).split(',').filter(Boolean).map((topic) => ({
      topic, owner: mesh.route(topic),
    })),
  }, null, 2));
  await mesh.leave();
}

async function bench() {
  const seconds = parseFloat(opt('seconds', '3'));
  const size = parseInt(opt('payload', '64'), 10);
  const fabric = new ShmFabric();
  const pub = new ClusterClient({ nodeId: 1, transport: new LoopbackShmTransport(fabric) });
  const con = new ClusterClient({ nodeId: 2, transport: new LoopbackShmTransport(fabric) });
  await pub.start(); await con.start();
  const reg = new MetricsRegistry({ labels: { mode: 'loopback-shm' } });
  const lat = reg.histogram('weft_cluster_hop_latency_ns', 'publish->consume.');
  const sub = con.subscribe('bench', { slotCount: 4096 });
  const it = sub[Symbol.asyncIterator]();
  // NOTE: no warm-up next() here — a pending request would silently consume
  // frame 1 and deadlock every batch one frame short. The first drain's
  // next() primes the generator and finds frames already committed.

  // Batched backpressure: publish a batch, then drain it. A fully
  // synchronous burst would lap every ring (single event loop — the consumer
  // cannot run mid-burst), which measures ring depth, not throughput.
  const BATCH = 512;
  const total = Math.max(BATCH, Math.floor(200000 * seconds));
  const payload = new Uint8Array(size);
  let consumed = 0;
  const drain = async (n) => {
    for (let i = 0; i < n; i++) {
      const r = await it.next();
      if (r.done) return;
      const f = r.value;
      const sendNs = (BigInt(f.tsHi) << 32n) | BigInt(f.tsLo);
      const hop = Number(process.hrtime.bigint() - sendNs);
      lat.record(hop > 0 && hop < 1e9 ? hop : 1);
      consumed++;
    }
  };
  const t0 = process.hrtime.bigint();
  for (let done = 0; done < total; done += BATCH) {
    const n = Math.min(BATCH, total - done);
    for (let i = 0; i < n; i++) {
      payload[0] = (done + i) & 0xff;
      pub.publish('bench', payload);
    }
    await drain(n);
  }
  const elapsed = Number(process.hrtime.bigint() - t0) / 1e9;
  const hist = reg.snapshot().histograms[0];
  console.log(JSON.stringify({
    kind: 'weft-cluster-bench', transport: 'loopback-shm', payloadBytes: size,
    published: consumed, consumed, seconds: +elapsed.toFixed(2),
    achievedFps: Math.round(consumed / elapsed),
    hopLatencyNs: { p50: hist.p50, p99: hist.p99 },
    gaps: sub.gapCount, stale: sub.staleCount,
  }));
  sub.running = false;
  it.return?.();
  await pub.stop(); await con.stop();
}

const USAGE = `weft-cluster <join|status|bench> [options]
  join   --id N --gossip-port P [--seed host:port,...] [--report-ms N]
  status --seed host:port,... [--observe-ms N] [--routes t1,t2]
  bench  [--seconds N] [--payload N]`;
const handlers = {
  join, status, bench,
  help: () => console.log(USAGE),
};
await (handlers[cmd] ?? handlers.help)();
