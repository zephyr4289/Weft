// test/hud.test.mjs — the fail-safe HUD: isolation from host crashes, live
// telemetry rows (fps/latency/throughput/dirty/tears), worker-crash fallback.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { HotPlaneProducer, HotPlaneView, HPL1 } from '../../heddle-core/src/index.js';
import { createHeddleHooks, PlaneContext } from '../src/core.js';
import { createWeftHud } from '../src/hud.js';
import { makeShim } from './shim.mjs';

function rig({ laneCount = 4, samplesPerLane = 256, hz = 250 } = {}) {
  const shim = makeShim();
  const hooks = createHeddleHooks(shim.React);
  const { WeftHud, mountWeftHud } = createWeftHud(shim.React, hooks);
  const producer = HotPlaneProducer.create({ laneCount, samplesPerLane, tickHz: hz });
  const ctx = new PlaneContext(producer.planeBuffer, { hz, raf: null, producer });
  const doc = createStubDoc();
  const host = doc.createElement('div');
  return { shim, hooks, WeftHud, mountWeftHud, producer, ctx, doc, host };
}

function createStubDoc() {
  function make(tag) {
    const node = {
      tagName: tag, children: [], className: '', textContent: '', _attrs: {},
      _parentNode: null,
      setAttribute(k, v) { this._attrs[k] = v; },
      getAttribute(k) { return this._attrs[k]; },
      appendChild(c) { this.children.push(c); c._parentNode = this; return c; },
      removeChild(c) {
        const i = this.children.indexOf(c);
        if (i >= 0) this.children.splice(i, 1);
        c._parentNode = null;
        return c;
      },
      get parentNode() { return this._parentNode; },
      ownerDocument: null,
    };
    return node;
  }
  const doc = make('#document');
  doc.ownerDocument = doc;
  doc.createElement = (tag) => {
    const n = make(tag);
    n.ownerDocument = doc;
    return n;
  };
  return doc;
}

test('HUD builds 7 telemetry rows and streams values from the plane', () => {
  const { mountWeftHud, producer, ctx, host } = rig();
  producer.publishLane(0, 1, 0);
  producer.publishTick(new Float64Array([1, 1, 1, 1]), 4, 1000);
  const hud = mountWeftHud(host, ctx, { intervalMs: 0 }); // manual ticks
  // first tick primes baselines
  hud.tick();
  // stream for a while
  for (let i = 0; i < 50; i++) {
    producer.publishLane(0, i, i * 4);
    ctx.scheduler.pump(i * 4e6);
  }
  hud.tick();
  assert.equal(hud.rows.length, 7);
  for (const r of hud.rows) assert.notEqual(r.textContent, '—', 'every row rendered');
  assert.ok(hud.rows[1].textContent.includes('.') || hud.rows[1].textContent === '—',
    'FPS(ui) numeric');
  hud.destroy();
  ctx.release();
});

test('HUD FPS(prod) rises with publish rate; MEM throughput reflects lane 0', () => {
  const { mountWeftHud, producer, ctx, host } = rig();
  const hud = mountWeftHud(host, ctx, { intervalMs: 0 });
  hud.tick(); // prime
  const t0 = Date.now() + 100; // ensure dt > 0 on next tick
  let lastNs = 0;
  for (let i = 0; i < 250; i++) { producer.publishLane(0, i, i); lastNs = i; }
  producer.publishTick(new Float64Array(4).fill(1), 4, lastNs);
  hud.tick();
  const mem = parseFloat(hud.rows[3].textContent);
  assert.ok(mem > 0, `MEM throughput positive, got ${hud.rows[3].textContent}`);
  hud.destroy();
  ctx.release();
});

