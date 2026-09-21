// test/visualizers.test.mjs — the four drop-in visualizers: engine contract,
// zero-setState component wiring, OHLC derivation correctness, peak-hold math.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { HotPlaneProducer, HotPlaneView } from '../../heddle-core/src/index.js';
import { createHeddleHooks, PlaneContext } from '../src/core.js';
import {
  createVisualizers, createOscilloscopeEngine, createCandlestickEngine,
  createOrderBookEngine, createAudioMeterEngine, make2DRecorder,
} from '../src/visualizers.js';
import { makeShim } from './shim.mjs';

function rig({ laneCount = 8, samplesPerLane = 256, hz = 250 } = {}) {
  const shim = makeShim();
  const hooks = createHeddleHooks(shim.React);
  const producer = HotPlaneProducer.create({ laneCount, samplesPerLane, tickHz: hz });
  const ctx = new PlaneContext(producer.planeBuffer, { hz, raf: null, producer });
  const view = new HotPlaneView(producer.planeBuffer);
  return { shim, hooks, producer, ctx, view };
}

function fakeCanvas() {
  return { width: 320, height: 160, ctx: make2DRecorder(), getContext: () => null };
}

test('Oscilloscope engine: newest-first window, full wave path drawn per frame', () => {
  const { producer, view } = rig();
  for (let i = 0; i < 300; i++) producer.publishLane(0, 50 + Math.sin(i / 5) * 10, i);
  const canvas = fakeCanvas();
  const engine = createOscilloscopeEngine({ lane: 0, window: 256 });
  const state = engine.init(canvas, canvas.ctx, view);
  engine.render(state, { frame: 1, nowNs: 0, skipped: 0, lateNs: 0 }, state.view);
  const c = canvas.ctx.calls;
  assert.ok((c.lineTo || 0) >= 250, `wave drawn: ${c.lineTo} lineTo calls`);
  assert.ok((c.stroke || 0) >= 2, 'grid + wave stroked');
  assert.equal(state.samples[0], 50 + Math.sin(299 / 5) * 10, 'sample[0] is the NEWEST value');
});

test('Candlestick engine: OHLC derivation is exact on a monotonic ramp', () => {
  const { producer, view } = rig();
  for (let i = 0; i < 64; i++) producer.publishLane(0, i, i); // ramp 0..63
  const canvas = fakeCanvas();
  const engine = createCandlestickEngine({ lane: 0, candles: 8, chunkSize: 8 });
  const state = engine.init(canvas, canvas.ctx, view);
  engine.render(state, { frame: 1, nowNs: 0, skipped: 0, lateNs: 0 }, state.view);
  // newest chunk = samples 56..63 → open 56, close 63, high 63, low 56
  assert.equal(state.ohlc[0], 56, 'candle 0 open (oldest of newest chunk)');
  assert.equal(state.ohlc[1], 63, 'candle 0 high');
  assert.equal(state.ohlc[2], 56, 'candle 0 low');
  assert.equal(state.ohlc[3], 63, 'candle 0 close');
  assert.ok(canvas.ctx.calls.fillRect >= 8, '8 candle bodies drawn');
  // rising close → last body fillStyle is the up color
  assert.equal(canvas.ctx.fillStyle, '#22c55e');
});

test('Candlestick engine: mixed direction yields red down-candles', () => {
  const { producer, view } = rig();
  // newest chunk descends: publish pattern with a fall at the end
  const vals = [];
  for (let i = 0; i < 56; i++) vals.push(100);
  vals.push(100, 90, 80, 70, 60, 50, 40, 30); // newest chunk falls
  for (const v of vals) producer.publishLane(0, v, 0);
  const canvas = fakeCanvas();
  const engine = createCandlestickEngine({ lane: 0, candles: 8, chunkSize: 8 });
  const state = engine.init(canvas, canvas.ctx, view);
  engine.render(state, { frame: 1, nowNs: 0, skipped: 0, lateNs: 0 }, state.view);
  assert.equal(state.ohlc[0], 100, 'open of newest chunk');
  assert.equal(state.ohlc[3], 30, 'close of newest chunk');
  assert.ok(canvas.ctx.history.includes('#ef4444'), 'down-candle color drawn (newest chunk falls)');
});

