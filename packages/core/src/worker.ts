/**
 * @file worker.ts — Worker-safe frame loops + the Transferable fallback
 * transport for @weft/core.
 *
 * WHY EXISTS: Implements the worker-side half of the web port per
 * WHITEPAPER §8.2, 02-KERNEL §4.2, and docs/PORTS.md §3.
 *
 * PLATFORM TRUTH (the reason this file was rewritten):
 * `requestAnimationFrame` does not exist inside a DedicatedWorkerGlobalScope.
 * The previous implementation called it unconditionally, so the advertised
 * "OffscreenCanvas worker loop" could never tick in a worker. This module now:
 *   1. Uses rAF when present (main thread / browser window context).
 *   2. Falls back to a timer-paced loop in workers (setTimeout(0) chain with
 *      fpsCap gating) — the loop only needs a heartbeat; on OffscreenCanvas
 *      workers the browser composites on commit()/transferToImageBitmap.
 *   3. Honors `fpsCap` in both schedulers.
 *   4. Validates the `shared` option instead of ignoring it.
 *   5. Ships the previously-promised-but-missing Transferable fallback
 *      (WeftTransferChannel): the SAB-free "one copy per frame" default path
 *      documented in ARCHITECTURE.md §3 honesty notes — latest-wins mailbox,
 *      pre-allocated buffer pool, zero steady-state allocation.
 *
 * CROSS-ORIGIN ISOLATION (COOP/COEP) NOTICE:
 * True zero-copy state sharing across Web Workers requires SharedArrayBuffer
 * (the Weft kernel is SAB-backed by design — see core/ts/weft.ts). Browsers
 * require the following HTTP headers on the serving origin:
 *   Cross-Origin-Opener-Policy: same-origin
 *   Cross-Origin-Embedder-Policy: require-corp
 * If the origin cannot ship COOP/COEP, use WeftTransferChannel below.
 *
 * DRAW PHASE DISCIPLINE:
 * Read operations (claim + live-view read) occur strictly inside the frame
 * callback, decoupled from composition or framework render cycles.
 */

import { Weft } from './index';

export interface WeftWorkerLoopOptions {
  /**
   * Use SharedArrayBuffer mode when true, or Transferable ArrayBuffer when
   * false. The Weft kernel is SAB-backed; `shared: false` is rejected with a
   * pointer to WeftTransferChannel (the honest fallback), rather than being
   * silently ignored as before. Default: auto-detect crossOriginIsolated.
   */
  shared?: boolean;

  /**
   * Target frame rate cap. Default 60. Honored by both the rAF and timer
   * schedulers (ticks arriving early are skipped — latest-wins makes
   * skipping free).
   */
  fpsCap?: number;

  /**
   * Callback invoked once per second of frames with the claimed seq and
   * measured FPS.
   */
  onFrame?: (seq: number, fps: number) => void;
}

export interface WeftWorkerController {
  start: () => void;
  stop: () => void;
  isRunning: () => boolean;
}

/// Default frame pacing (fps) for worker loops.
const DEFAULT_FPS_CAP = 60;

/**
 * Worker-safe frame scheduler. Uses requestAnimationFrame when the host
 * provides it (main thread), otherwise a setTimeout heartbeat (workers).
 * `fpsCap` gates the minimum interval between ticks in both modes.
 * Exported for reuse by framework bindings and the demo; testable with an
 * injected scheduler.
 */
