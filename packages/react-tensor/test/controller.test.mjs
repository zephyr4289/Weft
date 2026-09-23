// test/controller.test.mjs — TensorCanvasController: the zero-re-render frame
// path, driven headlessly with an injected pump (no React, no DOM).
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { WeftTensorRing, DLPackCode } from '../../weft-tensor/src/index.js';
import { TensorCanvasController } from '../src/core.js';

const U8 = { code: DLPackCode.UINT, bits: 8 };

function makeRing(w = 8, h = 4) {
  return WeftTensorRing.create({
    slotCount: 3, payloadCap: w * h * 4, dtype: U8, shape: [h, w, 4], tickHz: 120,
  });
}

function fakeCanvas() {
  const calls = { puts: 0, rects: 0, texts: 0 };
  const ctx = {
    calls,
    createImageData(w, h) { return { data: new Uint8ClampedArray(w * h * 4), width: w, height: h }; },
    putImageData() { calls.puts++; },
    strokeRect() { calls.rects++; },
    fillText() { calls.texts++; },
    beginPath() {}, moveTo() {}, lineTo() {}, stroke() {},
    lineWidth: 0, font: '', textBaseline: '', strokeStyle: null, fillStyle: null,
  };
  return { canvas: { getContext: (k) => (k === '2d' ? ctx : null) }, ctx, calls };
}

function manualPump() {
  const queue = [];
  let cancelled = 0;
  return {
    queue,
    get cancelled() { return cancelled; },
    request(cb) { queue.push(cb); return 1; },
    cancel() { cancelled++; },
    step() {
      const cbs = queue.splice(0, queue.length);
      for (const cb of cbs) cb();
      return cbs.length;
    },
  };
}

test('controller: every NEW frame renders exactly once — zero React involvement', () => {
  const ring = makeRing();
  const { canvas, calls } = fakeCanvas();
  const pump = manualPump();
  const seen = [];
  const c = new TensorCanvasController({
    ring, canvas, pump,
    overlay: (frame, scratch) => {
      seen.push(frame ? frame.seq : null);
      scratch.clear(); // detector contract: the overlay owns scratch lifecycle
      scratch.pushBox(1, 2, 3, 4, 0.9, 0);
    },
  });
  c.start();
  const statsIdentity = c.stats; // SAME object forever (no re-render trigger)
  pump.step(); // tick with NO frame -> stall path
  assert.equal(c.stats.stalls, 1);
  ring.commit(new Uint8Array(128).fill(9), { ts: 1n });
  pump.step();
  assert.deepEqual(seen, [null, 1]);
  assert.equal(calls.puts, 2, 'stall keeps last frame, frame blits once');
  assert.equal(calls.rects, 2, 'boxes drawn on stall tick AND frame tick');
  assert.equal(c.stats.frames, 1);
  assert.equal(c.stats.boxes, 1);
  // duplicate seq (nothing new) -> stall path repaints overlay, no re-blit
  pump.step();
  assert.equal(calls.puts, 3, 'stall repaints the LAST frame (canvas never flickers)');
  assert.deepEqual(seen, [null, 1, null], 'stall re-invokes overlay with null (HUD stays alive)');
  assert.equal(calls.rects, 3, 'stall ticks still draw overlay (HUD stays alive)');
  assert.ok(c.stats === statsIdentity, 'stats identity is stable');
  c.stop();
  assert.equal(pump.cancelled, 1);
});

test('controller: overlay receives the frame view and scratch is reused', () => {
  const ring = makeRing();
  const { canvas } = fakeCanvas();
  const pump = manualPump();
  const scratches = [];
  const c = new TensorCanvasController({
    ring, canvas, pump,
    overlay: (frame, scratch) => { scratches.push(scratch); scratch.clear(); },
    drawBoxes: false,
  });
  c.start();
  ring.commit(new Uint8Array(128), {});
  pump.step();
  ring.commit(new Uint8Array(128), {});
  pump.step();
  assert.equal(scratches.length, 2);
  assert.ok(scratches[0] === scratches[1], 'ONE OverlayScratch reused across frames');
  c.stop();
});

test('controller: requires ring and canvas (fail-fast)', () => {
  assert.throws(() => new TensorCanvasController({ canvas: {} }), /ring and canvas/);
  assert.throws(() => new TensorCanvasController({ ring: makeRing() }), /ring and canvas/);
});
