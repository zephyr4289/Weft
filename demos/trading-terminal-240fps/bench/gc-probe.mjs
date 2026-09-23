// bench/gc-probe.mjs — charter mandate probe: "< 32 KB total heap growth over
// 100,000 frames". One FRAME = full producer batch (16 lane publishes) +
// full consumer sweep (header + 16 lane reads + ring window + dirty scan) +
// scheduler pump with engine render. Run under --expose-gc (child process):
//   node --expose-gc bench/gc-probe.mjs [frames] [control]
// control mode: retained allocation per frame — MUST breach the gate (probe
// validity proof, no-silent-green).
import { writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { HotPlaneProducer, HotPlaneView, FrameScheduler, makeLaneOut, makeHeaderOut } from '../../../packages/heddle-core/src/index.js';
import { createOscilloscopeEngine, createCandlestickEngine, createOrderBookEngine, createAudioMeterEngine, make2DRecorder } from '../../../packages/react-heddle/src/visualizers.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const FRAMES = Number(process.argv[2] || 100_000);
const MODE = process.argv[3] || 'flight';
const GATE = 32 * 1024;
const gc = globalThis.gc;
if (typeof gc !== 'function') { console.error('FATAL: run with --expose-gc'); process.exit(2); }

const producer = HotPlaneProducer.create({ laneCount: 16, samplesPerLane: 256, tickHz: 240 });
const view = new HotPlaneView(producer.planeBuffer);
const laneOut = makeLaneOut();
const hdrOut = makeHeaderOut();
const changed = new Uint32Array(64);
const recent = new Float64Array(64);

// the four dashboard panels — real shipped engines over recorder canvases
const panels = [];
{
  const mk = (engine) => {
    const canvas = { width: 320, height: 160, ctx: make2DRecorder() };
    panels.push({ engine, state: engine.init(canvas, canvas.ctx, view), canvas });
  };
  mk(createOscilloscopeEngine({ lane: 0, window: 64 }));
  mk(createCandlestickEngine({ lane: 1, candles: 16, chunkSize: 4 }));
  mk(createOrderBookEngine({ bidLanes: [6, 7, 8], askLanes: [9, 10, 11] }));
  mk(createAudioMeterEngine({ lanes: [14, 15] }));
}
const scheduler = new FrameScheduler({ hz: 240, raf: null, now: () => 0 });
const NS = 1e9 / 240;

function runFlight(frames) {
  const values = new Float64Array(16);
  for (let f = 0; f < frames; f++) {
    // producer batch: 16 lane publishes (one full sweep)
    for (let l = 0; l < 16; l++) values[l] = 100 + ((f + l) & 255) * 0.25;
    producer.publishTick(values, 16, (f + 1) * NS);
    // consumer sweep
    view.readHeader(hdrOut);
    for (let l = 0; l < 16; l++) view.readLane(l, laneOut);
    view.readRecent(0, 64, recent);
    view.scanDirty(changed);
    // render frame at locked cadence
    scheduler.pump((f + 1) * NS);
    for (const p of panels) p.engine.render(p.state, scheduler.frameCtx, view);
  }
}

const retainedSink = []; // module-scope: must stay REACHABLE through the final gc()
function runControl(frames) {
  // RETAIN EVERY FRAME: a small retained pool (≤ ~10k objects) merely refills
  // pages reclaimed from warmup garbage — heapUsed shows no net growth and the
  // control would lie. 100k retained objects force real page commits (~11 MB).
  // (Also: the sink must OUTLIVE the function — a local dies before gc().)
  const values = new Float64Array(16);
  for (let f = 0; f < frames; f++) {
    retainedSink.push({ f, pad: [f, f, f] });
    for (let l = 0; l < 16; l++) values[l] = 100 + ((f + l) & 255) * 0.25;
    producer.publishTick(values, 16, (f + 1) * NS);
    for (let l = 0; l < 16; l++) view.readLane(l, laneOut);
    scheduler.pump((f + 1) * NS);
  }
  if (retainedSink.length === -1) console.error('unreachable');
}

// warmup (JIT tiering) then measure
runFlight(2000);
gc();
const a = process.memoryUsage().heapUsed;
if (MODE === 'control') runControl(FRAMES); else runFlight(FRAMES);
gc();
const b = process.memoryUsage().heapUsed;
const growth = b - a;
const gate = MODE === 'control' ? GATE : GATE;
const pass = MODE === 'control' ? growth >= gate : growth < gate;
const verdict = { mode: MODE, frames: FRAMES, heapGrowthBytes: growth, gateBytes: gate, pass };
console.log(JSON.stringify(verdict));
if (process.argv.includes('--evidence')) {
  const dir = join(HERE, '..', 'evidence');
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, `gc-probe-${MODE}-${FRAMES}.json`), JSON.stringify(verdict, null, 2) + '\n');
}
process.exit(pass ? 0 : 1);