export function createFrameLoop(
  tick: (now: number) => void,
  options: { fpsCap?: number; scheduler?: (cb: (now: number) => void) => () => void; now?: () => number } = {}
): { start: () => void; stop: () => void; isRunning: () => boolean } {
  const fpsCap = options.fpsCap && options.fpsCap > 0 ? options.fpsCap : DEFAULT_FPS_CAP;
  const minInterval = 1000 / fpsCap;
  const nowFn = options.now ?? ((t: number) => t);

  let running = false;
  let lastTick = -Infinity;
  let stopFn: (() => void) | null = null;

  const run = (cb: (now: number) => void): (() => void) => {
    // Injected scheduler (tests / custom hosts).
    if (options.scheduler) return options.scheduler(cb);
    // Browser main-thread path.
    if (typeof requestAnimationFrame === 'function') {
      const id = requestAnimationFrame(cb as FrameRequestCallback);
      return () => {
        if (typeof cancelAnimationFrame === 'function') cancelAnimationFrame(id as number);
        else clearTimeout(id as unknown as ReturnType<typeof setTimeout>);
      };
    }
    // Worker / Node path: timer heartbeat; fpsCap gating below paces it.
    let handle: ReturnType<typeof setTimeout>;
    const step = () => {
      if (!running) return;
      cb(Date.now());
      handle = setTimeout(step, 0);
    };
    handle = setTimeout(step, 0);
    return () => clearTimeout(handle);
  };

  return {
    start: () => {
      if (running) return;
      running = true;
      const schedule = run((now: number) => {
        const t = nowFn(now);
        // fpsCap gating: skip early ticks (free under latest-wins semantics).
        if (t - lastTick >= minInterval) {
          lastTick = t;
          tick(t);
        }
      });
      stopFn = () => {
        running = false;
        schedule();
      };
    },
    stop: () => {
      stopFn?.();
      stopFn = null;
    },
    isRunning: () => running,
  };
}

/**
 * Creates a high-frequency, wait-free rendering loop bound to an
 * OffscreenCanvas. Works on the main thread (rAF) AND inside workers
 * (timer-paced) — see the platform-truth note in the file header.
 *
 * @param canvas The OffscreenCanvas target (transfer it to the worker yourself).
 * @param weft The Weft kernel instance (SAB must be shared into the worker).
 * @param draw Callback executed on each paced tick to draw the live payload.
 * @param options Configuration for pacing and reporting.
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

  // `shared` validation (was silently ignored): the kernel is SAB-backed.
  if (options.shared === false) {
    throw new Error(
      'createWeftWorkerLoop: shared:false is not supported — the Weft kernel requires a ' +
        'SharedArrayBuffer. For non-cross-origin-isolated origins use WeftTransferChannel ' +
        '(the documented one-copy-per-frame Transferable fallback).'
    );
  }
  if (typeof SharedArrayBuffer !== 'undefined' && !(weft.sab instanceof SharedArrayBuffer)) {
    throw new Error('createWeftWorkerLoop: weft.sab is not a SharedArrayBuffer — cannot share across workers.');
  }

  const frameCountRef = { n: 0 };
  let lastTime = 0;
  let currentFps = 0;

  const loop = createFrameLoop(
    (now) => {
      // VSYNC-aligned (or paced): claim freshest published buffer.
      weft.claim();

      // Zero-allocation live read (cached payload view — Law 2).
      const payload = weft.rLive();

      // Draw callback receives the live held buffer in draw phase only.
      draw(ctx, payload);

      frameCountRef.n++;
      if (now - lastTime >= 1000) {
        currentFps = (frameCountRef.n * 1000) / (now - lastTime);
        frameCountRef.n = 0;
        lastTime = now;
        options.onFrame?.(weft.rSeq(), currentFps);
      }
    },
    { fpsCap: options.fpsCap }
  );

  return {
    start: () => {
      lastTime = Date.now();
      frameCountRef.n = 0;
      loop.start();
    },
    stop: () => loop.stop(),
    isRunning: () => loop.isRunning(),
  };
}

/**
 * Verifies if the current execution environment supports SharedArrayBuffer
 * with cross-origin isolation.
 */
export function isSharedMemorySupported(): boolean {
  return (
    typeof crossOriginIsolated !== 'undefined' &&
    crossOriginIsolated &&
    typeof SharedArrayBuffer !== 'undefined'
  );
}

