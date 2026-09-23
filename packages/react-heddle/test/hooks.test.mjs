// test/hooks.test.mjs — the three mandated signal hooks through the shim:
// useWeftSignal (micro-DOM mutator), useWeftStats (in-place live stats),
// useWeftBuffer (preallocated handler scratch). Proves ZERO setState/re-render
// while 100k+ frames stream through the plane.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { HotPlaneProducer, HPL1 } from '../../heddle-core/src/index.js';
import { createHeddleHooks, PlaneContext } from '../src/core.js';
import { makeShim } from './shim.mjs';

function rig({ laneCount = 4, samplesPerLane = 64, hz = 250 } = {}) { // 250 Hz ⇒ exact 4e6 ns interval (no fp drift)
  const shim = makeShim();
  const hooks = createHeddleHooks(shim.React);
  const producer = HotPlaneProducer.create({ laneCount, samplesPerLane, tickHz: hz });
  const ctx = new PlaneContext(producer.planeBuffer, { hz, raf: null, producer });
  return { shim, hooks, producer, ctx, plane: producer.planeBuffer };
}

test('useWeftSignal: binds node, mutates nodeValue per tick, ZERO setState', () => {
  const { shim, hooks, producer, ctx } = rig();
  const useWeftSignal = hooks.useWeftSignal;
  const ref = useWeftSignal(2, { plane: ctx });
  const node = { nodeValue: null };
  ref(node); // React calls ref callbacks on mount with the node
  shim.mount(); // effects run: ctx.acquire() starts scheduler? raf:null → pump manually

  // stop auto loop (raf null → start() would try self._raf...null → timer path only
  // if _timerMs set — it is NOT, so no loop; we pump the scheduler directly).
  for (let i = 0; i < 1000; i++) {
    producer.publishLane(2, 100 + i, 1000 + i);
    ctx.scheduler.pump(i * 4e6); // every pump renders (exact 4 ms interval)
  }
  // divider = round(hz/60) = 4 → text updated on frames 0,4,8,…,996
  assert.notEqual(node.nodeValue, null, 'signal wrote the lane value into the node');
  assert.equal(typeof node.nodeValue, 'number', 'nodeValue carries the raw number');
  const lastWriteFrame = Math.floor((ctx.scheduler.rendered - 1) / 4) * 4;
  assert.equal(node.nodeValue, 100 + lastWriteFrame,
    'latest written value matches the divider cadence');
  assert.equal(shim.setStateCalls.length, 0, 'NO setState — zero re-render by construction');
  shim.unmount();
  ctx.release();
});

test('useWeftSignal: textDivider honored (custom cadence)', () => {
  const { shim, hooks, producer, ctx } = rig();
  const ref = hooks.useWeftSignal(0, { plane: ctx, textDivider: 10 });
  const node = { nodeValue: null };
  ref(node);
  shim.mount();
  for (let i = 0; i < 100; i++) {
    producer.publishLane(0, i, i);
    ctx.scheduler.pump(i * 4e6);
  }
  assert.equal(node.nodeValue, 90, 'frames 0,10,…,90 → last written value 90');
  shim.unmount();
  ctx.release();
});

test('useWeftStats: stable object mutated in place — identity never changes', () => {
  const { shim, hooks, producer, ctx } = rig();
  const s1 = hooks.useWeftStats(1, { plane: ctx });
  const before = s1; // capture identity BEFORE mount/ticks
  shim.mount();
  for (let i = 0; i < 50; i++) {
    producer.publishLane(1, i * 2, i);
    ctx.scheduler.pump(i * 4e6);
  }
  assert.equal(s1, before, 'SAME object identity — mutated in place, never replaced');
  assert.equal(s1.current, 98);
  assert.equal(s1.min, 0);
  assert.equal(s1.max, 98);
  assert.ok(Math.abs(s1.avg - 49) < 1e-9);
  assert.equal(s1.samples, 50);
  assert.equal(shim.setStateCalls.length, 0);
  shim.unmount();
  ctx.release();
});

