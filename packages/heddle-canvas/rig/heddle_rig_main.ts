// heddle_rig_main.ts — the in-browser leg of the heddle-canvas shard.
//
// Runs in REAL Chromium (headless, SwiftShader-class rasterizers) under
// COOP/COEP (the SAB requirement — crossOriginIsolated is asserted before
// anything else, the Series-7 browser-sab lesson).
//
// WHAT IT MEASURES (and what it refuses to claim):
//   1. crossOriginIsolated === true (SAB contract).
//   2. The tier ladder on the REAL platform: WebGPU adapter / WebGL2
//      context / Canvas2D — each present tier gets the SAME scenario.
//   3. CROSS-TIER ==-GATE: the min/max decimation of a known waveform,
//      computed by the WGSL compute pass and by the GLSL transform-
//      feedback pass, read back and hashed (FNV-1a over f32 bits) —
//      must equal the CPU oracle hash. Bit-exact, not eyeballed.
//   4. DIRTY-SKIP: a clean frame issues ZERO uploads and draws.
//   5. Frame cost: per-tier average tick time — labeled MEASURED on
//      SwiftShader (software); hardware present-rate stays a hardware
//      claim (D-42 section 6).
//
// The page prints ONE JSON verdict line ("HEDDLE-RIG-VERDICT ") that the
// shard runner parses and gates on.

import {
  HotPlane,
  SynthProducer,
  HeddleEngine,
  NullHAL,
  acquireWebGL2,
  acquireWebGPU,
  WebGL2HAL,
  WebGPUHAL,
  Canvas2DHAL,
  walkTierLadder,
  decimateHash,
  HP_KIND,
} from '../src/index.ts';
import type { RenderHAL, LaneView } from '../src/index.ts';

const CFG = { canvasWidth: 640, canvasHeight: 360, tickHz: 240, columnCount: 640 };
const CAP = 65536; // waveform capacity — SwiftShader-sane

interface TierResult {
  tier: string;
  available: boolean;
  detail: string;
  oracle_match?: boolean;
  gpu_hash?: number;
  cpu_hash?: number;
  clean_skip_uploads?: number;
  clean_skip_draws?: number;
  avg_tick_micros?: number;
  frames?: number;
  error?: string;
}

const results: TierResult[] = [];

function makePlane(): { plane: HotPlane; lane: LaneView } {
  const plane = HotPlane.create([
    { kind: HP_KIND.WAVEFORM_F32, capacity: CAP, stride: 4, granularity: CAP / 64 },
  ]);
  return { plane, lane: plane.lanes[0] };
}

function publishAndHash(synth: SynthProducer, plane: HotPlane, lane: LaneView): number {
  synth.pumpWaveform(50000, 7.25); // deterministic fill (window wraps at 65536)
  const writePos = 50000 + 0; // total published
  const visible = Math.min(writePos, CAP);
  const scratch = new Float32Array(CFG.columnCount * 2);
  return decimateHash(lane.f32, CAP, visible, writePos % CAP, CFG.columnCount, scratch);
}

async function runScenario(
  name: string,
  hal: RenderHAL,
  readback: (lane: LaneView, out: Float32Array) => Promise<Float32Array> | Float32Array,
  opts?: { skipOracle?: boolean },
): Promise<void> {
  const r: TierResult = { tier: name, available: true, detail: 'ran' };
  try {
    const { plane, lane } = makePlane();
    const engine = new HeddleEngine(plane, hal, CFG);
    const synth = new SynthProducer(plane);
    const cpuHash = publishAndHash(synth, plane, lane);

    // One dirty frame: upload + compute decimation.
    const t0 = performance.now();
    engine.tick(t0 * 1000);
    // give the GPU queue a beat (SwiftShader executes synchronously, but
    // WebGPU's submit is async by contract)
    if (opts?.skipOracle === true) {
      await readback(lane, new Float32Array(CFG.columnCount * 2)); // drain seam anyway
      r.detail = 'ran (oracle gate N/A — backend elides GPU work)';
    } else {
      const mm = await readback(lane, new Float32Array(CFG.columnCount * 2));
      const gpuHash = hashWords(mm);
      r.oracle_match = gpuHash === cpuHash;
      r.gpu_hash = gpuHash;
      r.cpu_hash = cpuHash;
    }

    // Clean frame: NOTHING moves.
    const bindsBefore = hal.stats.bindCount;
    const drawsBefore = hal.stats.drawCalls;
    engine.tick(performance.now() * 1000);
    r.clean_skip_uploads = hal.stats.bindCount - bindsBefore;
    r.clean_skip_draws = hal.stats.drawCalls - drawsBefore;

    // Sustained frames: average tick cost (SwiftShader-labeled).
    const N = 240;
    const t1 = performance.now();
    for (let i = 0; i < N; i++) {
      synth.pumpWaveform(i % 2 === 0 ? 417 : 416, i / 240);
      engine.tick(performance.now() * 1000);
    }
    await readback(lane, new Float32Array(CFG.columnCount * 2)); // drain
    r.avg_tick_micros = Math.round(((performance.now() - t1) / N) * 1000 * 100) / 100;
    r.frames = N;
  } catch (e) {
    r.error = String(e);
  }
  results.push(r);
}

