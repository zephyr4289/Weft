// test/render.test.mjs — Canvas2D plane + overlay scratch with fake contexts,
// WebGL2 plane with a recording fake GL (Law 1 zero-alloc paths, pixel parity).
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  WeftTensorRing, Canvas2DPlane, WebGL2Plane, OverlayScratch,
  drawBoxes2D, drawSkeleton2D, COCO17_EDGES, DLPackCode,
} from '../src/index.js';

const U8 = { code: DLPackCode.UINT, bits: 8 };

function makeRing(w = 8, h = 4) {
  return WeftTensorRing.create({
    slotCount: 3, payloadCap: w * h * 4, dtype: U8, shape: [h, w, 4],
  });
}

/** Fake 2D context: records putImageData buffers. */
function fakeCtx2D(w, h) {
  const calls = { puts: [], rects: [], lines: [], texts: [] };
  return {
    calls,
    createImageData(iw, ih) { return { data: new Uint8ClampedArray(iw * ih * 4), width: iw, height: ih }; },
    putImageData(img) { calls.puts.push(img); },
    strokeRect(x, y, rw, rh) { calls.rects.push([x, y, rw, rh]); },
    fillText(t, x, y) { calls.texts.push(t); },
    beginPath() { calls.lines.push('begin'); },
    moveTo() {}, lineTo() {}, stroke() { calls.lines.push('stroke'); },
    lineWidth: 0, font: '', textBaseline: '', strokeStyle: null, fillStyle: null,
  };
}

function fakeCanvas(ctx) {
  return { getContext: (kind) => (kind === '2d' ? ctx : null), width: 8, height: 4 };
}

test('Canvas2DPlane: ONE ImageData reused, pixel-accurate blit per frame', () => {
  const ring = makeRing();
  const ctx = fakeCtx2D(8, 4);
  const plane = new Canvas2DPlane(fakeCanvas(ctx), ring);
  assert.equal(plane.width, 8);
  assert.equal(plane.height, 4);

  const rgba = new Uint8Array(8 * 4 * 4);
  for (let i = 0; i < rgba.length; i += 4) { rgba[i] = 255 - i; rgba[i + 1] = i; }
  ring.commit(rgba, { ts: 5n });
  plane.draw(ring.acquireLatest());

  assert.equal(ctx.calls.puts.length, 1);
  assert.equal(ctx.calls.puts[0].data[0], 255);        // first pixel R (255 - 0)
  assert.equal(ctx.calls.puts[0].data[1], 0);          // first pixel G (i = 0)
  assert.equal(ctx.calls.puts[0].data[5], 4);          // second pixel G (i = 4)
  // Second frame REUSES the same ImageData object (Law 1)
  const imgObj = ctx.calls.puts[0];
  ring.commit(new Uint8Array(rgba).fill(3), { ts: 6n });
  plane.draw(ring.acquireLatest());
  assert.equal(ctx.calls.puts.length, 2);
  assert.ok(ctx.calls.puts[1] === imgObj, 'ImageData instance reused, never rebuilt');
  assert.equal(ctx.calls.puts[1].data[0], 3);
  // Underrun: null keeps last frame on screen, counted as reuse
  plane.draw(null);
  assert.equal(plane.stats.reused, 1);
  assert.equal(plane.stats.draws, 3);
});

test('Canvas2DPlane dtype gate (Law 4)', () => {
  const f32ring = WeftTensorRing.create({
    slotCount: 2, payloadCap: 32, dtype: { code: DLPackCode.FLOAT, bits: 32 }, shape: [8],
  });
  const ctx = fakeCtx2D(8, 4);
  assert.throws(() => new Canvas2DPlane(fakeCanvas(ctx), f32ring), (e) =>
    e.code === 'WTR1_RENDER_DTYPE');
});

