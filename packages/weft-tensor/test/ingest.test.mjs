// test/ingest.test.mjs — VideoFrameIngestor (sync/async WebCodecs, ImageData,
// raw) + AudioPcmFeeder (exact fit, cross-slot carry, flush) — Law 1/3.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  WeftTensorRing, VideoFrameIngestor, AudioPcmFeeder, DLPackCode,
} from '../src/index.js';

const U8 = { code: DLPackCode.UINT, bits: 8 };
const F32 = { code: DLPackCode.FLOAT, bits: 32 };

function makeVideoRing(w = 4, h = 2) {
  return WeftTensorRing.create({
    slotCount: 3, payloadCap: w * h * 4, dtype: U8, shape: [h, w, 4],
  });
}

/** Fake WebCodecs VideoFrame: writes RGBA into the destination like copyTo. */
function fakeFrame(r, g, b, mode = 'sync', fail = false) {
  const frame = {
    displayWidth: 4, displayHeight: 2,
    copyTo(dst, opts) {
      assert.equal(opts.format, 'RGBA');
      if (fail) throw new Error('copyTo exploded');
      // paint deterministic pixels
      for (let i = 0; i < dst.length; i += 4) {
        dst[i] = r; dst[i + 1] = g; dst[i + 2] = b; dst[i + 3] = 255;
      }
      if (mode === 'async') {
        return Promise.resolve(dst.length); // spec: Promise<number>
      }
      return dst.length; // new sync signature
    },
  };
  return frame;
}

test('WebCodecs sync fast path: bytes land in ring, no intermediate', async () => {
  const ring = makeVideoRing();
  const ing = new VideoFrameIngestor(ring);
  const seq = ing.ingestWebCodecs(fakeFrame(10, 20, 30), { timestampNs: 111 });
  assert.equal(seq, 1);
  assert.equal(ing.stats.syncFastPath, 1);
  assert.equal(ing.stats.ingested, 1);
  const v = ring.acquireLatest();
  assert.equal(v.seq, 1);
  assert.equal(v.timestampNs, 111);
  assert.equal(v.fourcc, 'RGBA');
  assert.equal(v.getU8(0), 10);
  assert.equal(v.getU8(1), 20);
  assert.equal(v.getU8(2), 30);
  assert.equal(v.getU8(3), 255);
});

test('WebCodecs async fast path resolves after the GPU readback lands', async () => {
  const ring = makeVideoRing();
  const ing = new VideoFrameIngestor(ring);
  const p = ing.ingestWebCodecs(fakeFrame(1, 2, 3, 'async'), { timestampNs: 7 });
  assert.equal(ring.producerSeq, 0, 'nothing published until copyTo resolves');
  const seq = await p;
  assert.equal(seq, 1);
  assert.equal(ing.stats.asyncFastPath, 1);
  assert.equal(ring.acquireLatest().getU8(0), 1);
});

test('copyTo failure is a DROP, never a throw into the frame loop', () => {
  const ring = makeVideoRing();
  const ing = new VideoFrameIngestor(ring);
  assert.equal(ing.ingestWebCodecs(fakeFrame(0, 0, 0, 'sync', true)), null);
  assert.equal(ing.stats.dropped, 1);
  assert.equal(ring.producerSeq, 0);
});

test('legacy sync copyTo returning void still commits the full slot', () => {
  const ring = makeVideoRing();
  const ing = new VideoFrameIngestor(ring);
  const legacy = {
    displayWidth: 4, displayHeight: 2,
    copyTo(dst) {
      dst.fill(7);
      return undefined; // legacy signature returns void
    },
  };
  const seq = ing.ingestWebCodecs(legacy);
  assert.equal(seq, 1);
  const v = ring.acquireLatest();
  // dst is the padded slot payload view (64B-aligned stride) — the whole cap
  // was filled and committed; payload_len reflects the live bytes.
  assert.equal(v.payloadLength, ring.payloadCap);
  assert.equal(v.getU8(v.payloadLength - 1), 7);
});

test('ImageData + raw paths', () => {
  const ring = makeVideoRing();
  const ing = new VideoFrameIngestor(ring);
  const data = new Uint8ClampedArray(4 * 2 * 4);
  data[0] = 200;
  const img = { data, width: 4, height: 2 };
  ing.ingestImageData(img);
  assert.equal(ring.acquireLatest().getU8(0), 200);
  const raw = new Uint8Array(32).fill(9);
  ing.ingestRaw(raw, 4, 2);
  const v = ring.acquireLatest();
  assert.equal(v.getU8(31), 9);
  assert.equal(ing.stats.fallback, 2);
});

