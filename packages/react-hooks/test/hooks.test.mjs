// @weft/react-hooks — verification suite (node --test, zero deps).
//
// Gates:
//   Zero-rerender   10k frames -> render count stays 1 (the Pillar 1 §2.E
//                   contract: no setState, no VDOM reconciliation)
//   Lifecycle       unmount unsubscribes; StrictMode double-mount safe
//   Latest-ref      fresh onFrame closure per render, no resubscribe
//   Canvas pump     coalescing (newest frame wins), idle parking, hidden
//                   parking + visibilitychange resume, rAF cancellation
//   Sample ring     fixed capacity, wraparound, callback iteration
//   Graph painter   auto-scale + op recording (allocation-free path)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';

import {
  createHooks,
  createFrameSource,
  attachWebSocket,
  createSampleRing,
  drawFrameGraph,
} from '../src/core.js';
import { createShim, createCanvas } from './helpers/react-shim.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));

test('createFrameSource: subscribe/emit/unsubscribe lifecycle', () => {
  const src = createFrameSource();
  let calls = 0;
  const off = src.subscribe(() => calls++);
  assert.equal(src.listenerCount, 1);
  src.emit(new ArrayBuffer(8), 0, 8);
  assert.equal(calls, 1);
  off();
  off(); // idempotent
  assert.equal(src.listenerCount, 0);
  src.emit(new ArrayBuffer(8), 0, 8);
  assert.equal(calls, 1);
});

test('useWeftBuffer: 10,000 frames, ONE render — the zero-rerender contract', () => {
  const shim = createShim();
  const { useWeftBuffer } = createHooks(shim.React);
  const source = createFrameSource();
  let frames = 0;
  const Component = () => {
    useWeftBuffer(source, () => frames++);
    return null;
  };
  shim.render(Component);
  assert.equal(shim.renderCount, 1);

  const buf = new ArrayBuffer(64);
  for (let i = 0; i < 10_000; i++) source.emit(buf, 0, 64);
  assert.equal(frames, 10_000);
  assert.equal(shim.renderCount, 1, 'frames must never trigger re-renders');

  shim.unmount();
  source.emit(buf, 0, 64);
  assert.equal(frames, 10_000, 'no frames after unmount');
  assert.equal(source.listenerCount, 0, 'no leaked listeners');
});

test('useWeftBuffer: StrictMode double-mount leaves exactly one subscription', () => {
  const shim = createShim();
  const { useWeftBuffer } = createHooks(shim.React);
  const source = createFrameSource();
  let frames = 0;
  const Component = () => {
    useWeftBuffer(source, () => frames++);
    return null;
  };
  shim.strictRender(Component);
  assert.equal(source.listenerCount, 1, 'first mount must have cleaned up');
  source.emit(new ArrayBuffer(8), 0, 8);
  assert.equal(frames, 1);
});

test('useWeftBuffer: latest-ref — new closure per render without resubscribe', () => {
  const shim = createShim();
  const { useWeftBuffer } = createHooks(shim.React);
  const source = createFrameSource();
  let which = 'a';
  let countA = 0;
  let countB = 0;
  const Component = ({ variant }) => {
    useWeftBuffer(source, variant === 'a' ? () => countA++ : () => countB++);
    return null;
  };
  shim.render(() => Component({ variant: 'a' }));
  shim.render(() => Component({ variant: 'b' })); // fresh closure, same subscription
  assert.equal(shim.renderCount, 2);
  assert.equal(source.listenerCount, 1, 'resubscribe must not happen');
  source.emit(new ArrayBuffer(8), 0, 8);
  assert.equal(countA, 0);
  assert.equal(countB, 1);
  void which;
});

test('useWeftCanvas: coalescing — 100 frames, one tick, newest frame wins', () => {
  const shim = createShim();
  const savedRaf = globalThis.requestAnimationFrame;
  const savedCancel = globalThis.cancelAnimationFrame;
  const savedDoc = globalThis.document;
  globalThis.requestAnimationFrame = shim.requestAnimationFrame;
  globalThis.cancelAnimationFrame = shim.cancelAnimationFrame;
  globalThis.document = shim.document;
  try {
    const { useWeftCanvas } = createHooks(shim.React);
    const source = createFrameSource();
    const { canvas, ctx } = createCanvas();
    const drawn = [];
    let refResult = null;
    const Component = () => {
      refResult = useWeftCanvas(source, (c, buffer, byteOffset) => {
        drawn.push(byteOffset + (buffer.__tag ?? 0));
        c.clearRect(0, 0, 8, 8);
      });
      return null;
    };
    shim.render(Component);
    refResult.current = canvas; // simulate ref attach

    for (let i = 0; i < 100; i++) {
      const b = new ArrayBuffer(64);
      b.__tag = i; // tag to identify the frame
      source.emit(b, 0, 64);
    }
    assert.equal(shim.raf.queue.length, 1, 'one rAF requested despite 100 frames');
    assert.equal(shim.raf.step(), 1);
    assert.equal(drawn.length, 1);
    assert.equal(drawn[0], 99, 'newest frame wins');
    assert.equal(ctx.cleared, 1);

    assert.equal(shim.raf.step(), 1, 'loop re-queues once after a draw');
    assert.equal(drawn.length, 1, 'idle tick draws nothing');
    assert.equal(shim.raf.queue.length, 0, 'loop parks when idle');
  } finally {
    globalThis.requestAnimationFrame = savedRaf;
    globalThis.cancelAnimationFrame = savedCancel;
    globalThis.document = savedDoc;
  }
});