test('OverlayScratch + drawBoxes2D: bounded, reused, pixel records', () => {
  const scratch = new OverlayScratch(4);
  for (let i = 0; i < 6; i++) {
    const ok = scratch.pushBox(i, i, 2, 2, 0.5, i);
    if (i < 4) assert.ok(ok); else assert.equal(ok, false, 'bounded at 4');
  }
  assert.equal(scratch.count, 4);
  const out = [0, 0, 0, 0, 0, 0];
  scratch.boxAt(2, out);
  assert.deepEqual(out, [2, 2, 2, 2, 0.5, 2]);

  const ctx = fakeCtx2D(8, 4);
  const n = drawBoxes2D(ctx, scratch);
  assert.equal(n, 4);
  assert.equal(ctx.calls.rects.length, 4);
  assert.equal(ctx.calls.rects[3][0], 3);
  assert.equal(ctx.calls.texts.length, 4);
  scratch.clear();
  assert.equal(drawBoxes2D(ctx, scratch), 0);
});

test('drawSkeleton2D honours confidence gating', () => {
  const kp = new Float32Array(17 * 3);
  // nose(0)-leftEye(1) visible pair
  kp[0] = 1; kp[1] = 1; kp[2] = 0.9;      // nose
  kp[3] = 2; kp[4] = 2; kp[5] = 0.9;      // left eye
  kp[6] = 3; kp[7] = 3; kp[8] = 0.1;      // right eye — BELOW confidence
  const ctx = fakeCtx2D(8, 4);
  drawSkeleton2D(ctx, kp, 17, 0.5);
  // nose-leftEye drawn; nose-rightEye gated
  assert.equal(ctx.calls.lines.filter((s) => s === 'stroke').length, 1);
  assert.equal(COCO17_EDGES.length % 2, 0);
});

// ---------------------------------------------------------- WebGL2 fake GL

function fakeGL() {
  const calls = { texSub: [], drawArrays: [], shaders: [] };
  const gl = {
    calls,
    TEXTURE_2D: 1, RGBA8: 2, RGBA: 3, UNSIGNED_BYTE: 4, TRIANGLES: 5,
    VERTEX_SHADER: 6, FRAGMENT_SHADER: 7, COMPILE_STATUS: 8, LINK_STATUS: 9,
    TEXTURE_MIN_FILTER: 10, TEXTURE_MAG_FILTER: 11, LINEAR: 12,
    TEXTURE_WRAP_S: 13, TEXTURE_WRAP_T: 14, CLAMP_TO_EDGE: 15,
    createShader: (t) => ({ t }), shaderSource() {}, compileShader() {},
    getShaderParameter: () => true,
    createProgram: () => ({}), attachShader() {}, linkProgram() {},
    getProgramParameter: () => true, deleteShader() {}, useProgram() {},
    createTexture: () => ({}), bindTexture() {}, deleteTexture() {}, deleteProgram() {},
    texParameteri() {}, texImage2D(...a) { calls.texImage = a; },
    texSubImage2D(...a) { calls.texSub.push(a); },
    viewport() {}, drawArrays(...a) { calls.drawArrays.push(a); },
  };
  return gl;
}

test('WebGL2Plane: zero-alloc texSubImage2D from the slot view + fullscreen tri', () => {
  const ring = makeRing(8, 4);
  const gl = fakeGL();
  const canvas = { getContext: (k) => (k === 'webgl2' ? gl : null), width: 8, height: 4 };
  const plane = new WebGL2Plane(canvas, ring);
  assert.equal(plane.width, 8);

  const rgba = new Uint8Array(ring.payloadCap).fill(5);
  ring.commit(rgba, { ts: 1n });
  const view = ring.acquireLatest();
  plane.draw(view);

  assert.equal(gl.calls.texSub.length, 1);
  const sub = gl.calls.texSub[0];
  assert.equal(sub[2], 0);   // xoffset
  assert.equal(sub[3], 0);   // yoffset
  assert.equal(sub[4], 8);   // w
  assert.equal(sub[5], 4);   // h
  assert.ok(sub[8] === view.payloadView(), 'uploads read the PREALLOCATED slot view');
  assert.equal(sub[9], 0);   // srcOffset elements
  assert.equal(gl.calls.drawArrays.length, 1);
  assert.equal(gl.calls.drawArrays[0][0], gl.TRIANGLES);
  // null frame -> keeps texture, still composites
  plane.draw(null);
  assert.equal(gl.calls.drawArrays.length, 2);
  assert.equal(plane.stats.uploads, 1);
  plane.dispose();
});