function hashWords(f32: Float32Array): number {
  // FNV-1a over the f32 bits (same as cpu_oracle.bitsOf + fnv1a32).
  const u32 = new Uint32Array(f32.buffer, f32.byteOffset, f32.length);
  let h = 0x811c9dc5;
  for (let i = 0; i < u32.length; i++) {
    h ^= u32[i];
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}

async function main(): Promise<void> {
  const iso = typeof crossOriginIsolated !== 'undefined' && crossOriginIsolated === true;
  console.log(`HEDDLE-RIG-INFO crossOriginIsolated=${iso}`);

  const ladder = await walkTierLadder({
    probeWebGPU: async () => {
      const g = (navigator as unknown as { gpu?: { requestAdapter(): Promise<object | null> } }).gpu;
      if (g === undefined) return null;
      try {
        const adapter = await g.requestAdapter();
        return adapter as object | null;
      } catch {
        return null;
      }
    },
    probeWebGL2: () => {
      const c = document.createElement('canvas');
      return c.getContext('webgl2', { alpha: false, antialias: false });
    },
    probeCanvas2D: () => {
      const c = document.createElement('canvas');
      c.width = CFG.canvasWidth;
      c.height = CFG.canvasHeight;
      return c.getContext('2d');
    },
  });
  for (const refusal of ladder.refusals) {
    results.push({ tier: refusal.from, available: false, detail: refusal.detail });
  }

  // --- WebGPU (Tier 1) ------------------------------------------------------
  try {
    const g = (navigator as unknown as { gpu?: { requestAdapter(): Promise<object | null> } }).gpu;
    if (g !== undefined) {
      const acquired = await acquireWebGPU(g as never);
      if (acquired !== null) {
        const hal = new WebGPUHAL(acquired.device as never, acquired.machine);
        await runScenario('webgpu', hal, async (lane, out) => {
          const got = await (hal as WebGPUHAL).readBackMinmax(lane);
          out.set(got);
          return out;
        });
      } else {
        results.push({ tier: 'webgpu', available: false, detail: 'adapter refused' });
      }
    }
  } catch (e) {
    results.push({ tier: 'webgpu', available: false, detail: String(e) });
  }

  // --- WebGL2 (Tier 2) ------------------------------------------------------
  try {
    const canvas = document.createElement('canvas');
    canvas.width = CFG.canvasWidth;
    canvas.height = CFG.canvasHeight;
    const acq = acquireWebGL2(canvas);
    if (acq !== null) {
      const hal = new WebGL2HAL(acq.gl as never, acq.context);
      await runScenario('webgl2', hal, (lane, out) => {
        (hal as WebGL2HAL).readBackMinmax(lane, out);
        return out;
      });
    }
  } catch (e) {
    results.push({ tier: 'webgl2', available: false, detail: String(e) });
  }

  // --- Canvas2D (Tier 3) ----------------------------------------------------
  try {
    const canvas = document.createElement('canvas');
    canvas.width = CFG.canvasWidth;
    canvas.height = CFG.canvasHeight;
    const ctx = canvas.getContext('2d');
    if (ctx !== null) {
      const hal = new Canvas2DHAL(ctx as never);
      await runScenario('canvas2d', hal, (lane, out) => {
        // The raster's decimation cache, read back through the same seam
        // contract as the GPU tiers — the oracle gate then verifies the
        // WINDOW wiring against a fresh CPU decimation of the live lane.
        (hal as Canvas2DHAL).readBackMinmax(lane, out);
        return out;
      });
    }
  } catch (e) {
    results.push({ tier: 'canvas2d', available: false, detail: String(e) });
  }

  // --- NullHAL (the always-there floor) ------------------------------------
  // The NullHAL ELIDES the GPU work — there is no decimation to read back,
  // so the oracle gate does not apply to it (contract battery + Law-1 leg
  // cover it). It runs the scenario to prove the frame loop holds on the
  // browser host too.
  await runScenario('null', new NullHAL(), (lane, out) => {
    out.fill(0);
    return out;
  }, { skipOracle: true });

  const verdict = {
    crossOriginIsolated: iso,
    tiers: results,
    pass: iso && results.every(
      (t) =>
        // unavailable tiers are honest refusals, not failures
        (t.available !== true) ||
        (t.error === undefined
          && (t.oracle_match === undefined || t.oracle_match === true)
          && t.clean_skip_uploads === 0
          && t.clean_skip_draws === 0),
    ),
  };
  console.log(`HEDDLE-RIG-VERDICT ${JSON.stringify(verdict)}`);
}

void main();
