// canvas2d_blitter.ts — @weft/heddle-canvas Tier 3: direct pixel blitting.
//
// WHY EXISTS: the mandate's universal fallback — every device that can
// draw at all can draw into an ImageData. The naive 2D-canvas path (fillRect
// per data point, or worse: getImageData/putImageData per frame) allocates
// and copies its way to a GC pause at exactly the moment the display needs
// the frame. This blitter is the disciplined version: ONE ImageData carved
// at init, its Uint32Array alias written directly (zero intermediate
// arrays), and putImageData invoked ONCE per frame with the ACCUMULATED
// dirty rectangle (deliverable C: only mutated regions of the canvas
// re-rasterize — the 2D-context analogue of the GL scissor).
//
// Decimation happens CPU-side here (Tier 3 has no shaders) with the exact
// same min/max reduction the WGSL compute and GLSL TF passes perform —
// order-independent comparisons, so the three tiers agree BIT-EXACTLY on
// the same input (the cross-tier oracle the browser leg asserts).

import type { PlaneView, LaneView } from '../../plane/hot_plane.ts';
import type { RenderHAL } from '../hal.ts';
import { createHalStats, RENDER_TIERS, type EngineConfig, type HalStats } from '../tier.ts';
import { HeddleError, type TierFallbackEvent } from '../../errors.ts';
import { decimateWindowMinMax } from '../../renderers/cpu_oracle.ts';
import { HP_CANDLE_W_OHLC, HP_CANDLE_W_VOLUME, HP_LADDER_W_PRICE, HP_LADDER_W_SIDE, HP_LADDER_W_SIZE } from '../../plane/whp1.ts';

/** The minimal 2D-context surface the blitter touches (fake-able). */
export interface Canvas2DLike {
  putImageData(
    img: unknown, dx: number, dy: number,
    dirtyX?: number, dirtyY?: number, dirtyW?: number, dirtyH?: number,
  ): void;
}

const F32_MAX = 3.4028234663852886e38;

export class Canvas2DHAL implements RenderHAL {
  readonly tier = RENDER_TIERS.CANVAS2D;
  readonly directMapped = false;
  readonly name = 'canvas2d';
  readonly stats: HalStats = createHalStats();
  readonly sabViewAccepted = true; // reads the SAB directly, always

  // LAW1:INIT-BEGIN
  private ctx: Canvas2DLike | null = null;
  private image: ImageData | null = null;
  private pixels: Uint32Array | null = null; // alias over image.data
  private width = 0;
  private height = 0;
  /** Per-column cached [min,max] — incremental decimation state. */
  private minmax = new Float32Array(0);
  private columnCount = 0;
  private frameOpen = false;
  /** Accumulated dirty rect (pixel space), reset by endFrame. */
  private dirtyX0 = 0; private dirtyY0 = 0;
  private dirtyX1 = 0; private dirtyY1 = 0;
  private dirtyEmpty = true;
  private clearRGBA = 0xff000000 | (12 << 16) | (12 << 8) | 16; // ABGR LE
  // LAW1:INIT-END

  constructor(ctx: Canvas2DLike) {
    this.ctx = ctx;
  }

  initialize(plane: PlaneView, cfg: EngineConfig): void {
    if (typeof ImageData === 'undefined') {
      throw new HeddleError(
        'HC_E_BACKEND_REFUSED',
        'canvas2d hal: ImageData global absent (non-browser host)',
      );
    }
    this.width = cfg.canvasWidth;
    this.height = cfg.canvasHeight;
    this.columnCount = Math.min(cfg.columnCount, cfg.canvasWidth);
    // THE allocation: one ImageData, one alias, one decimation cache —
    // every frame after this reuses exactly these objects.
    this.image = new ImageData(this.width, this.height);
    this.pixels = new Uint32Array(
      this.image.data.buffer, 0, this.width * this.height,
    );
    this.pixels.fill(this.clearRGBA);
    this.minmax = new Float32Array(this.columnCount * 2);
    for (let c = 0; c < this.columnCount; c++) {
      this.minmax[c * 2] = F32_MAX;
      this.minmax[c * 2 + 1] = -F32_MAX;
    }
    void plane; // lane views arrive per-call; nothing lane-specific to carve
  }

  handleLoss(reason: string): TierFallbackEvent | null {
    // A 2D context cannot be "lost" the way GL contexts are; the only
    // honest death is the canvas element being torn down, which the host
    // reports through dispose. Surface it as a degradation event.
    return {
      from: this.name, to: 'null', code: 'HC_E_CONTEXT_LOST',
      detail: `canvas2d host context gone: ${reason}`,
    };
  }

  beginFrame(): void {
    this.frameOpen = true;
    this.dirtyEmpty = true;
    this.dirtyX0 = 0; this.dirtyY0 = 0; this.dirtyX1 = 0; this.dirtyY1 = 0;
  }

