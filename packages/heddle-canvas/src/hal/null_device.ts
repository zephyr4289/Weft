// null_device.ts — @weft/heddle-canvas NullHAL: the executable spec.
//
// WHY EXISTS: the CI sandbox (and any headless server node) has no browser,
// no GPU, no canvas. The honest answer to that is not "skip the graphics
// pillar" — it is a backend whose EVERY observable behavior matches the
// contract while the GPU submit itself is elided. NullHAL records the full
// call stream (views, sub-ranges, byte counts, draw geometry) into
// pre-allocated rings so the node battery can assert the OTHER backends'
// obligations — sub-range correctness, view identity (the zero-copy
// proof), draw-skip on clean lanes — without a GPU anywhere in the room.
// The 60,000-frame Law-1 heap gate and the frame-budget bench run on this
// backend in CI; hardware-tier numbers live in the browser leg and D-42.
//
// Everything frame-time here is number writes into pre-allocated arrays.

import type { PlaneView, LaneView } from '../plane/hot_plane.ts';
import type { RenderHAL } from './hal.ts';
import { createHalStats, RENDER_TIERS, type EngineConfig, type HalStats } from './tier.ts';
import type { TierFallbackEvent } from '../errors.ts';
import { HeddleError } from '../errors.ts';

/** One recorded upload. View identity is the zero-copy evidence. */
export interface UploadRecord {
  laneIndex: number;
  elemStart: number;
  elemEndExcl: number;
  bytes: number;
  /** The exact TypedArray handed to the (elided) GPU call. */
  view: Float32Array | Uint32Array | null;
}

/** One recorded draw. */
export interface DrawRecord {
  laneIndex: number;
  family: number; // 0 oscillo, 1 ladder, 2 pointcloud, 3 candles
  count: number;
  /** Oscillo records: writePos % capacity (ring window start). */
  windowStart: number;
}

const HISTORY = 4096;

export class NullHAL implements RenderHAL {
  readonly tier = RENDER_TIERS.NULL;
  readonly directMapped = false;
  readonly name = 'null';
  readonly stats: HalStats = createHalStats();
  readonly sabViewAccepted = true; // no GPU to refuse anything

  // LAW1:INIT-BEGIN — carved once in initialize(); nothing below allocates
  // after initialize() returns.
  private uploads: UploadRecord[] = [];
  private draws: DrawRecord[] = [];
  private uploadRing: Int32Array = new Int32Array(HISTORY * 4);
  private drawRing: Int32Array = new Int32Array(HISTORY * 3);
  private uploadHead = 0;
  private drawHead = 0;
  private frameOpen = false;
  private cfg: EngineConfig | null = null;
  private plane: PlaneView | null = null;
  // LAW1:INIT-END

  initialize(plane: PlaneView, cfg: EngineConfig): void {
    this.plane = plane;
    this.cfg = cfg;
    for (let i = 0; i < HISTORY; i++) {
      this.uploads.push({
        laneIndex: 0, elemStart: 0, elemEndExcl: 0, bytes: 0, view: null,
      });
      this.draws.push({ laneIndex: 0, family: 0, count: 0, windowStart: 0 });
    }
    this.uploadHead = 0;
    this.drawHead = 0;
    if (cfg.canvasWidth <= 0 || cfg.canvasHeight <= 0 || cfg.tickHz <= 0
      || cfg.columnCount <= 0) {
      throw new HeddleError(
        'HC_E_BACKEND_REFUSED',
        `null hal: invalid config w=${cfg.canvasWidth} h=${cfg.canvasHeight} tick=${cfg.tickHz} cols=${cfg.columnCount}`,
      );
    }
  }

  handleLoss(_reason: string): TierFallbackEvent | null {
    // The NullHAL cannot die — it is the floor. Reported, not thrown.
    return null;
  }

  beginFrame(): void {
    this.frameOpen = true;
  }

  uploadWaveform(lane: LaneView, elemStart: number, elemEndExcl: number): void {
    this.recordUpload(lane, elemStart, elemEndExcl, lane.f32);
  }

  uploadRows(lane: LaneView, elemStart: number, elemEndExcl: number): void {
    this.recordUpload(lane, elemStart, elemEndExcl, lane.u32);
  }

