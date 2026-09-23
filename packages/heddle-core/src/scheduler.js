// src/scheduler.js — FrameScheduler: fixed-cadence, drop-not-queue frame pump.
//
// The render cadence for heddle-2.0 (60/120/240 Hz). Contract (Law 1):
//   * ONE preallocated frame context object, mutated in place — the scheduler
//     allocates NOTHING per frame.
//   * Drop-not-queue: if the pump arrives late, whole intervals are SKIPPED
//     (counted), never queued. A 10 s stall is followed by exactly one frame,
//     not 2400 catch-up frames.
//   * Injectable clock + pump (raf / setTimeout / manual) — deterministic in
//     node --test with a virtual clock.

export class FrameScheduler {
  constructor({ hz = 240, raf = null, now = null } = {}) {
    if (!(hz > 0)) throw new RangeError('hz must be > 0');
    this.intervalNs = 1e9 / hz;
    this.hz = hz;
    this._raf = raf || (typeof requestAnimationFrame !== 'undefined'
      ? (cb) => requestAnimationFrame(cb)
      : null);
    this._now = now || (typeof performance !== 'undefined'
      ? () => performance.now() * 1e6
      : () => Date.now() * 1e6);
    this.frameCtx = { frame: 0, nowNs: 0, skipped: 0, lateNs: 0 };
    this.lastFrameNs = -1;
    this.running = false;
    this.paused = false;
    this.rendered = 0;
    this.skipped = 0;
    this.pauseCount = 0;
    this.resumeCount = 0;
    this._pending = null;
    this._loop = null;
    this._cb = null;
  }

  // Arm the scheduler WITHOUT launching a loop — for manual/virtual-clock
  // pumping (deterministic tests, headless benches). pump() drives frames.
  arm(callback) {
    this._cb = callback;
    this.running = true;
    this.lastFrameNs = -1; // first pump anchors the timeline
    return this;
  }

  start(callback) {
    if (this.running) return this;
    this.arm(callback);
    const self = this;
    this._loop = function loop() {
      if (!self.running) return;
      self.pump(self._now());
      if (self._raf) self._pending = self._raf(loop);
      else if (self._timerMs !== undefined && self._timerMs >= 0) {
        self._pending = setTimeout(loop, self._timerMs);
      }
    };
    this._loop();
    return this;
  }

  // Timer-driven variant for environments without rAF (terminal demos, tests).
  startWithTimer(callback, ms) {
    this._timerMs = ms;
    this._raf = null;
    return this.start(callback);
  }

  stop() {
    this.running = false;
    if (this._pending !== null && this._pending !== undefined && typeof clearTimeout === 'function') {
      clearTimeout(this._pending); // numeric id (browser) or Timeout object (node)
    }
    this._pending = null;
    this._cb = null;
    return this;
  }

  // One pump of the render loop. `nowNs` is injectable for tests.
  // Renders at most ONE frame per pump (drop-not-queue).
  // TOLERANCE_NS: 1 ns guard against floating-point clock drift (a frame is
  // never dropped for arriving < 1 ns "early"; physically irrelevant at any
  // human refresh rate, decisive for deterministic virtual-clock tests).
  static TOLERANCE_NS = 1;
  pump(nowNs) {
    if (!this.running || this.paused || this._cb === null) return false;
    if (this.lastFrameNs < 0) this.lastFrameNs = nowNs - this.intervalNs;
    const elapsed = nowNs - this.lastFrameNs;
    if (elapsed < this.intervalNs - FrameScheduler.TOLERANCE_NS) return false; // not due yet
    const lateNs = elapsed > this.intervalNs ? elapsed - this.intervalNs : 0;
    const skipped = Math.floor(lateNs / this.intervalNs);
    // advance by whole intervals: skipped whole frames are DROPPED, not queued
    this.lastFrameNs += (skipped + 1) * this.intervalNs;
    const ctx = this.frameCtx;
    ctx.frame += 1;
    ctx.nowNs = nowNs;
    ctx.skipped = skipped;
    ctx.lateNs = lateNs;
    this.rendered += 1;
    this.skipped += skipped;
    this._cb(ctx); // user render — reuses the same ctx object every frame
    return true;
  }

  // Page Visibility / Law 4: pausing is EXPLICIT; resume does not catch up.
  pause() {
    if (!this.paused) { this.paused = true; this.pauseCount += 1; }
    return this;
  }

  resume(nowNs) {
    if (this.paused) {
      this.paused = false;
      this.resumeCount += 1;
      if (typeof nowNs === 'number') this.lastFrameNs = nowNs - this.intervalNs;
    }
    return this;
  }
}
