// test/helpers/alloc_probe.mjs — zero-allocation probe (Law 1 hard evidence).
//
// Runs INSIDE `node --expose-gc` (spawned by CI + test/alloc.test.mjs):
//   node --expose-gc helpers/alloc_probe.mjs --mode <mode> --iters <N>
// Forces full GC before and after the steady-state loop and prints ONE line:
// {"mode":..., "iters":..., "heapBefore":..., "heapAfter":..., "deltaBytes":...}
// Modes: publish | subscribe | metrics | router | gossip
//   (gossip light up in its own pillar step — fails here if unregistered.)
import { ClusterClient, ShmFabric, LoopbackShmTransport,
         MembershipTable, ShardRouter, NodeHandle, MetricsRegistry } from '../../src/index.js';

const args = process.argv.slice(2);
function arg(name, dflt) {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : dflt;
}
const mode = arg('mode', 'publish');
const iters = parseInt(arg('iters', '100000'), 10);

function gc() { globalThis.gc(); globalThis.gc(); }

const enc = new TextEncoder();
const PAYLOAD = new Uint8Array(64); // reused producer payload
import { fnv1a64 } from '../../src/wire.js';
const topicHashCache = Array.from({ length: 64 }, (_, i) => fnv1a64(`bench-topic-${i}`));

function makePair() {
  const fabric = new ShmFabric();
  const pub = new ClusterClient({ nodeId: 1, transport: new LoopbackShmTransport(fabric) });
  const con = new ClusterClient({ nodeId: 2, transport: new LoopbackShmTransport(fabric) });
  return { fabric, pub, con };
}

async function buildRun() {
  if (mode === 'publish') {
    const { pub, con } = makePair();
    await pub.start(); await con.start();
    // Real subscriber ring, never iterated — worst-case fanout into memory
    // nobody drains (writer laps the ring; the poke path runs with no waiter).
    con.subscribe('probe');
    pub.registerTopic('probe');
    return {
      run: (n) => {
        for (let i = 0; i < n; i++) {
          PAYLOAD[0] = i & 0xff;
          pub.publish('probe', PAYLOAD);
        }
      },
      done: async () => { await pub.stop(); await con.stop(); },
    };
  }
  if (mode === 'subscribe') {
    const { pub, con } = makePair();
    await pub.start(); await con.start();
    const sub = con.subscribe('probe', { slotCount: 4 });
    const it = sub[Symbol.asyncIterator]();
    pub.registerTopic('probe');
    return {
      run: async (n) => {
        // Cooperative lock-step: request the frame FIRST (the async generator
        // parks on its wait path), then publish to wake it. Publishing after
        // awaiting would deadlock: the parked request can only be woken by
        // the NEXT iteration's publish, which never runs.
        for (let i = 0; i < n; i++) {
          PAYLOAD[0] = i & 0xff;
          const p = it.next();
          pub.publish('probe', PAYLOAD);
          await p;
        }
      },
      done: async () => {
        sub.running = false;
        it.return?.();
        await pub.stop(); await con.stop();
      },
    };
  }
  if (mode === 'metrics') {
    const reg = new MetricsRegistry({ labels: { node: '1' } });
    const frames = reg.counter('weft_cluster_frames_total', 'probe');
    const depth = reg.gauge('weft_cluster_ring_depth', 'probe');
    const lat = reg.histogram('weft_cluster_hop_latency_ns', 'probe');
    let i = 0;
    return {
      run: (n) => {
        for (let k = 0; k < n; k++) {
          i = (i + 1) & 0xffff;
          frames.add(1);
          depth.set(i);
          lat.record(200 + (i & 0x3ff));
        }
      },
      done: async () => {},
    };
  }
  if (mode === 'router') {
    const table = new MembershipTable(64);
    for (let i = 1; i <= 8; i++) {
      table.upsert({ nodeId: i * 7, addr: 0x0100007f, gossipPort: 5000 + i,
        flags: 1, lastSeenMs: 1, incarnation: 0, dataPort: 0, topicCount: 0 });
    }
    const router = new ShardRouter();
    for (let i = 1; i <= 8; i++) router.addNode(i * 7);
    const out = new NodeHandle();
    const route = {};
    const ids = [7, 14, 21, 28, 35, 42, 49, 56];
    return {
      run: (n) => {
        for (let k = 0; k < n; k++) {
          table.lookupInto(ids[k & 7], out);
          router.ownerInto(topicHashCache[k & 63], route);
        }
      },
      done: async () => {},
    };
  }
  throw new Error(`unknown probe mode: ${mode} (wire it up in its own pillar step)`);
}

const job = await buildRun();
// STEADY-STATE methodology: warm up first (JIT/IC metadata is one-time
// allocation, not per-frame garbage), gc, snapshot, THEN measure.
const WARMUP = Math.min(20000, iters);
await job.run(WARMUP);
gc();
const heapBefore = process.memoryUsage().heapUsed;
await job.run(iters);
gc();
const heapAfter = process.memoryUsage().heapUsed;
await job.done();
const line = JSON.stringify({
  mode, iters,
  heapBefore: Math.round(heapBefore),
  heapAfter: Math.round(heapAfter),
  deltaBytes: Math.round(heapAfter - heapBefore),
});
console.log(line);
