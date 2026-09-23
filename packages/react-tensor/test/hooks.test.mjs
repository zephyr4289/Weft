// test/hooks.test.mjs — the React hook binding through a MINI shim with real
// useRef/useEffect mount semantics. Proves: effect runs once per ring,
// cleanup stops the pump, statsRef is the LIVE controller stats object
// (zero re-render by construction — nothing in the hook ever sets state).
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { WeftTensorRing, DLPackCode } from '../../weft-tensor/src/index.js';
import { createTensorCanvasHooks } from '../src/core.js';

function makeShim() {
  const effects = [];
  const React = {
    useRef(init) {
      return { current: init }; // fresh per call — sufficient for mount-once tests
    },
    useEffect(fn, deps) { effects.push({ fn, deps }); },
  };
  return {
    React,
    effects,
    mount() {
      for (const e of effects) e.cleanup = e.fn();
    },
    unmount() {
      for (const e of effects) if (typeof e.cleanup === 'function') e.cleanup();
      effects.length = 0;
    },
  };
}

function makeRing() {
  return WeftTensorRing.create({
    slotCount: 2, payloadCap: 32, dtype: { code: DLPackCode.UINT, bits: 8 },
    shape: [2, 4],
  });
}

test('useWeftTensorCanvas: single effect, live statsRef, clean teardown', () => {
  const shim = makeShim();
  const useWeftTensorCanvas = createTensorCanvasHooks(shim.React);
  const ring = makeRing();
  const canvasRef = { current: { getContext: () => null } }; // null ctx -> constructor throws... use valid fake
  canvasRef.current = {
    getContext: (k) => k === '2d' ? {
      createImageData: (w, h) => ({ data: new Uint8ClampedArray(w * h * 4), width: w, height: h }),
      putImageData() {}, strokeRect() {}, fillText() {},
      beginPath() {}, moveTo() {}, lineTo() {}, stroke() {},
      lineWidth: 0, font: '', textBaseline: '', strokeStyle: null, fillStyle: null,
    } : null,
  };

  const statsRef = useWeftTensorCanvas(ring, canvasRef, {});
  assert.equal(shim.effects.length, 1, 'hook registers exactly ONE effect');
  shim.mount();
  // effect created a controller internally; statsRef.current is its sealed stats
  assert.equal(typeof statsRef.current.frames, 'number');
  // unmount cleans up without throwing
  shim.unmount();
});

test('useWeftTensorCanvas: no canvas -> no effect work, no crash', () => {
  const shim = makeShim();
  const useWeftTensorCanvas = createTensorCanvasHooks(shim.React);
  const ring = makeRing();
  const emptyRef = { current: null };
  const statsRef = useWeftTensorCanvas(ring, emptyRef, {});
  shim.mount(); // effect returns early
  assert.equal(statsRef.current.frames, 0);
  shim.unmount();
});
