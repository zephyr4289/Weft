// core.js — framework-agnostic zero-re-render tensor canvas controller.
//
// The controller owns the WHOLE frame path: ring -> plane -> overlay -> HUD
// counters, all on preallocated objects (Law 1). React never sees state —
// the only thing that ever changes on screen is the canvas pixels.
// React binding lives in index.js; tests drive this class directly.

import {
  Canvas2DPlane, WebGL2Plane, OverlayScratch,
  RingListener, rafPump, drawBoxes2D,
} from '../../weft-tensor/src/index.js';

export class TensorCanvasController {
  /**
   * @param {object} spec
   *   ring             WeftTensorRing (required)
   *   canvas           HTMLCanvasElement | OffscreenCanvas | fake (required)
   *   backend          '2d' (default) | 'webgl'
   *   width/height     explicit geometry (default: ring shape inference)
   *   overlay          (frame|null, scratch, ctx) => void — mutate the
   *                    scratch (boxes/keypoints); NEVER allocate here.
   *   drawBoxes        auto drawBoxes2D after overlay (2d backend, default true)
   *   pump             injectable pump (tests/headless); default rAF
   */
  constructor(spec) {
    const {
      ring, canvas, backend = '2d', width, height,
      overlay = null, drawBoxes = true, pump = null,
    } = spec;
    if (!ring || !canvas) throw new Error('TensorCanvasController: ring and canvas are required');
    this.ring = ring;
    this.overlay = overlay;
    this.drawBoxes = drawBoxes;
    this.stats = { ticks: 0, frames: 0, stalls: 0, boxes: 0 };
    Object.seal(this.stats);

    const planeOpts = {};
    if (width) planeOpts.width = width;
    if (height) planeOpts.height = height;
    this.plane = backend === 'webgl'
      ? new WebGL2Plane(canvas, ring, planeOpts)
      : new Canvas2DPlane(canvas, ring, planeOpts);
    this.scratch = overlay !== null ? new OverlayScratch() : null;
    this._listener = new RingListener(ring, {
      pump: pump ?? rafPump(),
      onFrame: this._onFrame,
      onStall: this._onStall,
    });
  }

  // Bound ONCE (Law 1): per-tick work is method calls on preallocated state.
  _onFrame = (frame) => {
    this.plane.draw(frame);
    this.stats.frames++;
    if (this.scratch !== null && this.overlay !== null) {
      this.overlay(frame, this.scratch, this.plane);
      this.stats.boxes = this.scratch.count;
      if (this.drawBoxes && this._ctx2d()) drawBoxes2D(this._ctx2d(), this.scratch);
    }
  };

  _onStall = () => {
    this.stats.stalls++;
    // Underrun: keep the last frame visible; still repaint overlay boxes so
    // HUD/telemetry stays alive at display refresh.
    this.plane.draw(null);
    if (this.scratch !== null && this.overlay !== null) {
      this.overlay(null, this.scratch, this.plane);
      if (this.drawBoxes && this._ctx2d()) drawBoxes2D(this._ctx2d(), this.scratch);
    }
  };

  _ctx2d() {
    // Canvas2DPlane keeps the 2d context private; overlay drawing reuses it
    // through the plane's public accessor (added for exactly this purpose).
    return this.plane.ctx ?? null;
  }

  start() { this._listener.start(); return this; }
  stop() { this._listener.stop(); return this; }
  get running() { return this._listener.running; }
}

/**
 * React binding factory — the host framework provides the two hooks.
 * The hook NEVER triggers re-renders from frame data: statsRef.current is
 * the controller's OWN sealed stats object (same identity, live values);
 * read it from rAF/interval HUD code, never from render state.
 */
export function createTensorCanvasHooks({ useRef, useEffect }) {
  return function useWeftTensorCanvas(ring, canvasRef, opts = {}) {
    const statsRef = useRef({ ticks: 0, frames: 0, stalls: 0, boxes: 0 });
    const optsRef = useRef(opts);
    optsRef.current = opts;

    useEffect(() => {
      const canvas = canvasRef.current;
      if (!ring || !canvas) return undefined;
      const o = optsRef.current;
      const controller = new TensorCanvasController({
        ring, canvas,
        backend: o.backend,
        overlay: o.overlay,
        drawBoxes: o.drawBoxes,
      });
      statsRef.current = controller.stats; // SAME sealed object — live, zero-copy
      controller.start();
      return () => {
        controller.stop();
        statsRef.current = { ticks: 0, frames: 0, stalls: 0, boxes: 0 };
      };
    }, [ring, canvasRef]);

    return statsRef;
  };
}
