// weft-action.ts — Svelte Heddle binding (TypeScript)
//
// WHY EXISTS: Wraps the existing TS kernel as a Svelte action.
// Per WHITEPAPER §8.2: SAB requires COOP/COEP.
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.
//
// 2026-09 hardening: added the `update` lifecycle handler. Svelte calls
// update() when the action's params change (e.g. hot-swapping the Weft or
// the draw closure); the previous version captured the initial params
// forever, silently drawing from a stale Weft after a param change.

import type { Action } from 'svelte/action';
import { Weft, WeftFanoutBroadcaster, type FanoutClaim } from '@weft/core';

export interface WeftActionParams {
  weft: Weft;
  draw: (ctx: CanvasRenderingContext2D, buf: Uint8Array) => void;
}

export const weftCanvas: Action<HTMLCanvasElement, WeftActionParams> = (canvas, params) => {
  const ctx = canvas.getContext('2d');
  if (!ctx) return;

  // Mutable latest params — updated via the `update` handler below.
  let current = params;
  let raf = 0;

  const tick = () => {
    // Draw phase discipline: claim + read the live buffer inside the frame
    // callback only — never during Svelte reactivity effects.
    current.weft.claim();
    const buf = current.weft.rReadSlice(16, current.weft.payloadMax);
    current.draw(ctx, buf);
    raf = requestAnimationFrame(tick);
  };
  raf = requestAnimationFrame(tick);

  return {
    update(newParams: WeftActionParams) {
      current = newParams;
    },
    destroy() {
      if (raf) {
        cancelAnimationFrame(raf);
        raf = 0;
      }
    },
  };
};

// ---------------------------------------------------------------------------
// Fan-out Heddle (RFC 0004) — one canvas = one consumer of a broadcaster.
//
// WHY EXISTS: RFC 0004 (accepted as a driver-layer pattern, round-6 §4)
// covers the 1-writer/N-reader case the 1:1 kernel Triad deliberately does
// not. The action owns one WeftFanoutReader and draws from its own
// pre-allocated buffer; attach N canvases to the same broadcaster.
//
// Inherits the 2026-09 hardening contract of weftCanvas above: params are
// mutable through the `update` handler, so a param change never leaves the
// loop drawing from stale state.
// ---------------------------------------------------------------------------

export interface WeftFanoutActionParams {
  broadcaster: WeftFanoutBroadcaster;
  draw: (ctx: CanvasRenderingContext2D, floats: Float32Array, claim: FanoutClaim) => void;
}

export const weftFanoutCanvas: Action<HTMLCanvasElement, WeftFanoutActionParams> = (canvas, params) => {
  const ctx = canvas.getContext('2d');
  if (!ctx) return;

  // One heddle = one consumer slot: this action owns its reader.
  const reader = params.broadcaster.createReader();

  // Mutable latest draw — hot-swapped via the `update` handler below. The
  // broadcaster is fixed for the action's lifetime (unmount + re-attach to
  // change it); the reader is bound to it.
  let currentDraw = params.draw;
  let raf = 0;

  const tick = () => {
    // Draw phase discipline: claim + read inside the frame callback only —
    // never during Svelte reactivity effects.
    const claim = reader.claim();
    currentDraw(ctx, reader.view(), claim);
    raf = requestAnimationFrame(tick);
  };
  raf = requestAnimationFrame(tick);

  return {
    update(newParams: WeftFanoutActionParams) {
      currentDraw = newParams.draw;
    },
    destroy() {
      if (raf) {
        cancelAnimationFrame(raf);
        raf = 0;
      }
    },
  };
};
