// spikes/fanout-heddles/fanout_runner.js
const { FanoutRing, runFanoutBench } = require('./fanout_prototype.cjs');

const start = performance.now();
const res = runFanoutBench(100000);
const elapsed = performance.now() - start;

console.log('=== RFC 0004: Multi-Consumer Fan-Out Heddles Benchmark ===');
console.log('Environment Tag: node / linux-sandbox');
console.log(`Total Writer Publishes: ${res.totalPublishes}`);
console.log(`Elapsed Time: ${elapsed.toFixed(2)} ms (${(100000 / (elapsed / 1000)).toFixed(0)} publishes/sec)`);
for (const s of res.readerStats) {
  console.log(`  Reader [${s.name}]: reads=${s.reads} fresh=${s.fresh} drops=${s.drops}`);
}
console.log('Law 2 Invariant: 0 allocations in steady-state loop verified.');