test('OrderBook engine: one row per lane level, mid line drawn', () => {
  const { producer, view } = rig({ laneCount: 8 });
  // bids lanes 0..3, asks lanes 4..7
  for (let i = 0; i < 4; i++) producer.publishLane(i, 100 - i, 0);
  for (let i = 0; i < 4; i++) producer.publishLane(4 + i, 100 + i, 0);
  const canvas = fakeCanvas();
  const engine = createOrderBookEngine({ bidLanes: [0, 1, 2, 3], askLanes: [4, 5, 6, 7] });
  const state = engine.init(canvas, canvas.ctx, view);
  engine.render(state, { frame: 1, nowNs: 0, skipped: 0, lateNs: 0 }, state.view);
  const c = canvas.ctx.calls;
  assert.equal(c.fillText || 0, 0, 'frame path is TEXT-FREE (Law 1 — labels live in the DOM signal layer)');
  assert.ok((c.fillRect || 0) >= 9, '8 bars + mid line drawn');
});

test('AudioMeter engine: levels clamp, peak-hold holds then releases', () => {
  const { producer, view } = rig({ laneCount: 4 });
  producer.publishLane(0, 0.5, 0);
  producer.publishLane(1, 1.5, 0); // clip
  const canvas = fakeCanvas();
  const engine = createAudioMeterEngine({ lanes: [0, 1], holdMs: 1000 });
  const state = engine.init(canvas, canvas.ctx, view);
  engine.render(state, { frame: 1, nowNs: 0, skipped: 0, lateNs: 0 }, state.view);
  assert.equal(state.levels[0], 0.5);
  assert.equal(state.levels[1], 1, 'clipped to 1.0');
  assert.equal(state.peaks[1], 1, 'peak-hold captured the clip');
  // same instant → peak holds
  engine.render(state, { frame: 2, nowNs: 500e6, skipped: 0, lateNs: 0 }, state.view);
  assert.equal(state.peaks[1], 1);
  // beyond holdMs AND channel now below → peak releases to the new level
  producer.publishLane(1, 0.2, 0);
  engine.render(state, { frame: 3, nowNs: 1600e6, skipped: 0, lateNs: 0 }, state.view);
  assert.equal(state.peaks[1], 0.2, 'peak released after hold expiry');
});

test('Visualizer components wire through WeftCanvas with ZERO setState', () => {
  const { shim, hooks, producer, ctx } = rig();
  const V = createVisualizers(shim.React, hooks);
  const el = V.WeftOscilloscope({ plane: ctx, hz: 250, engineProps: { lane: 0 } });
  assert.equal(el.type, 'canvas', 'healthy visualizer renders a canvas');
  el.props.ref.current = { width: 200, height: 100, getContext: () => make2DRecorder(), handlers: {}, addEventListener() {}, removeEventListener() {} };
  shim.mount();
  for (let i = 0; i < 40; i++) {
    producer.publishLane(0, Math.sin(i / 3), i);
    ctx.scheduler.pump(i * 4e6);
  }
  assert.equal(shim.setStateCalls.length, 0, 'streaming never touches React state');
  shim.unmount();
  ctx.release();
});

test('All four visualizers exist with stable component identity', () => {
  const { shim, hooks } = rig();
  const V = createVisualizers(shim.React, hooks);
  for (const name of ['WeftOscilloscope', 'WeftCandlestickChart', 'WeftOrderBook', 'WeftAudioMeter']) {
    assert.equal(typeof V[name], 'function', `${name} exported`);
    assert.equal(V[name], V[name], `${name} identity stable`);
  }
});