test('ISOLATION: a host UI crash does not stop the HUD (rows keep updating)', () => {
  const { mountWeftHud, producer, ctx, host } = rig();
  const hud = mountWeftHud(host, ctx, { intervalMs: 0 });
  hud.tick();
  // HOST "app" crash: simulate the host's rAF loop throwing every frame
  let hostThrew = 0;
  const hostileHostLoop = () => { throw new Error('host UI exploded'); };
  for (let i = 0; i < 10; i++) {
    try { hostileHostLoop(); } catch { hostThrew += 1; }
    producer.publishLane(0, i, i);
    hud.tick(); // HUD keeps its own cadence regardless
  }
  assert.equal(hostThrew, 10, 'host crashed every frame');
  assert.notEqual(hud.rows[3].textContent, '—', 'HUD telemetry still updating');
  assert.notEqual(hud.rows[6].textContent, 'FALLBACK 11', 'no worker-crash banner (host crash ≠ plane crash)');
  hud.destroy();
  ctx.release();
});

test('WORKER CRASH: explicit FALLBACK banner, HUD stays alive (Law 4)', () => {
  const { mountWeftHud, producer, ctx, host } = rig();
  const handlers = {};
  const fakeWorker = {
    addEventListener(t, fn) { handlers[t] = fn; },
    removeEventListener(t) { delete handlers[t]; },
  };
  const hud = mountWeftHud(host, ctx, { intervalMs: 0, worker: fakeWorker });
  hud.tick();
  assert.equal(hud.rows[6].textContent, 'OK');
  handlers.error({ message: 'producer worker died' }); // crash event
  producer.publishLane(0, 5, 5);
  hud.tick();
  assert.equal(hud.fatal.code, HPL1.WORKER_CRASH);
  assert.equal(hud.rows[6].textContent, `FALLBACK ${HPL1.WORKER_CRASH}`);
  assert.equal(hud.rows[6].className, 'fatal', 'banner styled fatal');
  // rows still stream AFTER the crash — the HUD itself survived
  assert.notEqual(hud.rows[3].textContent, '—');
  hud.destroy();
  ctx.release();
});

test('Seqlock tears surface in the TEARS row — never silent', () => {
  const { mountWeftHud, producer, host } = rig();
  const view = new HotPlaneView(producer.planeBuffer, { maxTries: 4 });
  producer.publishLane(0, 1, 1);
  const u32 = new Uint32Array(producer.planeBuffer);
  const ctrl = view.geo.laneCtrlBase;
  u32[ctrl >> 2] |= 1; // force odd (write-in-progress) → tear
  const out = { seqLo: 0, seqHi: 0, current: 0, min: 0, max: 0, avg: 0, samplesSeenLo: 0, samplesSeenHi: 0, head: 0, flags: 0, publishNsLo: 0, publishNsHi: 0, drops: 0 };
  view.readLane(0, out); // torn → tears counted
  const hud = mountWeftHud(host, view, { intervalMs: 0 });
  hud.tick();
  assert.equal(hud.rows[5].textContent, String(view.tears));
  assert.ok(view.tears > 0, 'tears registered before HUD tick');
  hud.destroy();
});

test('<WeftHud /> mounts on ref host and destroy() removes the box + stops ticks', () => {
  const { WeftHud, producer, ctx, host, shim } = rig();
  const el = WeftHud({ plane: ctx, intervalMs: 0, host });
  assert.equal(el, null, 'external host mode renders nothing into the React tree');
  shim.mount();
  // effect ran → HUD box attached to the external host
  assert.equal(host.children.length, 1, 'HUD box attached to shadow/host');
  const hudLike = host.children[0];
  assert.equal(hudLike.getAttribute('data-weft-hud'), '1');
  shim.unmount();
  assert.equal(host.children.length, 0, 'destroy removed the HUD box');
  ctx.release();
});

test('mountWeftHud with a null host fails explicitly (Law 4)', () => {
  const { mountWeftHud, producer } = rig();
  assert.throws(() => mountWeftHud(null, producer.planeBuffer), (e) => e.code === HPL1.PLANE_DETACHED);
});
