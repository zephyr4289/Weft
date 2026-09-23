// test/helpers/alloc_probe.mjs — zero-allocation probe (Law 1 hard evidence).
//
// Runs INSIDE `node --expose-gc` (spawned by test/alloc.test.mjs):
//   node --expose-gc helpers/alloc_probe.mjs --mode <mode> --iters <N>
// Forces full GC before and after the steady-state loop and prints
// {"mode":..., "iters":..., "heapBefore":..., "heapAfter":..., "deltaBytes":...}
// Modes: ring | ingest | audio | render | overlay
import { WeftTensorRing, VideoFrameIngestor, AudioPcmFeeder,
         OverlayScratch, DLPackCode } from '../../src/index.js';

const args = process.argv.slice(2);
function arg(name, dflt) {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : dflt;
}
const mode = arg('mode', 'ring');
const iters = parseInt(arg('iters', '100000'), 10);

const U8 = { code: DLPackCode.UINT, bits: 8 };
const F32 = { code: DLPackCode.FLOAT, bits: 32 };

function gc() {
  globalThis.gc();
  globalThis.gc();
}

let run;
if (mode === 'ring') {
  const ring = WeftTensorRing.create({
    slotCount: 4, payloadCap: 256, dtype: F32, shape: [64],
  });
  const src = new Float32Array(64);
  const out = new Uint8Array(256); // reused byte sink (matches payload cap)
  run = (n) => {
    for (let i = 0; i < n; i++) {
      src[0] = i;
      ring.commit(src, { tsLo: i * 1000, tsHi: 0 });
      const v = ring.acquireLatest();
      if (v !== null) v.copyPayloadInto(out);
    }
  };
} else if (mode === 'ingest') {
  const ring = WeftTensorRing.create({
    slotCount: 4, payloadCap: 64, dtype: U8, shape: [4, 4],
  });
  const ing = new VideoFrameIngestor(ring);
  const frame = {
    displayWidth: 4, displayHeight: 2,
    copyTo(dst) { dst[0] = 1; return dst.length; }, // sync WebCodecs-like path
  };
  run = (n) => {
    for (let i = 0; i < n; i++) {
      ing.ingestWebCodecs(frame, { tsLo: i, tsHi: 0 });
      ring.acquireLatest();
    }
  };
} else if (mode === 'audio') {
  const ring = WeftTensorRing.create({
    slotCount: 4, payloadCap: 512, dtype: F32, shape: [128],
  });
  const feeder = new AudioPcmFeeder(ring, { chunkSamples: 128, sampleHz: 48000 });
  const chunk = new Float32Array(128);
  run = (n) => {
    for (let i = 0; i < n; i++) {
      chunk[0] = i;
      feeder.feed(chunk);
      ring.acquireLatest();
    }
  };
} else if (mode === 'render') {
  // Canvas2DPlane draw path with a fake ctx (putImageData is external cost).
  const { Canvas2DPlane } = await import('../../src/index.js');
  const ring = WeftTensorRing.create({
    slotCount: 4, payloadCap: 230400, dtype: U8, shape: [180, 320, 4],
  });
  const fakeCtx = {
    createImageData(w, h) { return { data: new Uint8ClampedArray(w * h * 4), width: w, height: h }; },
    putImageData() {},
  };
  const plane = new Canvas2DPlane({ getContext: (k) => (k === '2d' ? fakeCtx : null) }, ring);
  const src = new Uint8Array(ring.payloadCap);
  run = (n) => {
    for (let i = 0; i < n; i++) {
      const seq = ring.commit(src, { tsLo: i, tsHi: 0 });
      void seq;
      const v = ring.acquireLatest();
      plane.draw(v);
    }
  };
} else if (mode === 'overlay') {
  const scratch = new OverlayScratch(256);
  const out = [0, 0, 0, 0, 0, 0]; // reused record — boxAt writes into it
  run = (n) => {
    for (let i = 0; i < n; i++) {
      scratch.clear();
      for (let b = 0; b < 8; b++) scratch.pushBox(b, b, 10, 10, 0.9, b);
      for (let b = 0; b < 8; b++) scratch.boxAt(b, out);
    }
  };
} else {
  console.error(`unknown mode ${mode}`);
  process.exit(2);
}

// STEADY-STATE methodology: warm up first (JIT/IC metadata is one-time
// allocation, not per-frame garbage), gc, snapshot, THEN measure.
const WARMUP = Math.min(20000, iters);
run(WARMUP);
gc();
const heapBefore = process.memoryUsage().heapUsed;
run(iters);
gc();
const heapAfter = process.memoryUsage().heapUsed;
console.log(JSON.stringify({
  mode, iters,
  heapBefore, heapAfter,
  deltaBytes: heapAfter - heapBefore,
}));