test('useWeftBuffer: stable buffer + publish routes through the producer', () => {
  const { shim, hooks, ctx } = rig();
  const b1 = hooks.useWeftBuffer(3, { plane: ctx });
  const before = b1;
  shim.mount();
  assert.equal(b1, before, 'stable identity — buffer never reallocated');
  b1.buffer[0] = 1234.5;
  b1.publish(77);
  const out = ctx.view.geo; // read back through the view
  const laneOut = { seqLo: 0, seqHi: 0, current: 0, min: 0, max: 0, avg: 0,
    samplesSeenLo: 0, samplesSeenHi: 0, head: 0, flags: 0, publishNsLo: 0, publishNsHi: 0, drops: 0 };
  assert.equal(ctx.view.readLane(3, laneOut), HPL1.OK);
  assert.equal(laneOut.current, 1234.5);
  assert.equal(laneOut.publishNsLo, 77);
  shim.unmount();
  ctx.release();
});

test('useWeftBuffer: publish without producer fails EXPLICITLY (Law 4)', () => {
  const { shim, hooks, plane } = rig();
  const roCtx = new PlaneContext(plane, { hz: 60, raf: null, producer: null });
  const b = hooks.useWeftBuffer(0, { plane: roCtx });
  shim.mount();
  b.buffer[0] = 1;
  assert.throws(() => b.publish(1), (e) => e.code === HPL1.PLANE_DETACHED);
  shim.unmount();
  roCtx.release();
});

test('provider resolution: hooks read the plane from context when no prop given', () => {
  const { shim, hooks, producer, ctx } = rig();
  const el = shim.React.createElement(hooks.WeftPlaneProvider, { value: ctx }, 'child');
  assert.equal(el.type, hooks.WeftPlaneProvider);
  // simulate provider scope for useContext
  const provided = shim.provider(hooks.PlaneCtxContext, ctx);
  shim.pushProvider(provided);
  const ref = hooks.useWeftSignal(1);
  const node = { nodeValue: null };
  ref(node);
  shim.mount();
  producer.publishLane(1, 7.25, 5);
  ctx.scheduler.pump(0);
  assert.equal(node.nodeValue, 7.25, 'context-provided plane resolved');
  assert.equal(shim.setStateCalls.length, 0);
  shim.unmount();
  shim.popProvider(provided);
  ctx.release();
});

test('no plane anywhere → explicit HPL1_PLAN_DETACHED error, not a mystery', () => {
  const { shim, hooks } = rig();
  const boom = () => hooks.useWeftSignal(0);
  // hooks without provider/prop must fail loudly at bind time
  assert.throws(() => {
    const ref = boom();
    ref({ nodeValue: null });
    shim.mount();
  }, (e) => e.code === HPL1.PLANE_DETACHED || e.message.includes('no plane'));
  shim.unmount();
});

test('100k frames through signal + stats: zero setState (mandate-scale re-render probe)', () => {
  const { shim, hooks, producer, ctx } = rig();
  const ref = hooks.useWeftSignal(0, { plane: ctx });
  const stats = hooks.useWeftStats(0, { plane: ctx });
  const node = { nodeValue: null };
  ref(node);
  shim.mount();
  for (let i = 0; i < 100_000; i++) {
    producer.publishLane(0, i * 0.5, i);
    if ((i & 1) === 0) ctx.scheduler.pump(i * 4e6); // 50k frames, 100k publishes
  }
  ctx.scheduler.pump(100_000 * 4e6); // final frame AFTER the last publish
  assert.equal(shim.setStateCalls.length, 0);
  assert.ok(stats.samples >= 100_000, `samples=${stats.samples}`);
  assert.ok(node.nodeValue !== null);
  shim.unmount();
  ctx.release();
});