// ---------------------------------------------------------------------------
// WeftTransferChannel — the documented Transferable fallback (ARCHITECTURE.md
// §3: "The web default is one copy per frame. Transferable ArrayBuffer works
// everywhere."). Semantically the Triad Protocol's latest-wins, implemented
// over a MessagePort with a pre-allocated buffer pool:
//
//   writer (worker)                          reader (main thread)
//   ─────────────────────                    ─────────────────────
//   publish(fill): take pooled buffer,       onmessage: newest frame wins;
//     fill(fill), post {seq, buf}              older frames are DROPPED
//     [transfer buf]                           (dropped frames are the
//                                              semantics of display)
//   recycle: reader returns consumed
//     buffers over the return port —
//     steady-state allocation = 0 (Law 2)
// ---------------------------------------------------------------------------

/// A frame delivered over the wire: seq + the transferable payload buffer.
export interface TransferFrame {
  seq: number;
  payload: ArrayBuffer;
}

/// Minimal port surface (MessagePort in browsers, node:worker_threads
/// MessagePort in Node) so the channel is testable without a browser.
export interface WeftPort {
  postMessage(message: unknown, transfer?: Transferable[]): void;
  onmessage: ((ev: { data: TransferFrame }) => void) | null;
}

export class WeftTransferWriter {
  private pool: ArrayBuffer[] = [];
  private seq = 0;
  /** Frames the writer has produced (telemetry, advisory). */
  publishes = 0;

  constructor(
    private port: WeftPort,
    payloadMax: number,
    poolSize = 3
  ) {
    for (let i = 0; i < poolSize; i++) this.pool.push(new ArrayBuffer(payloadMax));
  }

  /// Fill the next pooled buffer and hand it to the reader (transferred —
  /// zero copy on the wire, one copy per frame by design). If the pool is
  /// momentarily empty (reader still holds all buffers), the frame is
  /// DROPPED — latest-wins, never back-pressured (RFC-0001 I5).
  publish(fill: (payload: Uint8Array, seq: number) => void): 'sent' | 'dropped' {
    const buf = this.pool.pop();
    if (!buf) return 'dropped';
    const s = ++this.seq;
    fill(new Uint8Array(buf), s);
    this.publishes++;
    this.port.postMessage({ seq: s, payload: buf } satisfies TransferFrame, [buf]);
    return 'sent';
  }

  /// Reader returned a consumed buffer (recycle → steady-state alloc = 0).
  recycle(buf: ArrayBuffer): void {
    if (this.pool.length < 8) this.pool.push(buf);
  }
}

export class WeftTransferReader {
  private latest: TransferFrame | null = null;
  /** Frames dropped because a newer one arrived first (advisory). */
  dropped = 0;

  constructor(
    private port: WeftPort,
    private writer: WeftTransferWriter
  ) {
    port.onmessage = (ev: { data: TransferFrame }) => {
      if (this.latest) {
        this.dropped++;
        this.writer.recycle(this.latest.payload);
      }
      this.latest = ev.data;
    };
  }

  /// Draw-phase read: hand the freshest frame's bytes to `draw`, then return
  /// the buffer to the writer pool. Zero allocation at steady state.
  claimAndDraw(draw: (payload: Uint8Array, seq: number) => void): boolean {
    const frame = this.latest;
    if (!frame) return false;
    this.latest = null; // consumed; recycled below
    draw(new Uint8Array(frame.payload), frame.seq);
    this.writer.recycle(frame.payload);
    return true;
  }

  /// Peak into the freshest seq without consuming (advisory telemetry).
  peekSeq(): number {
    return this.latest?.seq ?? 0;
  }
}

/// Create a matched (writer, reader) transfer channel over an explicit port
/// pair. In the browser: `new MessageChannel()` — pass `ch.port1` to the
/// reader on the main thread, `ch.port2` to the writer in the worker.
export function createWeftTransferChannel(
  readerPort: WeftPort,
  writerPort: WeftPort,
  payloadMax: number,
  poolSize = 3
): { writer: WeftTransferWriter; reader: WeftTransferReader } {
  const writer = new WeftTransferWriter(writerPort, payloadMax, poolSize);
  const reader = new WeftTransferReader(readerPort, writer);
  return { writer, reader };
}
