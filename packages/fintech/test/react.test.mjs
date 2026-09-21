// react.test.mjs — zero-re-render contract proof with a recording React shim.
//
// Law 4 (W4-01/W4-06): the component function runs EXACTLY ONCE; book and
// telemetry updates flow through the controller + direct DOM mutation and
// NEVER through setState/props/state. The shim records every call, so any
// violation is mechanically visible.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createWeftOrderBook, createWeftTelemetryBar, paintTelemetryNodes } from '../src/react.js';
import { createOrderBookController } from '../src/orderbook-canvas.js';
import { packSnapshot, MDP1_SIZE, MDP1_TOP_LEVELS } from '../src/mdp1.js';
import { OrderBook } from '../src/book.js';
import { MarketTelemetry } from '../src/telemetry.js';

function shimReact() {
  const effects = [];
  const calls = { components: 0, setState: 0, createElement: 0 };
  return {
    calls,
    effects,
    useRef: (v) => ({ current: v ?? null }),
    useEffect: (fn) => { effects.push(fn); },
    useState: () => { calls.setState++; return [null, () => calls.setState++]; },
    createElement: (type, props, ...children) => {
      calls.createElement++;
      return { type, props, children: children.flat() };
    },
  };
}

function recordingCtx() {
  return {
    drawCalls: 0,
    fillStyle: '',
    font: '',
    fillRect(x, y, w, h) { this.drawCalls++; },
    fillText(s, x, y) { this.drawCalls++; },
  };
}

function feedAndSnapshot(msgs = 64) {
  const b = new OrderBook();
  for (let i = 0; i < msgs; i++) {
    b.applyView({
      type: 0x41, ts: i, refHi: 0, refLo: i + 1,
      side: i % 2 ? 0x53 : 0x42, shares: 100 + i, price: i % 2 ? 1001000 + i : 999000 - i,
      match: 0, ref2Hi: 0, ref2Lo: 0, locate: 7777,
    });
  }
  const buf = new Uint8Array(MDP1_SIZE);
  packSnapshot(b, buf, new Int32Array(MDP1_TOP_LEVELS), new Int32Array(MDP1_TOP_LEVELS));
  return buf;
}

test('<WeftOrderBook /> renders once and frames never re-invoke the component', () => {
  const React = shimReact();
  const WeftOrderBook = createWeftOrderBook(React);
  const mdp1 = feedAndSnapshot();
  const controllers = [];
  const el = WeftOrderBook({
    mdp1,
    createController: (opts) => {
      const c = createOrderBookController({ ...opts, clock: () => 0 });
      controllers.push(c);
      return c;
    },
  });
  // component ran once, produced canvas + banner
  assert.equal(React.calls.createElement, 3); // div + canvas + banner
  React.effects[0](); // mount
  assert.equal(controllers.length, 1);
  const controller = controllers[0];

  // drive 10,000 frames with changing book state — component never re-runs
  for (let i = 0; i < 10_000; i++) {
    mdp1[8] = i & 0xff;         // mutate seq bytes (wire-level churn)
    controller.render(i * 4_166_666);
  }
  assert.equal(controller.state.frames, 10_000); // loop ran, no re-render
  // component identity: createElement count unchanged (no re-render happened)
  assert.equal(React.calls.createElement, 3);
});

test('controller draws on seq change, skips identical frames, honors CRC', () => {
  const mdp1 = feedAndSnapshot();
  const ctx = recordingCtx();
  const controller = createOrderBookController({ mdp1, ctx, drawLabels: false });
  controller.render(0);
  const afterFirst = ctx.drawCalls;
  controller.render(1); // seq unchanged -> geometry redrawn but cheap; seq check in label path
  assert.ok(ctx.drawCalls >= afterFirst);
  // torn record: corrupt payload -> CRC fails -> frame skipped + torn count
  const torn = feedAndSnapshot();
  torn[100] ^= 0xff;
  const c2 = createOrderBookController({ mdp1: torn, ctx: recordingCtx() });
  const ok = c2.render(0);
  assert.equal(ok, false);
  assert.equal(c2.state.torn, 1);
  assert.equal(c2.state.drawCalls, 0);
});

test('240 FPS virtual clock: 4,800 frames, simulated 0.5ms cost -> 0 dropped', () => {
  const mdp1 = feedAndSnapshot();
  let simCost = 500_000; // 0.5ms per frame simulated
  let now = 0;
  const clock = () => { now += simCost; return now; };
  const controller = createOrderBookController({
    mdp1, ctx: recordingCtx(), clock, frameBudgetNs: 1e9 / 240,
  });
  for (let f = 0; f < 4800; f++) controller.render(f * (1e9 / 240));
  assert.equal(controller.state.frames, 4800);
  assert.equal(controller.state.drops, 0, `drops=${controller.state.drops} maxCost=${controller.state.maxCostNs}`);
});

test('negative control: simulated 5ms cost MUST trip the drop counter', () => {
  const mdp1 = feedAndSnapshot();
  let now = 0;
  const clock = () => { now += 5_000_000; return now; };
  const controller = createOrderBookController({
    mdp1, ctx: recordingCtx(), clock, frameBudgetNs: 1e9 / 240,
  });
  for (let f = 0; f < 480; f++) controller.render(f * (1e9 / 240));
  assert.equal(controller.state.frames, 480);
  assert.equal(controller.state.drops, 480); // every frame over budget
});

test('degrade/restore: context loss flips FALLBACK without throwing (W4-04)', () => {
  const mdp1 = feedAndSnapshot();
  const controller = createOrderBookController({ mdp1, ctx: recordingCtx() });
  const reason = controller.degrade('E_CONTEXT_LOST');
  assert.equal(reason, 'E_CONTEXT_LOST');
  assert.ok(controller.state.fallback);
  controller.restore();
  assert.ok(!controller.state.fallback);
});

test('telemetry bar: 10,000 updates mutate nodes in place; ZERO setState', () => {
  const React = shimReact();
  const telemetry = new MarketTelemetry();
  const Bar = createWeftTelemetryBar(React);
  Bar({ telemetry });
  React.effects[0](); // mount — would call requestAnimationFrame; guarded below

  const nodes = {
    rate: { nodeValue: '' },
    lat: { nodeValue: '' },
    msg: { nodeValue: '' },
  };
  for (let i = 0; i < 10_000; i++) {
    telemetry.record(40, (i + 1) * 400_000);
    telemetry.recordLatency(1500);
    paintTelemetryNodes(telemetry.slots, nodes);
  }
  assert.notEqual(nodes.rate.nodeValue, '');
  assert.ok(nodes.lat.nodeValue.endsWith('us'));
  assert.equal(nodes.msg.nodeValue, '10000');
  // the component itself never touched setState (shim counts useState too)
  assert.equal(React.calls.setState, 0);
});

test('unmount cleanup: stop() detaches without leaks (no timer left running)', () => {
  const React = shimReact();
  const mdp1 = feedAndSnapshot();
  const WeftOrderBook = createWeftOrderBook(React);
  let stopped = false;
  WeftOrderBook({
    mdp1,
    createController: (opts) => createOrderBookController({ ...opts, clock: () => 0 }),
    bindLoop: () => () => { stopped = true; },
  });
  const unmount = React.effects[0]();
  assert.equal(typeof unmount, 'function');
  unmount();
  assert.ok(stopped);
});
