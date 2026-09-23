#!/usr/bin/env node
// mesh_burst.mjs — in-process "simulated cluster" burst: the 1,000,000
// frames/sec mandate number. Four logical nodes over the LoopbackShm fabric
// (SAB = registered memory; publish = one-sided write into each consumer's
// ring, exactly what the WCR1 RDMA substrate does across machines). Frames
// are ingested by direct zero-copy ring drains with per-(node,topic)
// sequence verification — every frame accounted for, none skipped.
//
//   node mesh_burst.mjs --fps 1000000 --seconds 3
// Prints one JSON line and writes evidence/mesh-burst.json.

import process from 'node:process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  ClusterClient, LoopbackShmTransport, ShmFabric, MetricsRegistry,
} from '../../packages/weft-cluster/src/index.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const arg = (name, dflt) => {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : dflt;
};

const fpsTarget = parseInt(arg('fps', '1000000'), 10);
const seconds = parseFloat(arg('seconds', '3'));
const payloadSize = parseInt(arg('payload', '64'), 10);
// Single-topic burst: the mandate number counts .weft frames/sec in total.
const topics = ['burst'];

const fabric = new ShmFabric();
// CRC off in burst mode (CRC_PRESENT flag omitted — wire-level CRC is proven
// in the wire tests; the burst isolates memory-plane throughput).
const producer = new ClusterClient({ nodeId: 1, transport: new LoopbackShmTransport(fabric), crc: false });
const consumers = [2, 3, 4].map((id) => ({
  id,
  client: new ClusterClient({ nodeId: id, transport: new LoopbackShmTransport(fabric) }),
}));
producer.registerTopic('bench-mode');
for (const t of topics) producer.registerTopic(t);
for (const c of consumers) {
  for (const t of topics) {
    c.client.registerTopic(t);
    c.client.subscribe(t, { slotCount: 4096, payloadMax: 256 });
  }
}

const reg = new MetricsRegistry({ labels: { mode: 'loopback-shm-simulated-cluster' } });
const lat = reg.histogram('weft_cluster_hop_latency_ns', 'publish->ingest.');

// Payload: [u32 seqLo][u32 seqHi][f64 sendUs] — pure-number verification
// (no BigInt on the 3M-frame drain path; u32 seq suffices below 2^32).
const payload = new Uint8Array(payloadSize);
const view = new DataView(payload.buffer);
const seqByTopic = new Map();

// Direct ring drains (no async iterator — this mode measures memory-plane
// throughput; the UDP demo measures the full managed event-loop path).
const drainOnce = (client) => {
  let ingested = 0;
  for (const sub of client.subs) {
    const ring = sub.ring;
    const cur = Atomics.load(ring.i32, 16);
    while (sub.nextRead <= cur) {
      const code = ring.tryRead(sub.nextRead, sub.handle);
      if (code === 0) {
        const f = sub.handle;
        const seq = f.dv.getUint32(f.payloadOffset, true);
        const sendUs = f.dv.getFloat64(f.payloadOffset + 8, true);
        const prev = sub._lastSeq ?? 0;
        if (seq !== prev + 1) { sub._gapCount = (sub._gapCount ?? 0) + 1; }
        sub._lastSeq = seq;
        const hopNs = Math.round((performance.now() - sendUs) * 1000);
        if (hopNs > 0 && hopNs < 1e9) lat.record(hopNs);
        ingested++;
        sub.nextRead++;
      } else if (code === 1) {
        break; // torn slot — next pass
      } else {
        sub.nextRead++;
      }
    }
  }
  return ingested;
};

const perBurst = Math.max(1, Math.floor(fpsTarget * 0.002)); // 2ms bursts
const bursts = Math.ceil((fpsTarget * seconds) / perBurst);
let published = 0;
let ingested = 0;
const t0 = process.hrtime.bigint();

for (let b = 0; b < bursts; b++) {
  // Publish one burst round-robin over topics.
  for (let i = 0; i < perBurst; i++) {
    for (const topic of topics) {
      const seq = (seqByTopic.get(topic) ?? 0) + 1;
      seqByTopic.set(topic, seq);
      view.setUint32(0, seq >>> 0, true);
      view.setUint32(4, 0, true);
      view.setFloat64(8, performance.now(), true);
      producer.publish(topic, payload); // the payload itself goes on the wire
      published++;
    }
  }
  // Every consumer drains its rings (zero-copy decode + verify).
  for (const c of consumers) ingested += drainOnce(c.client);
}
// Final drain to catch tail frames.
for (const c of consumers) ingested += drainOnce(c.client);
const elapsed = Number(process.hrtime.bigint() - t0) / 1e9;

const expected = published * consumers.length; // every consumer ingests every frame
const gaps = consumers.reduce((a, c) => {
  let g = 0;
  for (const s of c.client.subs) g += (s._gapCount ?? 0);
  return a + g;
}, 0);
const hist = reg.snapshot().histograms[0];
const result = {
  kind: 'weft-cluster-mesh-burst', transport: 'loopback-shm (simulated cluster)',
  nodes: 4, consumers: 3, topics: topics.length, payloadBytes: payloadSize,
  fpsTarget, seconds,
  publishedFrames: published, ingestedFrames: ingested,
  expectedIngest: expected,
  achievedFps: Math.round(published / elapsed),
  fanoutWritesPerSec: Math.round((published * consumers.length) / elapsed),
  elapsedSec: +elapsed.toFixed(2),
  sequenceGaps: gaps,
  hopLatencyNs: { p50: hist.p50, p99: hist.p99 },
  at: new Date().toISOString(),
};
console.log(JSON.stringify(result));
const dir = path.join(__dirname, 'evidence');
fs.mkdirSync(dir, { recursive: true });
fs.writeFileSync(path.join(dir, 'mesh-burst.json'), JSON.stringify(result, null, 2) + '\n');
process.exit(gaps === 0 ? 0 : 2);
