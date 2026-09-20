// test/runtime.test.mjs — Law 3: runtime detection matrix + shareability gate.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { detectRuntime, runtimeMatrix, assertShareable } from '../src/runtime.js';

test('detectRuntime identifies node (this process)', () => {
  const r = detectRuntime();
  assert.equal(r.node, true);
  assert.equal(r.atomics, true);
  assert.equal(r.sharedArrayBuffer, true);
  assert.equal(runtimeMatrix().where, 'node');
});

test('detectRuntime: browser / deno / bun / worker / electron fakes', () => {
  const browser = detectRuntime({ window: {}, SharedArrayBuffer: function SAB() {}, Atomics: { store() {} } });
  assert.equal(browser.browser, true);
  assert.equal(browser.node, false);
  assert.equal(browser.sharedArrayBuffer, true);

  const denoGlobals = { Deno: { version: { deno: '2.0' } }, Atomics: {} };
  assert.equal(detectRuntime(denoGlobals).deno, true);
  assert.equal(runtimeMatrix(denoGlobals).where, 'deno');

  const bunGlobals = { Bun: { version: '1.1' }, Atomics: {} };
  assert.equal(runtimeMatrix(bunGlobals).where, 'bun');

  const worker = detectRuntime({ importScripts: function () {}, Atomics: {} });
  assert.equal(worker.worker, true);
  assert.equal(worker.browser, true, 'workers count as browser-family');

  const electron = detectRuntime({
    process: { versions: { node: '20' } }, window: {}, Atomics: {},
  });
  assert.equal(electron.electron, true);

  const webcodecs = detectRuntime({ VideoFrame: function () {} });
  assert.equal(webcodecs.webCodecs, true);
});

test('runtimeMatrix feature list stays sorted/deterministic', () => {
  const m = runtimeMatrix({});
  assert.equal(m.where, 'unknown');
  assert.deepEqual(m.features, ['esm']);
});

test('assertShareable accepts SAB, rejects plain ArrayBuffer', () => {
  const sab = new SharedArrayBuffer(8);
  assert.doesNotThrow(() => assertShareable(sab, 'test'));
  assert.throws(() => assertShareable(new ArrayBuffer(8), 'test'), (e) =>
    e instanceof TypeError && /not a SharedArrayBuffer/.test(e.message));
});
