// probes/alloc-probe.mjs — Law 1 evidence: heap growth over 100k hot-path ops.
//
// Run as a child so --expose-gc is guaranteed:
//   node --expose-gc probes/alloc-probe.mjs <mode>
//
// Modes: producer | consumer | scheduler | control
//   producer  — 100k publishTick(16 lanes)  ⇒ 1.6M lane publishes
//   consumer  — 100k × (readHeader + 16×readLane + readRecent + scanDirty)
//   scheduler — 100k scheduler pumps at 240 Hz cadence (empty render cb)
//   control   — NEGATIVE control: deliberately allocates; must show growth
//
// Gate: < 32 KiB heap growth per 100k ops (mandate: D-43 charter). The probe
// FAILS (exit 1) if a real mode grows ≥ gate OR if the control does NOT grow
// (a probe that cannot detect allocation is not evidence).
import { HotPlaneProducer, HotPlaneView, FrameScheduler, makeLaneOut, makeHeaderOut } from '../src/index.js';

const ITERS = 100_000;
const GATE = 32 * 1024;
const mode = process.argv[2];
if (!mode) { console.error('usage: node --expose-gc probes/alloc-probe.mjs <mode>'); process.exit(2); }
const gc = globalThis.gc;
if (typeof gc !== 'function') { console.error('FATAL: run with --expose-gc'); process.exit(2); }

const NOW0 = 1e12; // fixed monotonic start (ns)
const NS_PER_TICK = 1e9 / 240;

function measure(fn) {
  fn(2000, true); // warmup (JIT tiering)
  gc();
  const a = process.memoryUsage().heapUsed;
  fn(ITERS, false);
  gc();
  const b = process.memoryUsage().heapUsed;
  return b - a;
}

let growth;
if (mode === 'producer') {
  const p = HotPlaneProducer.create({ laneCount: 16, samplesPerLane: 256, tickHz: 240 });
  const values = new Float64Array(16);
  growth = measure((iters) => {
    for (let i = 0; i < iters; i++) {
      for (let l = 0; l < 16; l++) values[l] = 100 + ((i + l) & 1023) * 0.5;
      p.publishTick(values, 16, NOW0 + i * NS_PER_TICK);
    }
  });
} else if (mode === 'consumer') {
  const p = HotPlaneProducer.create({ laneCount: 16, samplesPerLane: 256, tickHz: 240 });
  const values = new Float64Array(16);
  values[0] = 1; p.publishTick(values, 16, NOW0); // activate lanes
  const v = new HotPlaneView(p.planeBuffer);
  const laneOut = makeLaneOut();
  const hdrOut = makeHeaderOut();
  const recent = new Float64Array(64);
  const changed = new Uint32Array(64);
  growth = measure((iters) => {
    for (let i = 0; i < iters; i++) {
      v.readHeader(hdrOut);
      for (let l = 0; l < 16; l++) v.readLane(l, laneOut);
      v.readRecent(i & 15, 64, recent);
      v.scanDirty(changed);
    }
  });
} else if (mode === 'scheduler') {
  const s = new FrameScheduler({ hz: 240, raf: null, now: () => 0 });
  s.start(() => { /* empty render: the probe measures the scheduler itself */ });
  growth = measure((iters) => {
    for (let i = 0; i < iters; i++) s.pump(NOW0 + i * NS_PER_TICK);
  });
  s.stop();
} else if (mode === 'control') {
  // NEGATIVE control: RETAINED allocations whose set GROWS throughout the
  // measured phase (escape-analysis-proof AND warmup-proof — a fixed-size
  // retained pool saturates during warmup and shows zero net growth).
  growth = measure((iters, isWarmup) => {
    if (isWarmup || !globalThis.__keep) globalThis.__keep = [];
    for (let i = 0; i < iters; i++) {
      if ((i & 7) === 0) globalThis.__keep.push({ a: i, b: i, c: i, d: i });
    }
  });
  if (globalThis.__keep.length === -1) console.error('unreachable');
} else {
  console.error(`unknown mode ${mode}`);
  process.exit(2);
}

const verdict = mode === 'control' ? growth >= GATE : growth < GATE;
console.log(JSON.stringify({ mode, iters: ITERS, heapGrowthBytes: growth, gateBytes: GATE, pass: verdict }));
process.exit(verdict ? 0 : 1);
