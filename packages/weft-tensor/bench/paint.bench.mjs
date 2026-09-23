// bench/paint.bench.mjs — 120 Hz paint-loop simulation through RingListener +
// Canvas2DPlane (fake ctx). Reports tick pacing and per-frame draw cost.
import { WeftTensorRing, Canvas2DPlane, RingListener, timeoutPump, DLPackCode } from '../src/index.js';

const W = 320, H = 180;
const ring = WeftTensorRing.create({
  slotCount: 4, payloadCap: W * H * 4, dtype: { code: DLPackCode.UINT, bits: 8 },
  shape: [H, W, 4], tickHz: 120, fourcc: 'RGBA',
});
const fakeCtx = {
  createImageData(w, h) { return { data: new Uint8ClampedArray(w * h * 4), width: w, height: h }; },
  putImageData() {},
};
const plane = new Canvas2DPlane({ getContext: (k) => (k === '2d' ? fakeCtx : null) }, ring);

// producer at ~120 Hz on a timer (simulated camera)
const src = new Uint8Array(ring.payloadCap);
let produced = 0;
const producer = setInterval(() => {
  src[0] = (produced++) & 0xff;
  ring.commit(src, { tsLo: produced, tsHi: 0 });
}, 8);

// render pump at 120 Hz
const drawTimes = [];
const listener = new RingListener(ring, {
  pump: timeoutPump(8),
  onFrame(frame) {
    const t0 = process.hrtime.bigint();
    plane.draw(frame);
    drawTimes.push(Number(process.hrtime.bigint() - t0) / 1000);
  },
});
listener.start();

setTimeout(() => {
  listener.stop();
  clearInterval(producer);
  drawTimes.sort((a, b) => a - b);
  const q = (p) => +drawTimes[Math.floor(p * (drawTimes.length - 1))].toFixed(3);
  const stats = listener.stats;
  console.log(JSON.stringify({
    bench: 'weft-tensor 120Hz paint loop',
    node: process.version,
    wallSeconds: 2,
    ticks: stats.ticks,
    framesRendered: stats.frames,
    stalls: stats.stalls,
    fps: +(stats.frames / 2).toFixed(1),
    draw_us: { p50: q(0.5), p99: q(0.99), max: q(1) },
    note: 'fake ctx: measures our zero-alloc path cost only (putImageData is external)',
  }));
  process.exit(0);
}, 2000);
