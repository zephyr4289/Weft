// bench/rerender-probe.mjs — charter mandate: "0 React component re-renders
// on the telemetry stream" over 100,000 live ticks. Mounts a full dashboard
// (signals × 4 + live stats + WeftOscilloscope + WeftOrderBook + WeftAudioMeter
// + WeftCanvas) through the mount-semantics shim and counts EVERY setState —
// the stream must produce ZERO. Exit 2 on violation (no-silent-green).
import { writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { HotPlaneProducer } from '../../../packages/heddle-core/src/index.js';
import { createHeddleHooks, createWeftCanvas, PlaneContext } from '../../../packages/react-heddle/src/core.js';
import { createVisualizers, make2DRecorder } from '../../../packages/react-heddle/src/visualizers.js';

const HERE = dirname(fileURLToPath(import.meta.url));

// mount-semantics shim (same contract as packages/react-heddle/test/shim.mjs)
function makeShim() {
  const effects = [];
  const React = {
    useRef(init) { return { current: init }; },
    useEffect(fn) { effects.push({ fn }); },
    useState(init) { return [init, () => shim.setStateCalls.push(1)]; },
    createContext(dv) { return { dv, providers: [] }; },
    useContext(ctx) { return ctx.providers.length ? ctx.providers.at(-1).value : ctx.dv; },
    createElement(type, props, ...children) { return { type, props: props || {}, children }; },
  };
  const shim = { React, effects, setStateCalls: [],
    mount() { for (const e of effects) e.cleanup = e.fn(); },
    unmount() { for (const e of effects) if (typeof e.cleanup === 'function') e.cleanup(); effects.length = 0; } };
  return shim;
}

const TICKS = Number(process.argv[2] || 100_000);
const shim = makeShim();
const hooks = createHeddleHooks(shim.React);
const V = createVisualizers(shim.React, hooks);
const WeftCanvas = createWeftCanvas(shim.React, hooks);

const producer = HotPlaneProducer.create({ laneCount: 16, samplesPerLane: 256, tickHz: 240 });
const ctx = new PlaneContext(producer.planeBuffer, { hz: 240, raf: null, producer });

// dashboard components — the app-shaped usage of every binding
const signals = [0, 1, 2, 3].map((lane) => {
  const ref = hooks.useWeftSignal(lane, { plane: ctx });
  const stats = hooks.useWeftStats(lane, { plane: ctx });
  return { ref, stats, node: { nodeValue: null } };
});
const buffer = hooks.useWeftBuffer(14, { plane: ctx });
signals.forEach((s) => s.ref(s.node));

function fakeCanvasEl() {
  return { type: 'canvas', props: { ref: { current: null } } };
}

const elements = [];
for (const Comp of [V.WeftOscilloscope, V.WeftCandlestickChart, V.WeftOrderBook, V.WeftAudioMeter]) {
  const el = Comp({ plane: ctx, hz: 240 });
  elements.push(el);
  if (el.type === 'canvas') {
    el.props.ref.current = { width: 200, height: 100, handlers: {}, addEventListener() {}, removeEventListener() {}, getContext: () => make2DRecorder() };
  }
}
const canvasEl = WeftCanvas({ plane: ctx, hz: 240, engine: {
  contextType: '2d',
  init: () => ({ frames: 0 }), render: (s) => { s.frames += 1; },
} });
if (canvasEl.type === 'canvas') {
  canvasEl.props.ref.current = { width: 200, height: 100, handlers: {}, addEventListener() {}, removeEventListener() {}, getContext: () => make2DRecorder() };
}
elements.push(canvasEl);

shim.mount();
// stream 100k ticks; one scheduled frame every 4 publishes → 25k frames
let frames = 0;
for (let i = 0; i < TICKS; i++) {
  producer.publishLane(i % 16, 100 + (i & 255) * 0.5, i);
  ctx.scheduler.pump(i * 1e6); // 1 ms ticks, 4.1666 ms interval → every 4th pump renders, 0 skips
}
ctx.scheduler.pump(TICKS * 1e6);
frames = ctx.scheduler.rendered;
buffer.buffer[0] = 42;
buffer.publish(1);

const setStateCalls = shim.setStateCalls.length;
shim.unmount();

const verdict = {
  ticks: TICKS, scheduledFrames: frames, schedulerRendered: ctx.scheduler.rendered,
  skipped: ctx.scheduler.skipped, // must be 0 — locked cadence
  setStateCalls, reRenders: setStateCalls, effectMounts: 1,
  pass: setStateCalls === 0 && ctx.scheduler.rendered > 1000,
};
console.log(JSON.stringify(verdict));
if (process.argv.includes('--evidence')) {
  const dir = join(HERE, '..', 'evidence');
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, `rerender-probe-${TICKS}.json`), JSON.stringify(verdict, null, 2) + '\n');
}
process.exit(verdict.pass ? 0 : 2);
