// bench/commit.bench.mjs — ingestion & acquire latency (Law 1 + <50us claim).
// Output: single JSON line (machine-readable evidence for the D-30 report).
import { WeftTensorRing, VideoFrameIngestor, AudioPcmFeeder, DLPackCode } from '../src/index.js';

function quantile(arr, q) {
  const i = Math.min(arr.length - 1, Math.floor(q * arr.length));
  return arr[i];
}
function bench(fn, n) {
  const times = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    const t0 = process.hrtime.bigint();
    fn(i);
    const t1 = process.hrtime.bigint();
    times[i] = Number(t1 - t0) / 1000; // us
  }
  const sorted = Float64Array.from(times).sort();
  return {
    n,
    p50_us: +quantile(sorted, 0.50).toFixed(3),
    p99_us: +quantile(sorted, 0.99).toFixed(3),
    max_us: +sorted[sorted.length - 1].toFixed(3),
  };
}

// -- video: 320x180 RGBA frames through the WebCodecs-shaped fast path ------
const videoRing = WeftTensorRing.create({
  slotCount: 4, payloadCap: 320 * 180 * 4, dtype: { code: DLPackCode.UINT, bits: 8 },
  shape: [180, 320, 4], tickHz: 120, fourcc: 'RGBA',
});
const ingestor = new VideoFrameIngestor(videoRing);
const fakeFrame = {
  displayWidth: 320, displayHeight: 180,
  copyTo(dst) { dst[0] = 1; return dst.length; }, // sync WebCodecs path (bytes land in ring)
};
// warmup
for (let i = 0; i < 5000; i++) { ingestor.ingestWebCodecs(fakeFrame); videoRing.acquireLatest(); }
const videoCommit = bench((i) => ingestor.ingestWebCodecs(fakeFrame, { tsLo: i, tsHi: 0 }), 50000);
const videoAcquire = bench(() => videoRing.acquireLatest(), 50000);

// -- audio: 2ch x 128 f32 (5.33ms quanta @48k) -------------------------------
const audioRing = WeftTensorRing.create({
  slotCount: 8, payloadCap: 2 * 128 * 4, dtype: { code: DLPackCode.FLOAT, bits: 32 },
  shape: [2, 128], tickHz: 9375, fourcc: 'PCM ',
});
const feeder = new AudioPcmFeeder(audioRing, { channels: 2, chunkSamples: 256, sampleHz: 48000 });
const chunk = new Float32Array(256);
for (let i = 0; i < 5000; i++) feeder.feed(chunk);
const audioCommit = bench((i) => { chunk[0] = i; feeder.feed(chunk); }, 50000);
const audioAcquire = bench(() => audioRing.acquireLatest(), 50000);

console.log(JSON.stringify({
  bench: 'weft-tensor commit/acquire',
  node: process.version,
  video: {
    geometry: '320x180 RGBA', slotCapBytes: videoRing.payloadCap,
    commit: videoCommit, acquire: videoAcquire,
    commit_under_50us: videoCommit.p99_us < 50,
  },
  audio: {
    geometry: '2ch x 128 f32 (interleaved quanta 256)', slotCapBytes: audioRing.payloadCap,
    commit: audioCommit, acquire: audioAcquire,
    commit_under_50us: audioCommit.p99_us < 50,
  },
}));
