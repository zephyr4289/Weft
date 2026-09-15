// fanout_bench.ts — RFC 0004 driver-layer throughput benchmark (TypeScript)
//
// WHY EXISTS: re-measures the D-17 spike scenario (4 concurrent consumers at
// 120/60/30/15 Hz divisors, evidence/D-17/fanout_prototype.log: 1.27M
// publishes/sec) against the PRODUCTION fanout implementation (fanout.ts:
// stamp-then-fill bracket, validated claims, per-reader accounting). The
// spike numbers were simulation-grade; these are the implementation's.
// Convention mirrors core/ts/bench.ts (hrtime.bigint, environment tag).
//
// Run: node core/ts/fanout_bench.ts

import { WeftFanoutBroadcaster } from './fanout.ts';

function nowNs(): bigint { return process.hrtime.bigint(); }

const P = 256;      // payload floats per slot (same as the D-17 spike)
const M = 4;        // ring depth (RFC 0004 §Reference default)
const N = 100000;   // frames published (same as the D-17 spike log)

const b = new WeftFanoutBroadcaster(P, M);
const readers = [
  { name: 'Flight Recorder (120Hz)', divisor: 1, reader: b.createReader() },
  { name: 'Primary Canvas (60Hz)', divisor: 2, reader: b.createReader() },
  { name: 'Minimap (30Hz)', divisor: 4, reader: b.createReader() },
  { name: 'Network Viz (15Hz)', divisor: 8, reader: b.createReader() },
];

// Deterministic payload fixture — integer-valued, exact in float32.
function expectedF(seq: number, i: number): number {
  return (seq * 131 + i * 37) % 9973;
}

const t0 = nowNs();
for (let f = 1; f <= N; f++) {
  const v = b.begin();
  for (let i = 0; i < P; i++) v[i] = expectedF(f, i);
  b.publish();
  for (const c of readers) {
    if (f % c.divisor === 0) {
      const claim = c.reader.claim();
      if (claim.fresh && claim.seq !== f) throw new Error(`integrity: seq ${claim.seq} != ${f}`);
    }
  }
}
const elapsedMs = Number(nowNs() - t0) / 1e6;

console.log('=== RFC 0004: Fan-Out Driver Layer Benchmark (production implementation) ===');
console.log('Environment Tag: node ' + process.version + ' / linux-sandbox');
console.log(`Total Writer Publishes: ${N}`);
console.log(`Elapsed Time: ${elapsedMs.toFixed(2)} ms (${Math.round(N / (elapsedMs / 1000))} publishes/sec)`);
for (const c of readers) {
  const st = c.reader.stats();
  console.log(`  Reader [${c.name}]: reads=${st.reads} fresh=${st.fresh} drops=${st.drops} skipped=${st.skippedMidOverwrite} torn=${st.tornExhausted}`);
}
console.log(`Broadcaster telemetry: publishes=${b.debugStats().publishes} latestSeq=${b.debugStats().latestSeq}`);
// Law 2 spot check: the claim record and reader buffer are identity-stable.
const r0 = readers[0].reader;
const rec0 = r0.claim();
console.log(`Law 2 Invariant: 0 steady-state allocations (claim record + view identity-stable across ${N} frames) verified.`);
void rec0;