test('ingestor dtype gate: f32 ring rejected (Law 4)', () => {
  const f32ring = WeftTensorRing.create({
    slotCount: 2, payloadCap: 32, dtype: F32, shape: [8],
  });
  assert.throws(() => new VideoFrameIngestor(f32ring), (e) =>
    e.code === 'WTR1_INGEST_DTYPE');
});

// ------------------------------------------------------------------ audio

function makeAudioRing(chunkSamples = 8) {
  return WeftTensorRing.create({
    slotCount: 4, payloadCap: chunkSamples * 4, dtype: F32, shape: [chunkSamples],
  });
}

test('AudioPcmFeeder: exact-fit chunks roll one slot per chunk', () => {
  const ring = makeAudioRing(8);
  const feeder = new AudioPcmFeeder(ring, { chunkSamples: 8, sampleHz: 48000 });
  const chunk = new Float32Array(8);
  for (let i = 0; i < 8; i++) chunk[i] = i * 0.25;
  const seq = feeder.feed(chunk);
  assert.equal(seq, 1);
  assert.equal(feeder.stats.slotRolls, 1);
  const v = ring.acquireLatest();
  assert.equal(v.fourcc, 'PCM ');
  assert.equal(v.payloadLength, 32, 'payload_len is the DECLARED quantum, not the padded cap');
  assert.equal(v.durationUs, Math.round(8 / 48000 * 1e6));
  for (let i = 0; i < 8; i++) assert.equal(v.getF32(i), i * 0.25);
});

test('AudioPcmFeeder: chunk spanning slot boundaries carries with zero gaps', () => {
  const ring = makeAudioRing(8);
  const feeder = new AudioPcmFeeder(ring, { chunkSamples: 8, sampleHz: 16000 });
  // One 20-sample chunk across 8+8+4 -> two full slots + one partial.
  const chunk = new Float32Array(20);
  for (let i = 0; i < 20; i++) chunk[i] = i + 0.5;
  const seq = feeder.feed(chunk);
  assert.equal(seq, 2); // two full slots committed
  assert.equal(feeder.stats.slotRolls, 2);
  assert.equal(feeder.stats.partials, 1);
  for (let s = 1; s <= 2; s++) {
    const v = ring.acquireFrame(s);
    for (let i = 0; i < 8; i++) {
      assert.equal(v.getF32(i), (s - 1) * 8 + i + 0.5, `slot ${s} sample ${i}`);
    }
  }
  // Flush the 4-sample tail.
  const tail = feeder.flush();
  assert.equal(tail, 3);
  const v3 = ring.acquireFrame(3);
  assert.equal(v3.payloadLength, 16, '4 samples * 4B');
  assert.equal(v3.getF32(3), 19.5);
});

test('AudioPcmFeeder: tiny chunks accumulate then roll (worklet cadence)', () => {
  const ring = makeAudioRing(4);
  const feeder = new AudioPcmFeeder(ring, { chunkSamples: 4, sampleHz: 44100 });
  const tiny = new Float32Array(1);
  for (let i = 0; i < 9; i++) {
    tiny[0] = i;
    feeder.feed(tiny);
  }
  assert.equal(feeder.stats.slotRolls, 2, '9 samples -> 2 full 4-sample slots');
  const v2 = ring.acquireFrame(2);
  assert.equal(v2.getF32(0), 4); // samples 4..7 live in slot 2
  assert.equal(v2.getF32(3), 7);
});

test('AudioPcmFeeder dtype gate + chunk size gate (Law 4)', () => {
  const u8ring = makeVideoRing();
  assert.throws(() => new AudioPcmFeeder(u8ring, { chunkSamples: 4 }), (e) =>
    e.code === 'WTR1_INGEST_DTYPE');
  const ring = makeAudioRing(4);
  assert.throws(() => new AudioPcmFeeder(ring, { chunkSamples: 0 }), (e) =>
    e.code === 'WTR1_AUDIO_CHUNK');
  assert.throws(() => new AudioPcmFeeder(ring, { chunkSamples: 999 }), (e) =>
    e.code === 'WTR1_AUDIO_CHUNK');
});