  drawOscillo(lane: LaneView, elemCount: number, windowStart: number): void {
    this.recordDraw(lane.laneIndex, 0, elemCount, windowStart);
  }

  drawDepthLadder(lane: LaneView, rowCount: number): void {
    this.recordDraw(lane.laneIndex, 1, rowCount, 0);
  }

  drawCandles(lane: LaneView, rowCount: number): void {
    this.recordDraw(lane.laneIndex, 3, rowCount, 0);
  }

  drawPointcloud(lane: LaneView, pointCount: number): void {
    this.recordDraw(lane.laneIndex, 2, pointCount, 0);
  }

  endFrame(): void {
    this.frameOpen = false;
    this.stats.presentCount++;
  }

  // --- introspection for the battery (init-time queries only) -----------

  /** Latest `n` upload records, written into caller-owned `out` array. */
  recentUploads(n: number, out: UploadRecord[]): number {
    const count = Math.min(n, HISTORY, this.uploadHead);
    for (let i = 0; i < count; i++) {
      const idx = (this.uploadHead - count + i) % HISTORY;
      const r = this.uploads[idx];
      const o = out[i];
      o.laneIndex = r.laneIndex; o.elemStart = r.elemStart;
      o.elemEndExcl = r.elemEndExcl; o.bytes = r.bytes; o.view = r.view;
    }
    return count;
  }

  /** Latest `n` draw records, written into caller-owned `out` array. */
  recentDraws(n: number, out: DrawRecord[]): number {
    const count = Math.min(n, HISTORY, this.drawHead);
    for (let i = 0; i < count; i++) {
      const idx = (this.drawHead - count + i) % HISTORY;
      const r = this.draws[idx];
      const o = out[i];
      o.laneIndex = r.laneIndex; o.family = r.family; o.count = r.count;
      o.windowStart = r.windowStart;
    }
    return count;
  }

  config(): EngineConfig {
    if (this.cfg === null) {
      throw new HeddleError('HC_E_BACKEND_REFUSED', 'null hal: not initialized');
    }
    return this.cfg;
  }

  planeView(): PlaneView {
    if (this.plane === null) {
      throw new HeddleError('HC_E_BACKEND_REFUSED', 'null hal: not initialized');
    }
    return this.plane;
  }

  // --- internal recorders (frame-time, allocation-free) ------------------

  private recordUpload(
    lane: LaneView,
    elemStart: number,
    elemEndExcl: number,
    view: Float32Array | Uint32Array,
  ): void {
    if (!this.frameOpen) {
      throw new HeddleError(
        'HC_E_BACKEND_REFUSED',
        'upload outside beginFrame/endFrame — contract violation',
      );
    }
    const bytes = (elemEndExcl - elemStart) * lane.strideBytes;
    const slot = this.uploadHead % HISTORY;
    const r = this.uploads[slot];
    r.laneIndex = lane.laneIndex;
    r.elemStart = elemStart;
    r.elemEndExcl = elemEndExcl;
    r.bytes = bytes;
    r.view = view;
    this.uploadHead++;
    const b = slot * 4;
    this.uploadRing[b] = lane.laneIndex;
    this.uploadRing[b + 1] = elemStart;
    this.uploadRing[b + 2] = elemEndExcl;
    this.uploadRing[b + 3] = bytes;
    this.stats.bindCount++;
    this.stats.uploadCalls++;
    this.stats.uploadedBytes += bytes;
  }

  private recordDraw(laneIndex: number, family: number, count: number, windowStart: number): void {
    if (!this.frameOpen) {
      throw new HeddleError(
        'HC_E_BACKEND_REFUSED',
        'draw outside beginFrame/endFrame — contract violation',
      );
    }
    const slot = this.drawHead % HISTORY;
    const r = this.draws[slot];
    r.laneIndex = laneIndex;
    r.family = family;
    r.count = count;
    r.windowStart = windowStart;
    this.drawHead++;
    const b = slot * 3;
    this.drawRing[b] = laneIndex;
    this.drawRing[b + 1] = family;
    this.drawRing[b + 2] = count;
    this.stats.drawCalls++;
  }
}
