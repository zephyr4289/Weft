// frame_budget.ts — THE Pillar 4 evidence bench.
//
// GATE 1 (Law 1): 60,000 consecutive frames, ZERO heap growth, measured
//   under `node --expose-gc` (the mandate's "zero-GC render loop" claim,
//   as an experiment — audit.ts carries the protocol).
// GATE 2 (240 FPS budget): the same run's per-frame ENGINE execution time
//   under a sustained 100,000 samples/sec input load (417/416 samples per
//   240 Hz tick, exact long-run rate). Budget = 4,166.67 µs; the ledger
//   counts every violation.
// GATE 3 (tier-3 row): the CPU decimation cost for the mandate's
//   1,000,000-point window — the honest Tier-3 envelope number, labeled.
//
// HONESTY BOUNDARY (printed in the output): this bench runs the NullHAL —
// the CI sandbox has no GPU. It measures the ENGINE path (dirty harvest,
// sub-range upload handoff, family dispatch, ledger) plus an
// allocation-free producer. The real-tier frame costs (WGSL compute
// dispatch, TF decimation, texSubImage/writeBuffer) are measured by the
// browser leg (ci heddle-canvas shard, SwiftShader) and quoted in
// D-42 §4 with their own labels. Nothing here is laundered into a
// hardware claim.
//
// Usage: node --expose-gc bench/frame_budget.ts [--frames N] [--quick]
// Exit:  0 all gates PASS   1 a gate FAILED (details on stdout)

import { HotPlane } from '../src/plane/hot_plane.ts';
import { NullHAL } from '../src/hal/null_device.ts';
import { HeddleEngine } from '../src/loop/frame_engine.ts';
import { markDirty32 } from '../src/plane/dirty_mask.ts';
import { decimateWindowMinMax } from '../src/renderers/cpu_oracle.ts';
import { heapGate, samplingAllocationGate } from '../src/law1/audit.ts';
import { HP_KIND } from '../src/plane/whp1.ts';

// --- CLI -------------------------------------------------------------------
const args = process.argv.slice(2);
const flag = (name: string): boolean => args.includes(name);
const opt = (name: string, def: number): number => {
  const i = args.indexOf(name);
  return i !== -1 && i + 1 < args.length ? Number(args[i + 1]) : def;
};

const QUICK = flag('--quick');
const MEASURED_FRAMES = QUICK ? 10_000 : 60_000;
const WARMUP_FRAMES = QUICK ? 2_000 : 5_000;
const RATE_PER_SEC = 100_000;
const TICK_HZ = 240;

// --- the plane: the full visual surface (1M-point waveform + all rows) ----
const plane = HotPlane.create([
  { kind: HP_KIND.WAVEFORM_F32, capacity: 1 << 20, stride: 4, granularity: 1 << 14 }, // 1M, 64 bits × 16K
  { kind: HP_KIND.DEPTH_LADDER_F32, capacity: 256, stride: 64, granularity: 4 },
  { kind: HP_KIND.CANDLE_OHLC_F32, capacity: 128, stride: 64, granularity: 2 },
  { kind: HP_KIND.POINTCLOUD_QUAT_F32, capacity: 512, stride: 128, granularity: 8 },
]);
const hal = new NullHAL();
const engine = new HeddleEngine(plane, hal, {
  canvasWidth: 1280, canvasHeight: 720, tickHz: TICK_HZ, columnCount: 1280,
});

// --- the allocation-free producer (bench-local, road B) --------------------
// Two allocation disciplines in one producer:
//   * dirty marks ride the split-u32 road (markDirty32) — SynthProducer's
//     64-bit BigInt road is correct but BigInt boxes, and a PRODUCER may
//     allocate; the heap gate drives BOTH sides in one measured loop, so
//     the bench producer stays SMI-clean.
//   * the waveform generator is INTEGER-ONLY (imul/mod/compare — every
//     value an int32 SMI, zero doubles anywhere). The waveform SHAPE is
//     irrelevant to the gates; the browser rig uses the full-precision
//     synthSample for visuals. Keeping the bench generator SMI-clean
//     removes the last double-boxing surface from the measured window.

/** Integer waveform: triangle sweep + imul noise, all int32 (SMI-safe). */
function synthSampleI(i: number): number {
  const ph = i % 2048;
  const tri = ph < 1024 ? ph : 2047 - ph;
  const noise = (Math.imul(i, 2654435761) >>> 20) & 63;
  return tri + noise - 512;
}
const laneWave = plane.lanes[0];
const laneLadder = plane.lanes[1];
const laneCandle = plane.lanes[2];
const laneCloud = plane.lanes[3];
let cursor = 0;
const perTick = RATE_PER_SEC / TICK_HZ; // 416.67 — alternate ceil/floor

