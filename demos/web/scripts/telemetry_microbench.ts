#!/usr/bin/env node
// telemetry_microbench.ts — per-cycle allocation + throughput measurement
// for the TS kernel's publish/claim telemetry regime.
//
// WHY EXISTS: the W6 feed bench (feed_gc_bench.ts) attributed Mode C's
// 8.9 KB/window produce-path cost to the kernel's BigInt telemetry by
// SUBTRACTION; the 2026-09-16 regime change (dual-i32 halves + Number step
// counters + dual-u32 canary, core/ts/weft.ts) closed it. This microbench
// measures the same thing DIRECTLY — one number per publish+claim cycle —
// so the regime's cost (and any future regression) is attributable without
// subtraction arithmetic.
//
// METHOD (feed_gc_bench M1/M4 discipline): M1 GC-free-window heapUsed
// deltas with a PerformanceObserver('gc') guard — windows containing any
// collection event are discarded, median reported; M4 a dedicated timing
// pass (hrtime allocations would pollute M1, so the passes are separated).
// The loop writes a fixed 64-byte frame through the public writer cursor
// (wBegin) — the same shape as the W6 Mode C path.
//
// Run (from demos/web, after `pnpm --filter @weft/core build`):
//   node --expose-gc scripts/telemetry_microbench.ts
// Not a CI gate — a committed evidence generator, in the fanout_bench.ts
// tradition (script + committed log beside it).

import { performance, PerformanceObserver } from 'node:perf_hooks';
import { writeFileSync, mkdirSync } from 'node:fs';

const PAYLOAD_MAX = 64;
const WINDOW = 1000; // publish+claim cycles per measurement window
const WINDOWS = 40;
const REPS = 5;
const TIMING_CYCLES = 2_000_000;

const lines: string[] = [];
function emit(s = ''): void {
  lines.push(s);
  console.log(s);
}

const heapUsed = (): number => process.memoryUsage().heapUsed;
const forceGC = (): boolean => {
  const g = (globalThis as { gc?: () => void }).gc;
  if (typeof g === 'function') {
    g();
    g();
    return true;
  }
  return false;
};

async function main(): Promise<void> {
  const { Weft } = await import('@weft/core');

  emit('TS KERNEL TELEMETRY MICROBENCH — publish/claim per-cycle cost');
  emit('================================================================');
  emit(`date: ${new Date().toISOString()}`);
  emit(`node: ${process.version} · platform: ${process.platform} ${process.arch}`);
  emit(`payload: ${PAYLOAD_MAX} B · ${WINDOW}-cycle windows · ${REPS} reps (medians)`);
  emit('method: M1 GC-free-window heapUsed deltas (observer-guarded); M4 dedicated');
  emit('        hrtime pass. Frame written via the public wBegin cursor.');
  emit('');

  // ---- M1: allocation per cycle ----
  const deltas: number[] = [];
  {
    const rec = { count: 0 };
    const obs = new PerformanceObserver((l) => {
      for (const e of l.getEntries()) rec.count++;
    });
    obs.observe({ entryTypes: ['gc'] });
    for (let rep = 0; rep < REPS; rep++) {
      const w = new Weft(PAYLOAD_MAX);
      for (let i = 0; i < 2000; i++) {
        w.wBegin()[0] = i & 0xff;
        w.publish(i, PAYLOAD_MAX);
        w.claim();
      }
      forceGC();
      let h0 = heapUsed();
      let c0 = rec.count;
      let pub = 0;
      for (let i = 0; i < WINDOW * WINDOWS; i++) {
        const c = w.wBegin();
        c[0] = i & 0xff;
        c[1] = (i >> 8) & 0xff;
        w.publish(i, PAYLOAD_MAX);
        w.claim();
        pub++;
        if (pub % WINDOW === 0) {
          const h1 = heapUsed();
          await new Promise<void>((r) => setImmediate(r)); // drain observer entries
          if (rec.count === c0) deltas.push(h1 - h0);
          h0 = heapUsed();
          c0 = rec.count;
        }
      }
    }
    obs.disconnect();
  }
  deltas.sort((a, b) => a - b);
  const medianWindow = deltas[Math.floor(deltas.length / 2)] ?? -1;
  emit(`M1 allocation: median ${medianWindow} B / ${WINDOW}-cycle window ` +
      `[${deltas.length}/${WINDOWS * REPS} valid windows] ` +
      `-> ~${(medianWindow / WINDOW).toFixed(2)} B per publish+claim cycle`);
  emit('  history: the BigInt regime measured ~152 B/cycle here (152,256 B median');
  emit('  window); the dual-i32 regime measures at sampling noise. The W6 feed');
  emit('  bench attributes the same delta by subtraction (Mode C produce path).');
  emit('');

  // ---- M4: throughput ----
  {
    const w = new Weft(PAYLOAD_MAX);
    for (let i = 0; i < 50_000; i++) {
      w.wBegin()[0] = i & 0xff;
      w.publish(i, PAYLOAD_MAX);
      w.claim();
    }
    const times: number[] = [];
    for (let rep = 0; rep < REPS; rep++) {
      const t0 = process.hrtime.bigint();
      for (let i = 0; i < TIMING_CYCLES; i++) {
        const c = w.wBegin();
        c[0] = i & 0xff;
        c[1] = (i >> 8) & 0xff;
        w.publish(i, PAYLOAD_MAX);
        w.claim();
      }
      times.push(Number(process.hrtime.bigint() - t0) / 1e6);
    }
    times.sort((a, b) => a - b);
    const med = times[Math.floor(times.length / 2)];
    emit(`M4 throughput: median ${med.toFixed(1)} ms / ${TIMING_CYCLES.toLocaleString()} cycles ` +
        `-> ${(TIMING_CYCLES / (med / 1000) / 1e6).toFixed(2)}M publish+claim cycles/s`);
    emit('  history: the BigInt regime measured ~347 ms (5.76M cycles/s) on this');
    emit('  machine class; the allocation-free regime ~229 ms (8.73M cycles/s).');
  }
  emit('');
  if (!forceGC()) emit('NOTE: --expose-gc was not provided; the forced-GC warmup ran without it.');

  const outDir = new URL('../evidence/', import.meta.url).pathname;
  mkdirSync(outDir, { recursive: true });
  const outFile = outDir + 'telemetry-microbench.log';
  writeFileSync(outFile, lines.join('\n') + '\n');
  console.log(`evidence log written: ${outFile}`);
  void performance;
}

void main();
