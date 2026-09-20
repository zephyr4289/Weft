// test/helpers/alloc_probe.mjs — zero-allocation probe (Law 1 hard evidence).
//
// Runs INSIDE `node --expose-gc` (spawned by CI + test/alloc.test.mjs):
//   node --expose-gc helpers/alloc_probe.mjs --mode <mode> --iters <N>
// Forces full GC before and after the steady-state loop and prints ONE line:
// {"mode":..., "iters":..., "heapBefore":..., "heapAfter":..., "deltaBytes":...}
// Modes: publish | subscribe | metrics | router | gossip
//   (metrics/router/gossip light up in P3/P4 — they fail here if unregistered.)
import { ClusterClient, ShmFabric, LoopbackShmTransport } from '../../src/index.js';

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
  throw new Error(`unknown probe mode: ${mode} (wire it up in its own pillar step)`);
}

const job = await buildRun();
// Warm up JIT + lazy singletons, then measure steady state.
await job.run(1000);
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