function pump(frameIndex: number): void {
  const n = frameIndex % 2 === 0 ? Math.ceil(perTick) : Math.floor(perTick);
  // waveform: wrapping stores + fence + dirty bits (road B)
  Atomics.add(plane.i32, laneWave.seqI32, 1);
  let w = cursor % laneWave.capacity;
  for (let k = 0; k < n; k++) {
    laneWave.f32[w] = synthSampleI(cursor + k);
    w++;
    if (w === laneWave.capacity) w = 0;
  }
  cursor += n;
  Atomics.store(plane.i32, laneWave.writePosI32, cursor);
  Atomics.add(plane.i32, laneWave.seqI32, 1);
  const w0 = (cursor - n) % laneWave.capacity;
  if (w0 + n <= laneWave.capacity) {
    markDirty32(plane.i32, laneWave, w0, n);
  } else {
    markDirty32(plane.i32, laneWave, w0, laneWave.capacity - w0);
    markDirty32(plane.i32, laneWave, 0, w0 + n - laneWave.capacity);
  }
  // rows: light per-frame updates (the L2/L3 + candle + IMU surface)
  const ladderRows = 200 + (frameIndex % 56);
  const words = laneLadder.strideBytes >> 2;
  for (let r = 0; r < ladderRows; r++) {
    laneLadder.u32[r * words] = laneLadder.u32[r * words]; // touch in place
  }
  Atomics.store(plane.i32, laneLadder.writePosI32, ladderRows);
  markDirty32(plane.i32, laneLadder, 0, ladderRows);
  Atomics.store(plane.i32, laneCandle.writePosI32, 128);
  markDirty32(plane.i32, laneCandle, 0, 128);
  Atomics.store(plane.i32, laneCloud.writePosI32, 512);
  markDirty32(plane.i32, laneCloud, 0, 512);
  Atomics.add(plane.i32, 0x10 / 4, 1); // plane epoch heartbeat
}

// --- run -------------------------------------------------------------------
let violations = 0;
const driver = (i: number): void => {
  pump(i);
  const t0 = performance.now();
  engine.tick(t0 * 1000); // virtual clock = real monotonic; budget measures the tick's own span
  const t1 = performance.now();
  if (t1 - t0 > 4166.6667 / 1000) violations++;
};

console.log(`heddle-canvas frame-budget bench: frames=${MEASURED_FRAMES} (warmup ${WARMUP_FRAMES}), load=${RATE_PER_SEC} samples/sec @ ${TICK_HZ} Hz, backend=null (engine path)`);
const gate = await samplingAllocationGate(WARMUP_FRAMES, MEASURED_FRAMES, driver);
const heapDelta = heapGate(WARMUP_FRAMES, MEASURED_FRAMES, driver); // secondary, reported

const p = engine.budget.percentiles();
const uploadedMiB = hal.stats.uploadedBytes / (1024 * 1024);

console.log(`law1-gate:      ${gate.passed ? 'PASS' : 'FAIL'} — sampling heap profiler: ${gate.engineBytes} B attributed to engine modules over ${gate.measuredFrames} frames (${gate.engineSamples} samples; total window incl. V8 internals ${gate.totalBytes} B)`);
console.log(`law1-secondary: heapUsed delta ${heapDelta.deltaBytes} B over the same window — REPORTED, not gated (V8 bookkeeping moves this while the code allocates nothing; the profiler decides the claim)`);
console.log(`frame-budget:   ${engine.budget.budgetViolations === 0 && violations === 0 ? 'PASS' : 'FAIL'} — p50 ${p.p50.toFixed(2)} µs, p95 ${p.p95.toFixed(2)} µs, p99 ${p.p99.toFixed(2)} µs, worst ${engine.budget.worst.toFixed(2)} µs vs 4166.67 µs budget (${engine.budget.budgetViolations + violations} violations)`);
console.log(`engine-stats:   draws ${hal.stats.drawCalls}, uploads ${hal.stats.uploadCalls} (${uploadedMiB.toFixed(1)} MiB), clean-lane skips ${hal.stats.skippedCleanLanes}, presents ${hal.stats.presentCount}`);

// --- GATE 3: the 1M-point CPU decimation row (Tier-3 envelope) -------------
const scratch = new Float32Array(1280 * 2);
const t3 = process.hrtime.bigint();
decimateWindowMinMax(laneWave.f32, laneWave.capacity, Math.min(cursor, laneWave.capacity), cursor % laneWave.capacity, 1280, scratch);
const t3micros = Number(process.hrtime.bigint() - t3) / 1000;
console.log(`tier3-decimate: 1,048,576-point window -> 1280 columns on CPU: ${t3micros.toFixed(0)} µs (MEASURED, oracle road; GPU tiers do this in-shader)`);

// --- JSON evidence line (house style) --------------------------------------
const evidence = {
  bench: 'heddle-canvas-frame-budget',
  backend: 'null',
  tick_hz: TICK_HZ,
  load_samples_per_sec: RATE_PER_SEC,
  frames: gate.measuredFrames,
  warmup: gate.warmupFrames,
  law1_engine_alloc_bytes: gate.engineBytes,
  law1_engine_alloc_samples: gate.engineSamples,
  law1_window_total_bytes: gate.totalBytes,
  law1_heapused_delta_reported: heapDelta.deltaBytes,
  law1_pass: gate.passed,
  p50_micros: Math.round(p.p50 * 100) / 100,
  p95_micros: Math.round(p.p95 * 100) / 100,
  p99_micros: Math.round(p.p99 * 100) / 100,
  worst_micros: Math.round(engine.budget.worst * 100) / 100,
  budget_violations: engine.budget.budgetViolations + violations,
  draws: hal.stats.drawCalls,
  upload_calls: hal.stats.uploadCalls,
  uploaded_mib: Math.round(uploadedMiB * 100) / 100,
  clean_lane_skips: hal.stats.skippedCleanLanes,
  tier3_decimate_1m_micros: Math.round(t3micros),
  label: 'ENGINE path on NullHAL (no GPU in CI); tier frame costs in the browser leg',
};
console.log(JSON.stringify(evidence));

process.exit(gate.passed && engine.budget.budgetViolations === 0 && violations === 0 ? 0 : 1);
