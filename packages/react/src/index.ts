// WeftCanvas.tsx — React Heddle binding (TypeScript)
//
// WHY EXISTS: Wraps the existing TS kernel (Phase 0e) as a React component.
// Per WHITEPAPER §8.2: SAB requires COOP/COEP; default web path is one-copy
// Transferable. Per 02-KERNEL §4.2: reads a Weft during the Draw phase only.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

import React, { useEffect, useRef } from 'react';
import { Weft } from '@weft/core';

export interface WeftCanvasProps extends React.CanvasHTMLAttributes<HTMLCanvasElement> {
  weft: Weft;
  draw: (ctx: CanvasRenderingContext2D, buf: Uint8Array) => void;
}

export function WeftCanvas({ weft, draw, ...canvasProps }: WeftCanvasProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    let raf = 0;
    const tick = () => {
      weft.claim();
      const buf = weft.rReadSlice(16, weft.payloadMax);
      draw(ctx, buf);
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);

    return () => cancelAnimationFrame(raf);
  }, [weft, draw]);

  return React.createElement('canvas', { ref: canvasRef, ...canvasProps });
}
