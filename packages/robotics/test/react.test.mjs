// test/react.test.mjs — <WeftPointCloudViewer /> / <WeftAttitudeIndicator />
// zero-re-render contract with a recording React shim (Pillar 4/6 pattern).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  createWeftPointCloudViewer, createWeftAttitudeIndicator,
  attachRing, FMT_POINTS_F32, FMT_IMU6DOF, makeMvp,
} from '../src/index.js';

// -- recording React shim -----------------------------------------------------
function makeReact(opts = {}) {
  const calls = { createElement: [], effects: [] };
  const hooks = [];
  let hookIdx = 0;
  function useRef(init) {
    const i = hookIdx++;
    hooks[i] = hooks[i] ?? { current: i === 0 && opts.canvas !== undefined ? opts.canvas : init };
    return hooks[i];
  }
  function useEffect(fn, deps) {
    calls.effects.push({ fn, deps, run() { return fn(); } });
  }
  function createElement(type, props, ...children) {
    calls.createElement.push({ type, props, children });
    return { type, props, children };
  }
  return { calls, react: { useRef, useEffect, createElement } };
}

function buildRing(records, { slotCount = 8 } = {}) {
  const total = 128 + 4096 * slotCount;
  const buf = new ArrayBuffer(total);
  const dv = new DataView(buf);
  const u8 = new Uint8Array(buf);
  u8.set([0x52, 0x4e, 0x47, 0x31], 0);
  dv.setUint16(4, 1, true);
  dv.setUint16(6, 128, true);
  dv.setUint32(8, 4096, true);
  dv.setUint32(12, slotCount, true);
  let seq = 0;
  for (const [topic, fmt, payload] of records) {
    seq++;
    const base = 128 + ((seq - 1) % slotCount) * 4096;
    dv.setBigUint64(base, BigInt(seq), true);
    dv.setUint32(base + 8, payload.length, true);
    dv.setUint32(base + 12, topic, true);
    dv.setUint32(base + 24, fmt, true);
    u8.set(payload, base + 64);
  }
  dv.setBigUint64(24, BigInt(seq), true);
  return buf;
}

function fakeGl() {
  const targets = [];
  return {
    drawingBufferWidth: 640, drawingBufferHeight: 480,
    VERTEX_SHADER: 1, FRAGMENT_SHADER: 2, COMPILE_STATUS: 3,
    LINK_STATUS: 4, ARRAY_BUFFER: 5, STREAM_DRAW: 6, POINTS: 7,
    COLOR_BUFFER_BIT: 8, DEPTH_BUFFER_BIT: 9, DEPTH_TEST: 10,
    getShaderParameter: () => true, getProgramParameter: () => true,
    createShader: () => ({}), shaderSource() {}, compileShader() {},
    createProgram: () => ({}), attachShader() {}, linkProgram() {},
    getUniformLocation: () => ({}), createBuffer: () => ({}),
    bindBuffer() {}, bufferData() {}, bufferSubData() {},
    createVertexArray: () => ({}), bindVertexArray() {},
    enableVertexAttribArray() {}, vertexAttribPointer() {},
    enable() {}, useProgram() {}, uniformMatrix4fv() {},
    viewport() {}, clearColor() {}, clear() {}, drawArrays() {},
  };
}

test('point cloud viewer: null canvas degrades to FALLBACK banner (W4-04)', () => {
  const { calls, react } = makeReact();
  const Viewer = createWeftPointCloudViewer(react);
  Viewer({ source: { acquire: () => null }, createEngine: ({ gl }) => ({ gl, ok: true, draw: () => true, frames: 0, pointsDrawn: 0 }) });
  assert.equal(calls.createElement.length, 4); // div + canvas + stats + banner
  assert.equal(calls.effects.length, 1);
  // null canvas -> mount shows the banner via direct DOM mutation and
  // returns NO cleanup (valid React semantics; engine never constructed)
  const unmount = calls.effects[0].run();
  assert.equal(unmount, undefined);
  // children evaluate before parents: canvas, stats, banner, container.
  const banner = calls.createElement.find((c) => c.props['data-weft-fallback'] !== undefined);
  assert.notEqual(banner, undefined);
  assert.equal(banner.props['data-weft-fallback'], 'hidden');
  assert.equal(calls.createElement.length, 4); // still exactly one render
});

