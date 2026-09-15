// weft-action.ts — Svelte Heddle binding (TypeScript)
//
// WHY EXISTS: Wraps the existing TS kernel as a Svelte action.
// Per WHITEPAPER §8.2: SAB requires COOP/COEP.
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

import type { Action } from 'svelte/action';
import { Weft } from '../../core/ts/weft';

export const weftCanvas: Action<HTMLCanvasElement, { weft: Weft; draw: (ctx: CanvasRenderingContext2D, buf: Uint8Array) => void }> = (canvas, params) => {
  const ctx = canvas.getContext('2d');
  if (!ctx) return;

  let raf = 0;
  const tick = () => {
    params.weft.claim();
    const buf = params.weft.rReadSlice(16, params.weft.payloadMax);
    params.draw(ctx, buf);
    raf = requestAnimationFrame(tick);
  };
  raf = requestAnimationFrame(tick);

  return { destroy: () => cancelAnimationFrame(raf) };
};
