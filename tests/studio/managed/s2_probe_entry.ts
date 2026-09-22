/**
 * Stage 2 probe entry — bundled by `bun build --target=node` and executed by
 * `node --expose-gc`. Ingests 1,000,000 telemetry messages through the studio
 * engine path, forcing GC at checkpoints; asserts ≤ 64 KiB heap growth.
 * A retained-allocation control MUST bite (≥ 4 MiB) or the harness is broken.
 */

import { RingMap } from '../../../packages/studio/src/engine/ring.ts';
import { Ingestion } from '../../../packages/studio/src/engine/ingest.ts';

const gc = globalThis.gc;
if (typeof gc !== 'function') {
  console.error('E_HARNESS: gc() unavailable — run with node --expose-gc');
  process.exit(3);
}

const heap = () => process.memoryUsage().heapUsed;

function measure(iterations, rate, control) {
  const ring = new RingMap(65536);
  const ing = new Ingestion(ring);
  // warmup (JIT tier-up outside the measured window)
  for (let i = 0; i < 100_000; i++) {
    ing.ingestTick(0, i * 1000, 1_000_000_000 + (i % 1_000_000), i % 5000, i % 2);
  }
  gc(); gc();
  const base = heap();
  if (control) {
    const sink = [];
    for (let i = 0; i < iterations; i++) {
      ing.ingestTick(0, i * 1000, 1_500_000_000 + (i % 1_000_000), i % 5000, i % 2);
      sink.push({ i, pad: i % 7 }); // retained allocation — MUST bite
    }
    if (sink.length === 0) throw new Error('unreachable');
  } else {
    for (let i = 0; i < iterations; i++) {
      ing.ingestTick(0, i * 1000, 1_500_000_000 + (i % 1_000_000), i % 5000, i % 2);
    }
  }
  gc(); gc();
  return { growth: heap() - base, published: ring.counters.published };
}

const N = 1_000_000;
const hot = measure(N, 0, false);
const control = measure(N, 0, true);

const KIB = 1024;
const result = {
  messages: N,
  hot_growth_kib: Math.round((hot.growth / KIB) * 1000) / 1000,
  control_growth_mib: Math.round((control.growth / KIB / 1024) * 100) / 100,
  gate_hot_kib: 64,
  gate_control_mib_min: 4,
  ok: hot.growth <= 64 * KIB && control.growth >= 4 * 1024 * KIB,
};
console.log(JSON.stringify(result, null, 2));
process.exit(result.ok ? 0 : 2);