test('point cloud viewer: 1,000 live frames drive the engine, ZERO re-renders', () => {
  const { calls, react } = makeReact({ canvas: { getContext: () => fakeGl() } });
  const buf = buildRing([[1, FMT_POINTS_F32, new Uint8Array(96)]]);
  const ring = attachRing(buf);
  // live source: committed advances per tick
  const dv = new DataView(buf);
  let tick = 0n;
  const source = {
    acquire() {
      tick++;
      const seq = 8n + tick;
      const base = 128 + (Number(seq - 1n) % 8) * 4096;
      dv.setBigUint64(base, seq, true);
      dv.setBigUint64(24, seq, true);
      return ring.acquire();
    },
  };
  let drawn = 0;
  const Viewer = createWeftPointCloudViewer(react);
  Viewer({ source, createEngine: ({ gl }) => ({
    gl, ok: true, frames: 0, pointsDrawn: 0,
    draw(f32, n) { drawn++; this.frames++; this.pointsDrawn = n; return true; },
  }) });
  const rafQ = [];
  const origRaf = globalThis.requestAnimationFrame;
  const origCaf = globalThis.cancelAnimationFrame;
  globalThis.requestAnimationFrame = (fn) => (rafQ.push(fn), rafQ.length);
  globalThis.cancelAnimationFrame = () => {};
  calls.effects[0].run();
  // simulate 1,000 display ticks
  for (let i = 0; i < 1000; i++) {
    const fn = rafQ[rafQ.length - 1];
    rafQ.pop();
    fn(i * 16);
  }
  globalThis.requestAnimationFrame = origRaf;
  globalThis.cancelAnimationFrame = origCaf;
  assert.equal(drawn, 1000);
  // ZERO createElement after mount (no re-render, no reconciliation)
  assert.equal(calls.createElement.length, 4);
});

test('attitude indicator: IMU quats drive canvas engine without re-render', () => {
  const { calls, react } = makeReact({ canvas: { getContext: () => ({ fillRect() {} }) } });
  const buf = buildRing([[1, FMT_IMU6DOF, new Uint8Array(64)]]);
  const ring = attachRing(buf);
  const dv = new DataView(buf);
  let tick = 0n;
  const source = {
    acquire() {
      tick++;
      const seq = 8n + tick;
      const base = 128 + (Number(seq - 1n) % 8) * 4096;
      dv.setBigUint64(base, seq, true);
      dv.setBigUint64(24, seq, true);
      return ring.acquire();
    },
  };
  let drawn = 0;
  const Indicator = createWeftAttitudeIndicator(react);
  Indicator({ source, createEngine: ({ ctx }) => ({
    ctx, ok: ctx !== null, frames: 0,
    draw() { drawn++; this.frames++; return true; },
  }) });
  const rafQ = [];
  const origRaf = globalThis.requestAnimationFrame;
  const origCaf = globalThis.cancelAnimationFrame;
  globalThis.requestAnimationFrame = (fn) => (rafQ.push(fn), rafQ.length);
  globalThis.cancelAnimationFrame = () => {};
  calls.effects[0].run();
  for (let i = 0; i < 500; i++) {
    const fn = rafQ[rafQ.length - 1];
    rafQ.pop();
    fn(i * 16);
  }
  globalThis.requestAnimationFrame = origRaf;
  globalThis.cancelAnimationFrame = origCaf;
  assert.equal(drawn, 500);
  assert.equal(calls.createElement.length, 3); // div + canvas + banner, once
});

test('mvp matrix: deterministic output', () => {
  const a = makeMvp(new Float32Array(16), 0.3, 0.5, 3);
  const b = makeMvp(new Float32Array(16), 0.3, 0.5, 3);
  assert.deepEqual(a, b);
  assert.equal(a[15], 1);
});
