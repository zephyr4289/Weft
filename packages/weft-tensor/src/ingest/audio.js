// ingest/audio.js — AudioPcmFeeder: continuous interleaved PCM -> ring.
//
// Fixed-size slots (channels x chunkSamples f32). Chunks may arrive in any
// size; the feeder streams them across slot boundaries WITHOUT allocating
// (Law 1): one preallocated carry handle, bounded per-segment loops.
// Target: 48kHz mono/stereo commit < 50us per chunk (bench/commit.bench.mjs).

import { LayoutError, DLPackCode, fourccFromString } from '../layout.js';

export class AudioPcmFeeder {
  /**
   * @param {import('../ring.js').WeftTensorRing} ring  f32 ring, cap >= chunkSamples
   * @param {object} opts { channels, chunkSamples, sampleHz }
   */
  constructor(ring, opts) {
    const { code, bits } = ring.layout.dtype;
    if (code !== DLPackCode.FLOAT || bits !== 32) {
      throw new LayoutError('WTR1_INGEST_DTYPE', 'AudioPcmFeeder needs an f32 ring (interleaved PCM)');
    }
    const { channels = 1, chunkSamples, sampleHz = 48000 } = opts;
    if (!Number.isInteger(chunkSamples) || chunkSamples < 1) {
      throw new LayoutError('WTR1_AUDIO_CHUNK', `chunkSamples ${chunkSamples} invalid`);
    }
    if (chunkSamples * 4 > ring.payloadCap) {
      throw new LayoutError('WTR1_AUDIO_CHUNK', `chunkSamples ${chunkSamples} exceeds slot cap ${ring.payloadCap}`);
    }
    this._ring = ring;
    this._channels = channels;
    this._chunkSamples = chunkSamples;
    this._sampleHz = sampleHz;
    this._fourcc = fourccFromString('PCM ');
    this._open = null;      // reused beginCommit handle
    this._openPos = 0;      // samples already written into the open slot
    this._carry = 0;        // samples carried across feed() calls
    // Preallocated meta scratch (Law 1) — no spread, no per-chunk objects.
    this._out = { ts: undefined, tsLo: undefined, tsHi: undefined,
      timestampNs: undefined, durationUs: 0, fourcc: 0, rank: undefined, planes: undefined };
    this.stats = { chunks: 0, samples: 0, slotRolls: 0, partials: 0 };
    Object.seal(this.stats);
  }

  _applyMeta(meta) {
    const o = this._out;
    o.ts = undefined; o.tsLo = undefined; o.tsHi = undefined; o.timestampNs = undefined;
    o.rank = undefined; o.planes = undefined;
    o.fourcc = this._fourcc;
    o.durationUs = Math.round((this._chunkSamples / this._sampleHz) * 1e6);
    if (meta) {
      if (meta.ts !== undefined) o.ts = meta.ts;
      if (meta.tsLo !== undefined) o.tsLo = meta.tsLo;
      if (meta.tsHi !== undefined) o.tsHi = meta.tsHi;
      if (meta.timestampNs !== undefined) o.timestampNs = meta.timestampNs;
      if (meta.durationUs !== undefined) o.durationUs = meta.durationUs;
      if (meta.rank !== undefined) o.rank = meta.rank;
      if (meta.planes !== undefined) o.planes = meta.planes;
    }
    return o;
  }

  get channels() { return this._channels; }
  get chunkSamples() { return this._chunkSamples; }
  get sampleHz() { return this._sampleHz; }
  /** Slot capacity in samples after 64B stride padding (informational). */
  get slotSamples() { return this._ring.payloadCap / 4; }

  /**
   * Feed interleaved PCM (Float32Array). Any length; the feeder rolls slots
   * internally. Zero allocation on every path.
   * @returns {number} highest seq committed by this call (or last open seq)
   */
  feed(chunk, meta) {
    let pos = 0;
    let lastSeq = this._open !== null ? this._open.seq : 0;
    while (pos < chunk.length) {
      if (this._open === null) {
        this._open = this._ring.beginCommit();
        this._openPos = 0;
      }
      const dst = this._open.payloadTyped; // Float32Array over the slot cap
      const room = this._chunkSamples - this._openPos; // roll at the DECLARED quantum
      const take = Math.min(room, chunk.length - pos);
      // Bounded segment copy (typically 128..1024 samples) — zero alloc.
      for (let i = 0; i < take; i++) dst[this._openPos + i] = chunk[pos + i];
      this._openPos += take;
      pos += take;
      this._carry += take;
      if (this._openPos === this._chunkSamples) {
        lastSeq = this._ring.finishCommit(this._open, this._chunkSamples * 4, this._applyMeta(meta));
        this._open = null;
        this.stats.slotRolls++;
      } else {
        this.stats.partials++;
      }
    }
    this.stats.chunks++;
    this.stats.samples += chunk.length;
    return lastSeq;
  }

  /** Flush a trailing partial slot (end of stream / shutdown). */
  flush(meta) {
    if (this._open === null || this._openPos === 0) return null;
    const byteLen = this._openPos * 4;
    const out = this._applyMeta(meta);
    const seq = this._ring.finishCommit(this._open, byteLen, out);
    this._open = null;
    this._openPos = 0;
    return seq;
  }
}
