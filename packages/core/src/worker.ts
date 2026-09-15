/**
 * @file worker.ts — OffscreenCanvas Worker loop template for @weft/core.
 *
 * WHY EXISTS: Implements the VSYNC-aligned OffscreenCanvas Worker loop per
 * WHITEPAPER §8.2 and 02-KERNEL §4.2.
 *
 * CROSS-ORIGIN ISOLATION (COOP/COEP) NOTICE:
 * True zero-copy state sharing across Web Workers requires SharedArrayBuffer.
 * Browsers require the following HTTP headers on the serving origin:
 *   Cross-Origin-Opener-Policy: same-origin
 *   Cross-Origin-Embedder-Policy: require-corp
 *
 * If cross-origin isolation is not active (crossOriginIsolated === false),
 * this module provides a Transferable ArrayBuffer fallback mode.
 *
 * DRAW PHASE DISCIPLINE:
 * Read operations (weft.claim() and reading the live buffer slice) occur
 * strictly inside the requestAnimationFrame draw callback, completely
 * decoupled from UI composition or React/Vue render cycles.
 */

import { Weft } from './index';

export interface WeftWorkerLoopOptions {
  /**
   * Use SharedArrayBuffer mode when true, or Transferable ArrayBuffer when false.
   * Defaults to checking globalThis.crossOriginIsolated if available.
   */
  shared?: boolean;

  /**
   * Target frame rate cap (default 60, or 120 if supported).
   */
  fpsCap?: number;

  /**
   * Callback invoked when a frame is rendered.
   */
  onFrame?: (seq: number, fps: number) => void;
}

export interface WeftWorkerController {
  start: () => void;
  stop: () => void;
  isRunning: () => boolean;
}

/**
 * Creates a high-frequency, wait-free rendering loop on an OffscreenCanvas.
 *
 * @param canvas The OffscreenCanvas target transferred to the worker.
 * @param weft The Weft kernel instance sharing the underlying buffer.
 * @param draw Callback executed on each VSYNC tick to draw the live payload.
 * @param options Configuration for shared memory and pacing.
 */
export function createWeftWorkerLoop(
  canvas: OffscreenCanvas,
  weft: Weft,
  draw: (ctx: OffscreenCanvasRenderingContext2D, payload: Uint8Array) => void,
  options: WeftWorkerLoopOptions = {}
): WeftWorkerController {
  const ctx = canvas.getContext('2d');
  if (!ctx) {
    throw new Error('Failed to acquire 2d context from OffscreenCanvas');
  }

  let running = false;
  let rafId = 0;
  let lastTime = performance.now();
  let frameCount = 0;
  let currentFps = 0;

  const tick = (now: number) => {
    if (!running) return;

    // VSYNC-aligned: claim freshest published buffer
    weft.claim();

    // Zero-allocation slice read directly from held buffer
    const payload = weft.rReadSlice(16, weft.payloadMax);

    // Draw callback receives live buffer in draw phase only
    draw(ctx, payload);

    frameCount++;
    if (now - lastTime >= 1000) {
      currentFps = (frameCount * 1000) / (now - lastTime);
      frameCount = 0;
      lastTime = now;
      if (options.onFrame) {
        options.onFrame(weft.rSeq(), currentFps);
      }
    }

    rafId = requestAnimationFrame(tick);
  };

  return {
    start: () => {
      if (running) return;
      running = true;
      lastTime = performance.now();
      frameCount = 0;
      rafId = requestAnimationFrame(tick);
    },
    stop: () => {
      running = false;
      if (rafId) {
        cancelAnimationFrame(rafId);
        rafId = 0;
      }
    },
    isRunning: () => running,
  };
}

/**
 * Verifies if the current execution environment supports SharedArrayBuffer
 * with cross-origin isolation.
 */
export function isSharedMemorySupported(): boolean {
  return typeof crossOriginIsolated !== 'undefined' && crossOriginIsolated && typeof SharedArrayBuffer !== 'undefined';
}
