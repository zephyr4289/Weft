// WeftCanvas.tsx — React Heddle binding (TypeScript)
//
// WHY EXISTS: Wraps the existing TS kernel (Phase 0e) as a React component.
// Per WHITEPAPER §8.2: SAB requires COOP/COEP; default web path is one-copy
// Transferable. Per 02-KERNEL §4.2: reads a Weft during the Draw phase only.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.
//
// 2026-09 hardening: the draw closure is read through a ref (latest-ref
// pattern) and is deliberately NOT an effect dependency. A `draw` prop with
// unstable identity (an inline lambda — the common case) previously tore
// down and restarted the rAF loop on every parent render; the loop now keys
// only on the Weft instance. `weft` identity change is the only event that
// legitimately requires a new loop.

import React, { useEffect, useRef } from 'react';
import { Weft } from '@weft/core';

export interface WeftCanvasProps extends React.CanvasHTMLAttributes<HTMLCanvasElement> {
  weft: Weft;
  draw: (ctx: CanvasRenderingContext2D, buf: Uint8Array) => void;
}

export function WeftCanvas({ weft, draw, ...canvasProps }: WeftCanvasProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  // Latest-ref: the loop always calls the freshest draw closure without
  // re-subscribing. Updating a ref during render is the accepted escape
  // hatch for exactly this pattern (React docs: "the latest props" ref).
  const drawRef = useRef(draw);
  drawRef.current = draw;

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    let raf = 0;
    const tick = () => {
      // Draw phase discipline: claim + read live buffer inside the frame
      // callback only — never during React render/commit.
      weft.claim();
      const buf = weft.rReadSlice(16, weft.payloadMax);
      drawRef.current(ctx, buf);
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);

    return () => cancelAnimationFrame(raf);
  }, [weft]); // draw intentionally excluded — see the latest-ref note above.

  return React.createElement('canvas', { ref: canvasRef, ...canvasProps });
}