  uploadWaveform(lane: LaneView, elemStart: number, elemEndExcl: number): void {
    // Tier 3 has no GPU memory to fill — the "upload" is the accounting
    // (bytes the other tiers would have moved) plus the dirty-rect union.
    // The decimation itself runs at draw time over the LIVE lane view,
    // exactly like the WGSL/GLSL passes read their storage buffers.
    this.assertFrame();
    const bytes = (elemEndExcl - elemStart) * lane.strideBytes;
    this.stats.bindCount++;
    this.stats.uploadCalls++;
    this.stats.uploadedBytes += bytes;
    const cols = Math.min(this.columnCount, this.width);
    const c0 = Math.floor((elemStart / lane.capacity) * cols);
    const c1 = Math.min(cols, Math.ceil((elemEndExcl / lane.capacity) * cols));
    if (c1 > c0) this.expandDirty(c0, 0, c1, this.height);
  }

  uploadRows(lane: LaneView, elemStart: number, elemEndExcl: number): void {
    this.assertFrame();
    this.stats.bindCount++;
    this.stats.uploadCalls++;
    this.stats.uploadedBytes += (elemEndExcl - elemStart) * lane.strideBytes;
    // Rows live in the SAB; rasterization reads them directly in draw*.
    // Raster-space dirt: rows map to price→y bands; without a projection
    // cache the honest bound is the rows' y extents — expand per draw.
  }

  drawOscillo(lane: LaneView, elemCount: number, windowStart: number): void {
    this.assertFrame();
    const px = this.pixels;
    if (px === null) throw new HeddleError('HC_E_CONTEXT_LOST', 'canvas2d hal: not initialized');
    const w = this.width, h = this.height;
    const cols = Math.min(this.columnCount, w);
    // THE reference decimation (renderers/cpu_oracle.ts) — the same window
    // walk the WGSL compute and GLSL TF passes perform, so Tier 3's ribbon
    // is bit-exact with Tiers 1/2 by construction.
    decimateWindowMinMax(lane.f32, lane.capacity, elemCount, windowStart, cols, this.minmax);
    const color = 0xff2fbf71; // ABGR: opaque green
    const yScale = (h - 1) * 0.9;
    const yOff = (h - 1) * 0.05;
    for (let c = 0; c < cols; c++) {
      const lo = this.minmax[c * 2];
      const hi = this.minmax[c * 2 + 1];
      if (lo > hi) continue; // never-computed column (sentinel)
      let y0 = h - 1 - Math.round(lo * yScale + yOff);
      let y1 = h - 1 - Math.round(hi * yScale + yOff);
      if (y0 > y1) { const t = y0; y0 = y1; y1 = t; }
      if (y0 < 0) y0 = 0;
      if (y1 > h - 1) y1 = h - 1;
      const base = c;
      for (let y = y0; y <= y1; y++) px[y * w + base] = color;
    }
    this.stats.drawCalls++;
    this.expandDirty(0, 0, cols, h);
  }

  drawDepthLadder(lane: LaneView, rowCount: number): void {
    this.assertFrame();
    const px = this.pixels;
    if (px === null) throw new HeddleError('HC_E_CONTEXT_LOST', 'canvas2d hal: not initialized');
    const w = this.width, h = this.height;
    const u = lane.u32; // 64B rows, 16 u32 words
    const rows = Math.min(rowCount, lane.capacity);
    const bidColor = 0xff3d7bd6;  // blue asks (ABGR)
    const askColor = 0xffd6873d;  // orange bids
    const yScale = (h - 4) / Math.max(1, rows);
    for (let r = 0; r < rows; r++) {
      const row = (r * (lane.strideBytes >> 2)) | 0;
      const price = bitcastU32ToF32(u[row + HP_LADDER_W_PRICE]);
      const size = bitcastU32ToF32(u[row + HP_LADDER_W_SIZE]);
      const side = u[row + HP_LADDER_W_SIDE]; // 0 bid / 1 ask
      const y = h - 2 - Math.floor(price * yScale);
      if (y < 0 || y > h - 1) continue;
      const xEnd = Math.min(w, Math.max(2, Math.round(size * (w - 4))));
      const color = side === 0 ? bidColor : askColor;
      const line = y * w;
      for (let x = 0; x < xEnd; x++) px[line + x] = color;
    }
    this.stats.drawCalls++;
    this.expandDirty(0, 0, w, h);
  }

  drawPointcloud(lane: LaneView, pointCount: number): void {
    this.assertFrame();
    const px = this.pixels;
    if (px === null) throw new HeddleError('HC_E_CONTEXT_LOST', 'canvas2d hal: not initialized');
    const w = this.width, h = this.height;
    const u = lane.u32; // 128B rows, 32 u32 words
    const pts = Math.min(pointCount, lane.capacity);
    const color = 0xffe0d24a;
    for (let p = 0; p < pts; p++) {
      const row = (p * (lane.strideBytes >> 2)) | 0;
      const x = Math.round((bitcastU32ToF32(u[row]) * 0.5 + 0.5) * (w - 1));
      const y = Math.round((bitcastU32ToF32(u[row + 1]) * 0.5 + 0.5) * (h - 1));
      if (x < 1 || x > w - 2 || y < 1 || y > h - 2) continue;
      // 2x2 sprite (Tier-3 economy: no quaternion math on the CPU floor —
      // orientation is a Tier-1/2 visual; position is the honest core).
      px[y * w + x] = color;
      px[y * w + x + 1] = color;
      px[(y + 1) * w + x] = color;
      px[(y + 1) * w + x + 1] = color;
    }
    this.stats.drawCalls++;
    this.expandDirty(0, 0, w, h);
  }

