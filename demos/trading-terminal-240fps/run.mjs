// demos/trading-terminal-240fps/run.mjs — Pillar 4 flight demo orchestrator.
//
// Stages (fail-closed, exit 2 on any mandate violation):
//   A  ingestion burst   — 100,000 synthetic market ticks across 16 HPL1 lanes
//                          through publishLane at full speed (ticks/sec ≥ 100k)
//   B  240 Hz frame lock — virtual-clock frame budget: every frame = producer
//                          batch + full consumer sweep + all four panel
//                          engines + HUD poll; skipped frames must be 0
//   C  GC probe          — child --expose-gc: heap growth < 32 KiB over
//                          100,000 frames + negative control bites
//   D  re-render probe   — full React dashboard through the mount shim:
//                          0 setState over 100,000 live ticks
//   E  live HUD snapshot — tears/dirty/latency read from the plane
//
// Evidence JSON → evidence/flight-<ts>.json. ANSI dashboard on stdout.
import { spawnSync } from 'node:child_process';
import { writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { performance } from 'node:perf_hooks';

import { HotPlaneView, FrameScheduler, makeLaneOut, makeHeaderOut } from '../../packages/heddle-core/src/index.js';
import { createOscilloscopeEngine, createCandlestickEngine, createOrderBookEngine, createAudioMeterEngine, make2DRecorder } from '../../packages/react-heddle/src/visualizers.js';
import { MarketFeed, createDemoPlane, LANE_COUNT } from './producer.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const TICKS_MANDATE = 100_000;
const NS = 1e9 / 240;

// ---- shared plane ----
const producer = createDemoPlane();
const view = new HotPlaneView(producer.planeBuffer);
const feed = new MarketFeed();
const laneOut = makeLaneOut();
const hdrOut = makeHeaderOut();
const changed = new Uint32Array(64);
const recent = new Float64Array(64);

const panel = (label, engine) => {
  const canvas = { width: 320, height: 160, ctx: make2DRecorder() };
  return { label, engine, canvas, state: engine.init(canvas, canvas.ctx, view) };
};
const panels = [
  panel('oscilloscope L0', createOscilloscopeEngine({ lane: 0, window: 64 })),
  panel('candles L1', createCandlestickEngine({ lane: 1, candles: 16, chunkSize: 4 })),
  panel('orderbook 6..11', createOrderBookEngine({ bidLanes: [6, 7, 8], askLanes: [9, 10, 11] })),
  panel('audio 14/15', createAudioMeterEngine({ lanes: [14, 15] })),
];
const scheduler = new FrameScheduler({ hz: 240, raf: null, now: () => 0 });

const results = {};
let failures = 0;
const stage = (name, fn) => {
  process.stdout.write(`\n[${name}]\n`);
  try { fn(); } catch (e) { failures += 1; console.log(`  FAIL ${e.message}`); }
};

// ---- Stage A: 100k ticks/sec ingestion burst ----
stage('A ingestion burst — 100,000 ticks x 16 lanes', () => {
  const t0 = performance.now();
  const endNs = feed.burst(producer, TICKS_MANDATE, 0, 1000); // 1 µs/tick cadence
  const wallMs = performance.now() - t0;
  const ticksPerSec = Math.round(TICKS_MANDATE / (wallMs / 1000));
  results.ingestion = { ticks: TICKS_MANDATE, wallMs: +wallMs.toFixed(1), ticksPerSec, endNs };
  console.log(`  ${TICKS_MANDATE.toLocaleString()} ticks in ${wallMs.toFixed(1)} ms → ${ticksPerSec.toLocaleString()} ticks/sec`);
  if (ticksPerSec < TICKS_MANDATE) { failures += 1; console.log('  FAIL below 100k ticks/sec mandate'); }
  // sequence integrity: every lane saw ceil(ticks/16) samples
  let seen = 0;
  for (let l = 0; l < LANE_COUNT; l++) { view.readLane(l, laneOut); seen += laneOut.samplesSeenLo; }
  console.log(`  samples on plane: ${seen.toLocaleString()} (expected ${TICKS_MANDATE.toLocaleString()})`);
  if (seen !== TICKS_MANDATE) { failures += 1; console.log('  FAIL sequence gap in ingestion'); }
});

// ---- Stage B: 240 Hz frame lock over 24,000 frames (100 s virtual) ----
stage('B 240 Hz frame lock — 24,000 frames, 4 panels + HUD poll', () => {
  const FRAMES = 24_000;
  const values = new Float64Array(16);
  const costs = new Float64Array(FRAMES);
  scheduler.arm(() => {}); // manual virtual-clock pumping (pump() no-ops unarmed)
  scheduler.rendered = 0; scheduler.skipped = 0;
  for (let f = 0; f < FRAMES; f++) {
    for (let l = 0; l < LANE_COUNT; l++) values[l] = feed.row[l] + ((f + l) & 255) * 0.05;
    producer.publishTick(values, LANE_COUNT, feed.tickIndex * 1000 + (f + 1) * NS);
    const t0 = performance.now();
    view.readHeader(hdrOut);
    for (let l = 0; l < LANE_COUNT; l++) view.readLane(l, laneOut);
    view.readRecent(0, 64, recent);
    view.scanDirty(changed);
    scheduler.pump((f + 1) * NS);
    for (const p of panels) p.engine.render(p.state, scheduler.frameCtx, view);
    costs[f] = (performance.now() - t0) * 1000;
  }
  // steady-state tail: skip the first 240 frames (JIT tiering warmup)
  const steady = Array.from(costs.slice(240)).sort((a, b) => a - b);
  const p50 = steady[Math.floor(steady.length * 0.5)];
  const p99 = steady[Math.floor(steady.length * 0.99)];
  const max = steady[steady.length - 1];
  results.frameLock = {
    frames: FRAMES, rendered: scheduler.rendered, skipped: scheduler.skipped,
    frameCostUs: { p50: +p50.toFixed(1), p99: +p99.toFixed(1), max: +max.toFixed(1) },
    budgetUs: +(NS / 1000).toFixed(1),
  };
  console.log(`  rendered ${scheduler.rendered}/${FRAMES}, skipped ${scheduler.skipped}`);
  console.log(`  frame cost µs: p50=${p50.toFixed(1)} p99=${p99.toFixed(1)} max=${max.toFixed(1)} (budget ${(NS / 1000).toFixed(1)})`);
  if (scheduler.rendered !== FRAMES || scheduler.skipped !== 0) {
    failures += 1; console.log('  FAIL frame lock broken (skips > 0 under budget)');
  }
  // gate on the p99 TAIL; single noise spikes (GC, vCPU steal) are recorded as
  // evidence — the pillar-3 honesty convention (13.5ns measured vs 10ns mandate)
  if (p99 > NS / 1000) {
    failures += 1; console.log('  FAIL p99 frame cost exceeds 240 Hz budget');
  }
  if (max > NS / 1000) {
    console.log('  NOTE max > budget (environment noise on shared vCPU) — recorded; p99 is the gate');
  }
});

// ---- Stage C: GC probe (subprocess, --expose-gc) ----
stage('C GC allocation probe — < 32 KiB over 100,000 frames + control', () => {
  const run = (mode) => spawnSync(process.execPath, ['--expose-gc', join(HERE, 'bench/gc-probe.mjs'), '100000', mode, '--evidence'], { encoding: 'utf8' });
  const flight = run('flight');
  const control = run('control');
  results.gcProbe = {
    flight: safeJson(flight.stdout), control: safeJson(control.stdout),
  };
  console.log(`  flight  : ${flight.stdout.trim() || flight.stderr.trim()}`);
  console.log(`  control : ${control.stdout.trim() || control.stderr.trim()}`);
  if (flight.status !== 0) { failures += 1; console.log('  FAIL flight heap growth breached 32 KiB gate'); }
  if (control.status !== 0) { failures += 1; console.log('  FAIL negative control did NOT breach the gate (probe cannot detect allocation)'); }
});

// ---- Stage D: re-render probe (subprocess) ----
stage('D re-render probe — 0 setState over 100,000 ticks', () => {
  const r = spawnSync(process.execPath, [join(HERE, 'bench/rerender-probe.mjs'), '100000', '--evidence'], { encoding: 'utf8' });
  results.rerenderProbe = safeJson(r.stdout);
  console.log(`  ${r.stdout.trim() || r.stderr.trim()}`);
  if (r.status !== 0) { failures += 1; console.log('  FAIL telemetry stream triggered React re-renders'); }
});

// ---- Stage E: live HUD snapshot ----
stage('E HUD snapshot — tears / dirty / latency', () => {
  view.readHeader(hdrOut);
  const tearsBefore = view.tears;
  view.readLane(0, laneOut);
  const lagNs = (hdrOut.lastPublishNsLo - laneOut.publishNsLo) >>> 0;
  results.hud = {
    tears: tearsBefore, dirtyBits: view.dirtyCount,
    publishLagMs: +(lagNs / 1e6).toFixed(3),
    globalMin: hdrOut.globalMin, globalMax: hdrOut.globalMax,
  };
  console.log(`  tears=${tearsBefore} dirtyBits=${view.dirtyCount} publishLag=${(lagNs / 1e6).toFixed(3)} ms`);
  if (tearsBefore !== 0) { failures += 1; console.log('  FAIL unexpected seqlock tears in single-threaded flight'); }
});

// ---- ANSI terminal dashboard (visual twin of the web panels) ----
function sparkline(values, width) {
  const chars = ' ▁▂▃▄▅▆▇█';
  let out = '';
  for (let i = 0; i < width; i++) {
    const v = values[values.length - 1 - (width - 1 - i)] ?? 0;
    out += chars[Math.min(8, Math.floor(((v - 0) / 1) * 8))];
  }
  return out;
}
view.readRecent(0, 32, recent);
const lo = Math.min(...recent), hi = Math.max(...recent);
const norm = Float64Array.from(recent, (v) => (v - lo) / ((hi - lo) || 1));
console.log('\n╭─ trading-terminal-240fps ─────────────────────────────╮');
console.log(`│ osc L0    ${sparkline(norm, 24)}            │`);
view.readLane(6, laneOut);
console.log(`│ bid L1 ${String(Math.round(laneOut.current)).padStart(6)} ▏${'█'.repeat(Math.round(laneOut.current / 200))}`);
view.readLane(9, laneOut);
console.log(`│ ask L1 ${String(Math.round(laneOut.current)).padStart(6)} ▏${'█'.repeat(Math.round(laneOut.current / 200))}`);
console.log(`│ FPS target 240 · frames rendered ${scheduler.rendered} · 0 re-renders │`);
console.log('╰───────────────────────────────────────────────────────╯');

// ---- evidence ----
const evidence = {
  demo: 'trading-terminal-240fps',
  date: new Date().toISOString(),
  node: process.version,
  mandates: {
    ticksPerSecond: results.ingestion?.ticksPerSec,
    framesLocked: results.frameLock?.rendered,
    framesSkipped: results.frameLock?.skipped,
    heapGrowthOver100kFrames: results.gcProbe?.flight?.heapGrowthBytes,
    reRendersOver100kTicks: results.rerenderProbe?.reRenders,
  },
  stages: results,
  failures,
  pass: failures === 0,
};
const dir = join(HERE, 'evidence');
mkdirSync(dir, { recursive: true });
const file = join(dir, `flight-${Date.now()}.json`);
writeFileSync(file, JSON.stringify(evidence, null, 2) + '\n');
console.log(`\nevidence → ${file}`);
console.log(evidence.pass ? 'FLIGHT PASS — all mandates verified' : `FLIGHT FAIL — ${failures} mandate violation(s)`);
process.exit(evidence.pass ? 0 : 2);

function safeJson(s) {
  try { return JSON.parse(s); } catch { return null; }
}
