// bench/router.bench.mjs — membership lookup + rendezvous routing latency.
// Emits ONE JSON line: {"kind":"weft-cluster-router-bench", ...}
// CI gates: membership lookup p50 < 10 ns (the mandate), router ownerOf
// p50 recorded honestly (mandate has no number; target < 200ns @ 8 nodes).
import { MembershipTable, ShardRouter, NodeHandle } from '../src/index.js';
import { fnv1a64 } from '../src/wire.js';

const t = new MembershipTable(64);
for (let i = 1; i <= 8; i++) {
  t.upsert({ nodeId: i * 7, addr: 0x0100007f, gossipPort: 5000 + i,
    flags: 1, lastSeenMs: i * 100, incarnation: 0, dataPort: 6000 + i, topicCount: 0 });
}
const out = new NodeHandle();
const ids = [7, 14, 21, 28, 35, 42, 49, 56];

const router = new ShardRouter();
for (let i = 1; i <= 8; i++) router.addNode(i);
const topicHashes = [];
for (let i = 0; i < 64; i++) topicHashes.push(fnv1a64(`bench-topic-${i}`));
const rout = {};

function bench(fn, warmup, iters, batch) {
  for (let i = 0; i < warmup; i++) fn(i);
  const samples = [];
  for (let s = 0; s < 20; s++) {
    const t0 = process.hrtime.bigint();
    for (let i = 0; i < batch; i++) fn(i);
    const t1 = process.hrtime.bigint();
    samples.push(Number(t1 - t0) / batch); // ns per op
  }
  samples.sort((a, b) => a - b);
  const p = (q) => samples[Math.min(samples.length - 1, Math.floor(q * samples.length))];
  return { p50: p(0.5), p99: p(0.99) };
}

// Membership lookup: rotate node ids to defeat branch predictors.
const lookup = bench((i) => { t.lookupInto(ids[i & 7], out); }, 200000, 2000000, 50000);

// Rendezvous routing over a rotating topic set.
const route = bench((i) => { router.ownerInto(topicHashes[i & 63], rout); }, 20000, 200000, 10000);

// Sanity: lookups actually find nodes (no cheating via empty scans).
let hits = 0;
for (let i = 0; i < 8; i++) if (t.lookupInto(ids[i], out) === 0) hits++;
if (hits !== 8) { console.error('LOOKUP SANITY FAILED'); process.exit(1); }

const line = JSON.stringify({
  kind: 'weft-cluster-router-bench',
  membershipLookupNs: { p50: +lookup.p50.toFixed(1), p99: +lookup.p99.toFixed(1) },
  routerOwnerNs: { p50: +route.p50.toFixed(1), p99: +route.p99.toFixed(1), nodes: 8 },
  mandate: 'lock-free cluster state table lookups < 10 ns (Pillar 3 mandate B1)',
});
console.log(line);
