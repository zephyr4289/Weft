// ingest/video.js — VideoFrame / ImageData / raw RGBA -> ring, zero intermediate.
//
// Fast path (WebCodecs): VideoFrame.copyTo(slotWritable, {format:'RGBA'})
// writes GPU/plane bytes DIRECTLY into ring slot memory — no base64, no JSON,
// no temporary canvas, no per-frame allocation (Law 1).
// Fallback paths (ImageData / raw) are documented small-allocation or
// caller-owned paths for environments without WebCodecs (Law 3).

import { LayoutError, DLPackCode, fourccFromString } from '../layout.js';

export class VideoFrameIngestor {
  /**
   * @param {import('../ring.js').WeftTensorRing} ring  u8 ring sized for w*h*4
   * @param {object} [opts] { width, height, fourcc='RGBA' }
   */
  constructor(ring, opts = {}) {
    const { code, bits } = ring.layout.dtype;
    if (code !== DLPackCode.UINT || bits !== 8) {
      throw new LayoutError('WTR1_INGEST_DTYPE', 'VideoFrameIngestor needs a u8 ring (RGBA bytes)');
    }
    this._ring = ring;
    this._width = opts.width ?? null;
    this._height = opts.height ?? null;
    this._fourcc = fourccFromString(opts.fourcc ?? 'RGBA');
    // Preallocated meta passthrough (Law 1): finishCommit() reads KNOWN fields;
    // we mutate this scratch object per commit instead of spreading `meta`.
    this._out = { ts: undefined, tsLo: undefined, tsHi: undefined,
      timestampNs: undefined, durationUs: 0, fourcc: 0, rank: undefined, planes: undefined };
    this.stats = { ingested: 0, asyncFastPath: 0, syncFastPath: 0, fallback: 0, dropped: 0 };
    Object.seal(this.stats);
  }

  _applyMeta(meta, durationUs) {
    const o = this._out;
    o.ts = undefined; o.tsLo = undefined; o.tsHi = undefined; o.timestampNs = undefined;
    o.rank = undefined; o.planes = undefined;
    o.fourcc = this._fourcc;
    o.durationUs = durationUs;
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

  _finish(h, written, meta) {
    const byteLen = typeof written === 'number' && written > 0
      ? written
      : h.payloadU8.length; // legacy sync copyTo returns void; slot was sized for the frame
    const seq = this._ring.finishCommit(h, byteLen,
      this._applyMeta(meta, 8333));
    this.stats.ingested++;
    return seq;
  }

  /**
   * WebCodecs fast path. Works with both the sync (legacy) and async (spec)
   * copyTo signatures; the async variant resolves after the GPU readback
   * lands in ring memory. Returns seq (or Promise<seq> for the async path),
   * or null when the frame was dropped (never throws into the frame loop).
   */
  ingestWebCodecs(frame, meta) {
    const h = this._ring.beginCommit();
    let expected;
    try {
      expected = frame.copyTo(h.payloadU8, { format: 'RGBA' });
    } catch (err) {
      this.stats.dropped++;
      return null;
    }
    if (expected && typeof expected.then === 'function') {
      this.stats.asyncFastPath++;
      // Async path: Promise + two closures per frame (documented; use the
      // sync path or ingestRaw in allocation-critical loops).
      return expected.then(
        (written) => this._finish(h, written, meta),
        () => { this.stats.dropped++; return null; },
      );
    }
    this.stats.syncFastPath++;
    return this._finish(h, expected, meta);
  }

  /** ImageData path (2D canvas readbacks). Small view wrapper, documented. */
  ingestImageData(imageData, meta) {
    const data = imageData.data; // Uint8ClampedArray
    return this.ingestBytes(
      new Uint8Array(data.buffer, data.byteOffset, data.byteLength),
      meta,
    );
  }

  /** Raw RGBA path (synthetic feeds, Node pipelines, tests). Zero allocation.
   *  width/height are implicit in the V1 ring shape (rings are fixed-geometry);
   *  `meta` flows through untouched. */
  ingestRaw(rgba, width, height, meta) {
    return this.ingestBytes(rgba, meta);
  }

  ingestBytes(bytes, meta) {
    const h = this._ring.beginCommit();
    const dst = h.payloadU8;
    const n = Math.min(bytes.length, dst.length);
    if (bytes.length !== dst.length) {
      // Short/oversized payloads: bounded loop copy, zero allocation.
      for (let i = 0; i < n; i++) dst[i] = bytes[i];
    } else {
      dst.set(bytes);
    }
    const seq = this._ring.finishCommit(h, n, this._applyMeta(meta, 0));
    this.stats.ingested++;
    this.stats.fallback++;
    return seq;
  }
}
