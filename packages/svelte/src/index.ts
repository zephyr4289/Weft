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
import { Weft } from '@weft/core';

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
    const buf = current.weft.rLive();
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
