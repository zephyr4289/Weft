// webgl2_context.ts — @weft/heddle-canvas WebGL2 acquisition + loss ladder.
//
// WHY EXISTS: Law 4 names WebGL context loss as a first-class failure mode
// ("explicit error handling for WebGL context loss"). The browser spec
// gives the protocol — 'webglcontextlost' MUST preventDefault() to opt
// into restoration, resources die with the context, 'webglcontextrestored'
// fires when the driver is back — but the spec does not stop the frame
// loop from feeding a dead context. This module owns that seam: it
// acquires the context, wires BOTH events, and exposes a single honest
// state (ALIVE / LOST_PENDING_RESTORE / DEAD) the HAL checks before every
// frame so a lost context degrades — never throws mid-loop.

import { HeddleError } from '../../errors.ts';

export const GL_CONTEXT_STATE = {
  ALIVE: 0,
  LOST_PENDING_RESTORE: 1,
  DEAD: 2,
} as const;
export type GLContextState = (typeof GL_CONTEXT_STATE)[keyof typeof GL_CONTEXT_STATE];

/** The context events we route (structural — fake-able in node). */
export interface GLContextEventTarget {
  addEventListener(type: string, listener: (ev: { preventDefault(): void }) => void): void;
}

export interface WebGL2AcquireResult {
  readonly gl: unknown; // WebGL2RenderingContext-ish (structural GL2 in binder)
  readonly context: GLContextStateMachine;
}

/**
 * The loss/restore state machine. ONE instance per acquired context; the
 * HAL holds it and the frame loop consults `canDraw()` — no event handler
 * allocations per frame, the listeners are wired exactly once (Law 1).
 */
export class GLContextStateMachine {
  // LAW1:INIT-BEGIN (listeners wired once at acquisition)
  private state: GLContextState = GL_CONTEXT_STATE.ALIVE;
  private readonly onLost: (ev: { preventDefault(): void }) => void;
  private readonly onRestored: () => void;
  private restoreHandler: (() => void) | null = null;
  // LAW1:INIT-END

  constructor(target: GLContextEventTarget) {
    // Listeners close over `this` only — carved once, never per frame.
    this.onLost = (ev) => {
      ev.preventDefault(); // REQUIRED to allow restoration
      this.state = GL_CONTEXT_STATE.LOST_PENDING_RESTORE;
    };
    this.onRestored = () => {
      this.state = GL_CONTEXT_STATE.ALIVE;
      if (this.restoreHandler !== null) this.restoreHandler();
    };
    target.addEventListener('webglcontextlost', this.onLost);
    target.addEventListener('webglcontextrestored', this.onRestored);
  }

  /** Frame-loop gate: true only when submissions are legal. */
  canDraw(): boolean {
    return this.state === GL_CONTEXT_STATE.ALIVE;
  }

  currentState(): GLContextState {
    return this.state;
  }

  /** Host registers the resource re-carve to run on restoration. */
  onRestore(handler: () => void): void {
    this.restoreHandler = handler;
  }

  /** Host gave up on restoration (e.g. timeout): mark DEAD, honestly. */
  markDead(): void {
    this.state = GL_CONTEXT_STATE.DEAD;
  }
}

/**
 * Acquire a WebGL2 context from a canvas-like host. Returns null when the
 * platform refuses (the ladder treats null as the named refusal
 * HC_E_BACKEND_REFUSED, never as a crash).
 *
 * EVENT TARGET DISCIPLINE (the spec's quiet corner): webglcontextlost /
 * webglcontextrestored fire at the CANVAS, not at the context object —
 * a WebGL2RenderingContext in Chromium has NO addEventListener. The state
 * machine therefore wires its listeners on the canvas; the context rides
 * along as the drawing surface.
 */
export function acquireWebGL2(
  canvas: {
    getContext(type: 'webgl2', attrs?: unknown): unknown;
    addEventListener(type: string, listener: (ev: { preventDefault(): void }) => void): void;
  },
): WebGL2AcquireResult | null {
  const gl = canvas.getContext('webgl2', { alpha: false, antialias: false });
  if (gl === null || gl === undefined) return null;
  if (typeof canvas.addEventListener !== 'function') {
    throw new HeddleError(
      'HC_E_BACKEND_REFUSED',
      'canvas host lacks event target surface (non-browser host?)',
    );
  }
  return { gl, context: new GLContextStateMachine(canvas) };
}
