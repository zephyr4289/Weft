// synth.mjs — synthetic camera: procedural moving-blob scene committed
// straight into a WTR1 ring. Zero per-frame allocation (Law 1): one
// preallocated RGBA scratch, pure index math, commit via typed set.

export class SyntheticCamera {
  /**
   * @param {import('../../../packages/weft-tensor/src/index.js').WeftTensorRing} ring
   * @param {object} opts { width, height, blobs, hz }
   */
  constructor(ring, opts = {}) {
    const { width = 160, height = 90, blobs = 3, hz = 120 } = opts;
    this.ring = ring;
    this.width = width;
    this.height = height;
    this.blobs = blobs;
    this.hz = hz;
    this.seq = 0;
    // Preallocated frame scratch (single, reused — Law 1)
    this._frame = new Uint8Array(width * height * 4);
    this._bg = Math.floor(255 * 0.12); // dark studio background
  }

  /** Produce the next frame and commit it. Returns the committed seq. */
  tick() {
    this.seq++;
    const f = this._frame;
    const { width: W, height: H, seq } = this;
    f.fill(this._bg);
    // Moving bright blobs — positions are smooth sinusoids (deterministic).
    for (let b = 0; b < this.blobs; b++) {
      const ph = (seq / (this.hz * 1.7)) + b * 2.1;
      const cx = (0.5 + 0.38 * Math.sin(ph * 1.3 + b)) * W;
      const cy = (0.5 + 0.36 * Math.cos(ph * 0.9 + b * 1.7)) * H;
      const r = 8 + 3 * Math.sin(ph * 2.2 + b);
      this._blob(f, cx, cy, r, 235 - b * 25);
    }
    return this.ring.commit(this._frame, {
      ts: this.seq * Math.round(1e9 / this.hz),
      fourcc: 'RGBA',
    });
  }

  _blob(f, cx, cy, r, intensity) {
    const W = this.width, H = this.height;
    const x0 = Math.max(1, Math.floor(cx - r)), x1 = Math.min(W - 2, Math.ceil(cx + r));
    const y0 = Math.max(1, Math.floor(cy - r)), y1 = Math.min(H - 2, Math.ceil(cy + r));
    const r2 = r * r;
    for (let y = y0; y <= y1; y++) {
      const dy2 = (y - cy) * (y - cy);
      for (let x = x0; x <= x1; x++) {
        const dx = x - cx;
        const d2 = dx * dx + dy2;
        if (d2 <= r2) {
          const edge = 1 - d2 / r2; // soft edge, no allocations
          const v = this._bg + (intensity - this._bg) * (0.55 + 0.45 * edge);
          const o = (y * W + x) * 4;
          f[o] = v; f[o + 1] = v; f[o + 2] = v; f[o + 3] = 255;
        }
      }
    }
  }
}
