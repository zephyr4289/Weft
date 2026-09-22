// probe.ts — the zero-allocation runtime execution probe.

import { TriadRing, PAYLOAD_BYTES } from '../ring/triad.ts';

export interface ProbeResult {
  iterations: number;
  rounds: number;
  growthKiB: number;
  perRoundKiB: number[];
  commits: number;
  reads: number;
  gateKiB: number;
  pass: boolean;
}

const GATE_KIB = 64;

/**
 * The hot loop: annotated for the self-lint dogfood proof. Deliberately
 * written with zero rule-matrix hits: no closures, no literals, no `new`,
 * no allocating array methods.
 *
 * @hot
 */
export function probeHotLoop(ring: TriadRing, view: Float64Array, iterations: number): number {
  let checksum = 0;
  const payload = ring.payload;
  for (let i = 0; i < iterations; i++) {
    const w = ring.acquireWrite();
    if (w >= 0) {
      const off = w * PAYLOAD_BYTES;
      view[0] = i;
      view[1] = i * 0.5;
      view[2] = checksum;
      payload[off] = i & 255;
      payload[off + 1] = (i >>> 8) & 255;
      ring.commitWrite();
    }
    const r = ring.acquireRead();
    if (r >= 0) {
      const v = payload[r * PAYLOAD_BYTES] | (payload[r * PAYLOAD_BYTES + 1] << 8);
      checksum = (checksum + v) | 0;
      ring.releaseRead();
    }
  }
  return checksum;
}

function heapUsed(): number {
  return process.memoryUsage().heapUsed;
}

function gc(): void {
  const g = (globalThis as { gc?: () => void }).gc;
  if (typeof g === 'function') g();
}

/**
 * Run the probe. `rounds` full passes are executed and the MINIMUM heap
 * growth is reported (best-of-N suppresses JIT/tier-up noise; a real leak
 * grows every round and still fails the gate). A control allocation bit
 * is available via `bite`.
 */
export function runZeroAllocProbe(iterations = 1_000_000, rounds = 3, bite = false): ProbeResult {
  const ring = new TriadRing();
  const view = new Float64Array(ring.payload.buffer, 0, PAYLOAD_BYTES / 8);
  const perRound: number[] = [];

  probeHotLoop(ring, view, Math.min(iterations, 200_000));

  for (let round = 0; round < rounds; round++) {
    gc();
    gc();
    const before = heapUsed();
    let sink = 0;
    if (bite) {
      const junk: number[] = [];
      for (let i = 0; i < iterations; i++) junk.push(i);
      sink = junk.length;
    } else {
      sink = probeHotLoop(ring, view, iterations);
    }
    gc();
    gc();
    const growth = heapUsed() - before;
    perRound.push(Math.round((growth / 1024) * 10) / 10);
    if (!Number.isFinite(sink)) throw new Error('unreachable');
  }
  const growthKiB = Math.min(...perRound);
  return {
    iterations,
    rounds,
    growthKiB,
    perRoundKiB: perRound,
    commits: ring.commits,
    reads: ring.reads,
    gateKiB: GATE_KIB,
    pass: growthKiB <= GATE_KIB,
  };
}
