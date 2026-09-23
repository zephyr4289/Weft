// listener.js — RingListener: the frame pump that bridges rings to renderers.
//
// One persistent closure per listener; the tick function is created ONCE in
// the constructor (Law 1: no per-frame allocation). Pacing is injectable —
// rAF on browsers, timeout pumps in Node/Deno/tests.

import { runtimeMatrix } from './runtime.js';

export function rafPump() {
  const hasRaf = typeof requestAnimationFrame === 'function';
  return {
    request(cb) {
      if (hasRaf) return requestAnimationFrame(cb);
      return setTimeout(cb, 1); // headless fallback (tests / node)
    },
    cancel(id) {
      if (hasRaf) cancelAnimationFrame(id); else clearTimeout(id);
    },
  };
}

export function timeoutPump(intervalMs) {
  return {
    request(cb) { return setTimeout(cb, intervalMs); },
    cancel(id) { clearTimeout(id); },
  };
}

/**
 * Pump new frames from a ring into a callback at display pace.
 *
 *   const l = new RingListener(ring, { onFrame: (frame) => plane.draw(frame) });
 *   l.start(); ... l.stop();
 *
 * The callback receives the REUSED flyweight view — blit it and let go.
 */
export class RingListener {
  constructor(ring, opts = {}) {
    this._ring = ring;
    this._onFrame = opts.onFrame ?? null;
    this._onStall = opts.onStall ?? null;
    this._pump = opts.pump ?? rafPump();
    this._running = false;
    this._rafId = null;
    this._lastSeq = ring.producerSeq;
    this.stats = { ticks: 0, frames: 0, stalls: 0 };
    // ONE bound closure for the whole listener lifetime (zero-alloc ticks).
    this._tick = () => {
      if (!this._running) return;
      this.stats.ticks++;
      const frame = this._ring.acquireLatest(this._lastSeq);
      if (frame !== null) {
        this._lastSeq = frame.seq;
        this.stats.frames++;
        if (this._onFrame !== null) this._onFrame(frame);
      } else {
        this.stats.stalls++;
        if (this._onStall !== null) this._onStall(this.stats);
      }
      if (this._running) this._rafId = this._pump.request(this._tick);
    };
    Object.seal(this.stats);
  }

  get running() { return this._running; }
  get lastSeq() { return this._lastSeq; }

  start() {
    if (this._running) return this;
    this._running = true;
    this._rafId = this._pump.request(this._tick);
    return this;
  }

  stop() {
    this._running = false;
    if (this._rafId !== null) { this._pump.cancel(this._rafId); this._rafId = null; }
    return this;
  }

  setOnFrame(cb) { this._onFrame = cb; return this; }
}

/** Environment fingerprint line for CI evidence (Law 3). */
export function fabricFingerprint() {
  const r = runtimeMatrix();
  return `weft-tensor fabric: ${r.where} [${r.features.join(',')}]`;
}
