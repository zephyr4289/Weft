// render/canvas2d.js — Canvas2D plane: ring bytes -> pixels, zero allocation.
//
// ONE ImageData preallocated at attach (Law 1). Per frame:
//   payload memcpy (typed .set) + putImageData — that's the whole cost.
// No ImageData construction, no context state churn, no GC spikes.

import { LayoutError, DLPackCode } from '../layout.js';

export class Canvas2DPlane {
  /**
   * @param {HTMLCanvasElement|OffscreenCanvas} canvas
   * @param {import('../ring.js').WeftTensorRing} ring  u8 RGBA ring
   * @param {object} [opts] { width, height }
   */
  constructor(canvas, ring, opts = {}) {
    const { code, bits } = ring.layout.dtype;
    if (code !== DLPackCode.UINT || bits !== 8) {
      throw new LayoutError('WTR1_RENDER_DTYPE', 'Canvas2DPlane needs a u8 RGBA ring');
    }
    const L = ring.layout;
    let rank = L.rank;
    const w = opts.width ?? L.shape[rank - 2];   // [..., h, w, 4] convention
    const h = opts.height ?? L.shape[rank - 3];
    if (!w || !h) throw new LayoutError('WTR1_RENDER_SHAPE', 'cannot infer canvas size from ring shape — pass {width, height}');
    const ctx = canvas.getContext('2d', { alpha: false, desynchronized: true });
    if (ctx === null) throw new LayoutError('WTR1_NO_CTX', 'canvas 2d context unavailable');
    this._canvas = canvas;
    this._ctx = ctx;
    this._w = w; this._h = h;
    // ONE ImageData for the lifetime of the plane.
    this._imageData = ctx.createImageData(w, h);
    this._clamped = this._imageData.data;
    this._ring = ring;
    this.stats = { draws: 0, frames: 0, reused: 0 };
    Object.seal(this.stats);
  }

  get width() { return this._w; }
  get height() { return this._h; }

  /**
   * Draw one acquired frame (or keep the last frame when `frame` is null —
   * e.g. ring underrun — so the canvas never flickers).
   */
  draw(frame) {
    if (frame !== null && frame !== undefined) {
      // Exact-cap fast path inside copyPayloadInto -> single typed .set.
      frame.copyPayloadInto(this._clamped);
      this.stats.frames++;
    } else {
      this.stats.reused++;
    }
    this._ctx.putImageData(this._imageData, 0, 0);
    this.stats.draws++;
    return this.stats.draws;
  }
}
