// test/canvas.test.mjs — <WeftCanvas /> through the shim: engine contract,
// render loop with ZERO setState while frames stream, Law-4 fatal paths
// (bad engine, null context, GPU context loss) and Page Visibility pauses.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { HotPlaneProducer, HPL1 } from '../../heddle-core/src/index.js';
import { createHeddleHooks, createWeftCanvas, PlaneContext, getPlaneContext } from '../src/core.js';
import { makeShim } from './shim.mjs';

function countingEngine() {
  const engine = {
    contextType: '2d',
    init(canvas, ctx, view) {
      engine.lastInitState = { frames: 0, canvas, ctx, view, disposed: false };
      return engine.lastInitState;
    },
    render(state, frameCtx, view) { state.frames += 1; },
    dispose(state) { state.disposed = true; },
  };
  return engine;
}

function rig({ hz = 250 } = {}) {
  const shim = makeShim();
  const hooks = createHeddleHooks(shim.React);
  const WeftCanvas = createWeftCanvas(shim.React, hooks);
  const producer = HotPlaneProducer.create({ laneCount: 4, samplesPerLane: 256, tickHz: hz });
  const ctx = new PlaneContext(producer.planeBuffer, { hz, raf: null, producer });
  const canvasStub = {
    handlers: {},
    nullCtx: false,
    getContext() { return this.nullCtx ? null : { stub: true }; },
    addEventListener(t, fn) { this.handlers[t] = fn; },
    removeEventListener(t) { delete this.handlers[t]; },
  };
  return { shim, hooks, WeftCanvas, producer, ctx, canvasStub, hz };
}

function mount(r, props) {
  const el = r.WeftCanvas(props);
  if (el.type === 'canvas') el.props.ref.current = r.canvasStub; // React attaches refs pre-effect
  r.shim.mount();
  return el;
}

test('WeftCanvas: engine init once, render per scheduled frame, ZERO setState', () => {
  const r = rig();
  const engine = countingEngine();
  const el = mount(r, { plane: r.ctx, engine, hz: r.hz, fallback: 'OFFLINE' });
  assert.equal(el.type, 'canvas', 'renders a canvas element while healthy');
  const state = engine.lastInitState;
  for (let i = 0; i < 10_000; i++) {
    r.producer.publishLane(0, i * 0.25, i);
    if ((i & 7) === 0) r.ctx.scheduler.pump(i * 4e6); // 1250 scheduled frames
  }
  r.ctx.scheduler.pump(10_000 * 4e6);
  assert.equal(state.frames, 1251, 'engine rendered EVERY scheduled frame');
  assert.equal(r.shim.setStateCalls.length, 0, 'no setState on the telemetry stream');
  r.shim.unmount();
  assert.equal(state.disposed, true, 'engine disposed on unmount');
  r.ctx.release();
});

test('WeftCanvas: engine without init/render is a Law-4 fatal (BAD_RENDER_ENGINE)', () => {
  const r = rig();
  const fatal = [];
  mount(r, { plane: r.ctx, engine: { contextType: '2d' }, onFatal: (c) => fatal.push(c) });
  assert.deepEqual(fatal, [HPL1.BAD_RENDER_ENGINE]);
  assert.equal(r.shim.setStateCalls.length, 1, 'exactly ONE exceptional re-render (the fallback)');
  r.shim.unmount();
  r.ctx.release();
});

test('WeftCanvas: null GPU context → CONTEXT_LOST fatal', () => {
  const r = rig();
  r.canvasStub.nullCtx = true; // getContext returns null
  const fatal = [];
  mount(r, { plane: r.ctx, engine: countingEngine(), onFatal: (c) => fatal.push(c) });
  assert.deepEqual(fatal, [HPL1.CONTEXT_LOST]);
  assert.equal(r.shim.setStateCalls.length, 1);
  r.shim.unmount();
  r.ctx.release();
});

test('WeftCanvas: webglcontextlost event → explicit CONTEXT_LOST fallback (Law 4)', () => {
  const r = rig();
  const fatal = [];
  mount(r, { plane: r.ctx, engine: countingEngine(), onFatal: (c) => fatal.push(c) });
  const handler = r.canvasStub.handlers.webglcontextlost;
  assert.equal(typeof handler, 'function', 'context-lost listener registered');
  handler({ preventDefault() {} });
  assert.deepEqual(fatal, [HPL1.CONTEXT_LOST]);
  r.shim.unmount();
  r.ctx.release();
});

test('WeftCanvas: Page Visibility pauses the scheduler, resume does not catch up', () => {
  const r = rig();
  // minimal document stub (core.js attaches visibilitychange when present)
  globalThis.document = {
    hidden: false,
    handlers: {},
    addEventListener(t, fn) { this.handlers[t] = fn; },
    removeEventListener(t) { delete this.handlers[t]; },
  };
  try {
    const events = [];
    mount(r, { plane: r.ctx, engine: countingEngine(), hz: r.hz, onEvent: (c, d) => events.push([c, d]) });
    assert.equal(typeof globalThis.document.handlers.visibilitychange, 'function');
    for (let i = 0; i < 100; i++) { r.producer.publishLane(0, i, i); r.ctx.scheduler.pump(i * 4e6); }
    const rendered = r.ctx.scheduler.rendered;
    globalThis.document.hidden = true;
    globalThis.document.handlers.visibilitychange();
    assert.equal(r.ctx.scheduler.paused, true, 'hidden → scheduler paused');
    assert.ok(events.some(([c]) => c === HPL1.TAB_HIDDEN), 'HPL1_TAB_HIDDEN surfaced');
    for (let i = 100; i < 200; i++) r.ctx.scheduler.pump(i * 4e6);
    assert.equal(r.ctx.scheduler.rendered, rendered, 'no frames while hidden');
    globalThis.document.hidden = false;
    globalThis.document.handlers.visibilitychange();
    assert.equal(r.ctx.scheduler.paused, false, 'visible → resumed');
    r.producer.publishLane(0, 1, 1);
    r.ctx.scheduler.pump(200 * 4e6);
    assert.equal(r.ctx.scheduler.rendered, rendered + 1, 'exactly ONE frame on resume (no catch-up)');
    r.shim.unmount();
  } finally {
    delete globalThis.document;
  }
  r.ctx.release();
});

test('WeftCanvas: raw SharedArrayBuffer plane prop (no PlaneContext) also works', () => {
  const r = rig();
  const engine = countingEngine();
  // pass the raw buffer: component adopts its own PlaneContext via getPlaneContext
  const el = mount(r, { plane: r.producer.planeBuffer, engine, hz: r.hz });
  assert.equal(el.type, 'canvas');
  for (let i = 0; i < 40; i++) r.producer.publishLane(0, i, i);
  const autoCtx = getPlaneContext(r.producer.planeBuffer, { hz: r.hz });
  autoCtx.scheduler.pump(0); // first pump anchors + renders
  autoCtx.scheduler.pump(4e6);
  assert.ok(engine.lastInitState.frames >= 1, 'auto-context scheduler drove the engine');
  r.shim.unmount();
  autoCtx.release();
});