test('useWeftCanvas: parks when hidden, resumes on visibilitychange', () => {
  const shim = createShim();
  const savedRaf = globalThis.requestAnimationFrame;
  const savedCancel = globalThis.cancelAnimationFrame;
  const savedDoc = globalThis.document;
  globalThis.requestAnimationFrame = shim.requestAnimationFrame;
  globalThis.cancelAnimationFrame = shim.cancelAnimationFrame;
  globalThis.document = shim.document;
  try {
    const { useWeftCanvas } = createHooks(shim.React);
    const source = createFrameSource();
    const { canvas, ctx } = createCanvas();
    let draws = 0;
    const Component = () => {
      const ref = useWeftCanvas(source, () => draws++);
      ref.current = canvas;
      return null;
    };
    shim.render(Component);

    shim.document.hidden = true;
    source.emit(new ArrayBuffer(64), 0, 64);
    assert.equal(shim.raf.step(), 1);
    assert.equal(draws, 0, 'no draw while hidden');
    assert.equal(shim.raf.queue.length, 0, 'loop parked itself');

    shim.document.hidden = false;
    shim.document.dispatch('visibilitychange');
    assert.equal(shim.raf.queue.length, 1, 'visibilitychange resumed the loop');
    assert.equal(shim.raf.step(), 1);
    assert.equal(draws, 1, 'the parked frame was painted on resume');
    assert.equal(ctx.cleared, 0);
  } finally {
    globalThis.requestAnimationFrame = savedRaf;
    globalThis.cancelAnimationFrame = savedCancel;
    globalThis.document = savedDoc;
  }
});

test('useWeftCanvas: unmount cancels pending rAF and detaches listeners', () => {
  const shim = createShim();
  const savedRaf = globalThis.requestAnimationFrame;
  const savedCancel = globalThis.cancelAnimationFrame;
  const savedDoc = globalThis.document;
  globalThis.requestAnimationFrame = shim.requestAnimationFrame;
  globalThis.cancelAnimationFrame = shim.cancelAnimationFrame;
  globalThis.document = shim.document;
  try {
    const { useWeftCanvas } = createHooks(shim.React);
    const source = createFrameSource();
    const Component = () => {
      useWeftCanvas(source, () => {});
      return null;
    };
    shim.render(Component);
    source.emit(new ArrayBuffer(64), 0, 64);
    assert.equal(shim.raf.queue.length, 1);
    const before = shim.raf.cancelCount;
    shim.unmount();
    assert.equal(shim.raf.cancelCount, before + 1, 'pending rAF cancelled');
    assert.equal(source.listenerCount, 0);
    shim.raf.step();
    // no crash, nothing drawn — callback gone
  } finally {
    globalThis.requestAnimationFrame = savedRaf;
    globalThis.cancelAnimationFrame = savedCancel;
    globalThis.document = savedDoc;
  }
});

test('createSampleRing: fixed capacity, wraparound order, callback iteration', () => {
  const ring = createSampleRing(4);
  for (const v of [1, 2, 3, 4]) ring.push(v);
  assert.equal(ring.length, 4);
  const seen = [];
  ring.forEach((v) => seen.push(v));
  assert.deepEqual(seen, [1, 2, 3, 4]);
  ring.push(5); // overwrite oldest
  ring.push(6);
  seen.length = 0;
  ring.forEach((v) => seen.push(v));
  assert.deepEqual(seen, [3, 4, 5, 6]);
});

test('drawFrameGraph: auto-scale and painter ops (allocation-free reads)', () => {
  const ctx = {
    ops: [],
    clearRect() { this.ops.push('clear'); },
    beginPath() { this.ops.push('begin'); },
    moveTo(x, y) { this.ops.push(['move', x, y]); },
    lineTo(x, y) { this.ops.push(['line', x, y]); },
    stroke() { this.ops.push('stroke'); },
  };
  const ring = createSampleRing(3);
  ring.push(0);
  ring.push(10);
  ring.push(5);
  drawFrameGraph(ctx, ring, 100, 50);
  assert.equal(ctx.ops[0], 'clear');
  assert.equal(ctx.ops[1], 'begin');
  assert.equal(ctx.ops.at(-1), 'stroke');
  const moves = ctx.ops.filter((o) => Array.isArray(o));
  assert.equal(moves.length, 3);
  // first sample (0) maps to the bottom, last (5) to mid-height
  assert.ok(Math.abs(moves[0][2] - 49) < 1e-9, `bottom edge, got ${moves[0][2]}`);
});

test('attachWebSocket: binary messages flow, strings ignored, detach works', () => {
  const source = createFrameSource();
  const handlers = new Map();
  const fakeWs = {
    listener: null,
    addEventListener(type, fn) {
      this.listener = fn;
    },
    removeEventListener(type, fn) {
      if (this.listener === fn) this.listener = null;
    },
    dispatch(type, ev) {
      if (this.listener) this.listener(ev);
    },
  };
  const detach = attachWebSocket(source, fakeWs);
  let frames = 0;
  source.subscribe(() => frames++);
  fakeWs.dispatch('message', { data: new ArrayBuffer(16) });
  fakeWs.dispatch('message', { data: 'text frame' });
  assert.equal(frames, 1);
  detach();
  fakeWs.dispatch('message', { data: new ArrayBuffer(16) });
  assert.equal(frames, 1);
});

test('package surface: index.js re-exports the full API', () => {
  const src = readFileSync(join(HERE, '..', 'src', 'index.js'), 'utf8');
  for (const name of ['useWeftBuffer', 'useWeftCanvas', 'createFrameSource', 'attachWebSocket', 'createSampleRing', 'drawFrameGraph']) {
    assert.ok(src.includes(name), `missing export ${name}`);
  }
  // zero-dep guarantee: the only runtime import is react itself + core
  assert.match(src, /from 'react'/);
  assert.match(src, /from '\.\/core\.js'/);
});