  drawCandles(lane: LaneView, rowCount: number): void {
    this.assertFrame();
    const px = this.pixels;
    if (px === null) throw new HeddleError('HC_E_CONTEXT_LOST', 'canvas2d hal: not initialized');
    const w = this.width, h = this.height;
    const f = lane.f32;
    const words = lane.strideBytes >> 2;
    const rows = Math.min(rowCount, lane.capacity);
    const up = 0xff2fbf71;    // ABGR green (close >= open)
    const down = 0xffd65050;  // ABGR red
    const yScale = (h - 1) * 0.92;
    const yOff = (h - 1) * 0.04;
    const colW = Math.max(1, Math.floor(w / Math.max(1, rows)));
    for (let r = 0; r < rows; r++) {
      const base = r * words + HP_CANDLE_W_OHLC;
      const open = f[base], high = f[base + 1], low = f[base + 2], close = f[base + 3];
      void HP_CANDLE_W_VOLUME;
      const color = close >= open ? up : down;
      const x0 = r * colW;
      // wick (1-px column through high..low)
      const wy0 = Math.max(0, Math.min(h - 1, h - 1 - Math.round(high * yScale + yOff)));
      const wy1 = Math.max(0, Math.min(h - 1, h - 1 - Math.round(low * yScale + yOff)));
      const wickX = Math.min(w - 1, x0 + (colW >> 1));
      for (let y = wy0; y <= wy1; y++) px[y * w + wickX] = color;
      // body (column band open..close)
      let by0 = h - 1 - Math.round(open * yScale + yOff);
      let by1 = h - 1 - Math.round(close * yScale + yOff);
      if (by0 > by1) { const t = by0; by0 = by1; by1 = t; }
      by0 = Math.max(0, by0); by1 = Math.min(h - 1, by1);
      const bx0 = x0;
      const bx1 = Math.min(w - 1, x0 + colW - 1);
      for (let y = by0; y <= by1; y++) {
        const line = y * w;
        for (let x = bx0; x <= bx1; x++) px[line + x] = color;
      }
    }
    this.stats.drawCalls++;
    this.expandDirty(0, 0, w, h);
  }

  endFrame(): void {
    this.assertFrame();
    const ctx = this.ctx;
    const img = this.image;
    if (ctx === null || img === null) {
      throw new HeddleError('HC_E_CONTEXT_LOST', 'canvas2d hal: no context');
    }
    if (!this.dirtyEmpty) {
      const dw = this.dirtyX1 - this.dirtyX0;
      const dh = this.dirtyY1 - this.dirtyY0;
      ctx.putImageData(
        img, 0, 0,
        this.dirtyX0, this.dirtyY0, dw, dh,
      );
      this.stats.scissorClips++;
      this.stats.presentCount++;
    } else {
      // Nothing mutated: NO putImageData at all — the clean-frame skip.
      this.stats.skippedCleanLanes++;
    }
    this.frameOpen = false;
  }

  /** The ImageData carved at init (identity check for the reuse gate). */
  imageData(): ImageData {
    if (this.image === null) {
      throw new HeddleError('HC_E_BACKEND_REFUSED', 'canvas2d hal: not initialized');
    }
    return this.image;
  }

  // --- internal (frame-time, allocation-free) ----------------------------

  private expandDirty(x0: number, y0: number, x1: number, y1: number): void {
    if (this.dirtyEmpty) {
      this.dirtyX0 = x0; this.dirtyY0 = y0;
      this.dirtyX1 = x1; this.dirtyY1 = y1;
      this.dirtyEmpty = false;
    } else {
      if (x0 < this.dirtyX0) this.dirtyX0 = x0;
      if (y0 < this.dirtyY0) this.dirtyY0 = y0;
      if (x1 > this.dirtyX1) this.dirtyX1 = x1;
      if (y1 > this.dirtyY1) this.dirtyY1 = y1;
    }
  }

  private assertFrame(): void {
    if (!this.frameOpen) {
      throw new HeddleError(
        'HC_E_BACKEND_REFUSED',
        'canvas2d hal: operation outside beginFrame/endFrame',
      );
    }
  }
}

/** Bit-preserving u32->f32 lane read (structured rows carry f32 payloads). */
const _bc = new Float32Array(1);
const _bcv = new Uint32Array(_bc.buffer);
function bitcastU32ToF32(bits: number): number {
  _bcv[0] = bits >>> 0;
  return _bc[0];
}
