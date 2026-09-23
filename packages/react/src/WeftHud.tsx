// WeftHud.tsx — RFC 0016 §9: the real-time DevTools overlay (React binding).
//
// WHY EXISTS: lock-free concurrency is invisible. Ring depth, freshness,
// drop bursts, and governor actions live in atomic counters and trace
// shards — developer-invisible until something breaks. The HUD renders
// them as a live oscilloscope ON TOP of the app: one glance answers "is
// the pipeline healthy, and if not, since when".
//
// ZERO-GC RENDER (the Law 2 story, UI edition): the component owns NO React
// state for the waveform. Samples land in preallocated Float32Array rings
// (one per metric) written by a single requestAnimationFrame loop; the
// canvas is repainted imperatively. Nothing allocates per frame, nothing
// re-renders — React reconciliation is never in the hot path. Unmount is
// the only allocation event.
//
// SAMPLING IS THE HOST'S JOB (Law 3 — mechanism, not policy): the HUD
// takes a `sample` callback and asks it once per frame for a flat record
// ({depth, behind, drops, ...}); where the numbers come from (debug view,
// trace shards, trend estimator) is the caller's architecture. The HUD
// never touches a Weft instance itself.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION (the repo's
// honesty pattern for UI bindings).

import React, { useEffect, useRef } from 'react';

/// One sample of pipeline health — all fields optional, the HUD renders
/// what it gets and holds the rest at zero.
export interface WeftHudSample {
  depth?: number;       // ring depth (0..1 normalized or absolute + max)
  depthMax?: number;
  behind?: number;      // framesBehind (freshness)
  drops?: number;       // cumulative drops (rendered as burst deltas)
  throughput?: number;  // fps or offers/s
  trend?: number;       // weft_trend verdict 0..3 (0 stable .. 3 burst)
}

export interface WeftHudProps {
  sample: () => WeftHudSample;
  /** Width/height in CSS pixels. Default 260x88. */
  width?: number;
  height?: number;
  /** History length (frames of waveform). Default 240. */
  history?: number;
  /** OSCAmber | OSCTerminal | OSCRed — default OSCAmber. */
  theme?: 'amber' | 'terminal' | 'red';
  /** Smoothing for the sparkline (0..1); 0.35 matches the eye's lag. */
  smoothing?: number;
}

interface MetricRing {
  buf: Float32Array;
  head: number;   // next write index
  filled: number;
  value: number;  // last smoothed value
}

const THEME = {
  amber: { fg: '#ffb347', grid: 'rgba(255,179,71,0.18)', bg: 'rgba(12,10,4,0.72)', warn: '#ff6b3d' },
  terminal: { fg: '#5dff9f', grid: 'rgba(93,255,159,0.15)', bg: 'rgba(3,12,6,0.72)', warn: '#ffd23d' },
  red: { fg: '#ff5d5d', grid: 'rgba(255,93,93,0.16)', bg: 'rgba(12,4,4,0.72)', warn: '#ff2222' },
} as const;

function makeRing(n: number): MetricRing {
  return { buf: new Float32Array(n), head: 0, filled: 0, value: 0 };
}

function pushRing(r: MetricRing, v: number): void {
  r.buf[r.head] = v;
  r.head = (r.head + 1) % r.buf.length;
  if (r.filled < r.buf.length) r.filled++;
}

function drawRing(ctx: CanvasRenderingContext2D, r: MetricRing, y0: number, h: number,
                  max: number, color: string, w: number): void {
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.25;
  ctx.beginPath();
  const n = r.buf.length;
  for (let i = 0; i < r.filled; i++) {
    // oldest -> newest, left -> right
    const idx = (r.head - r.filled + i + n * 2) % n;
    const v = Math.max(0, Math.min(1, r.buf[idx] / (max || 1)));
    const x = (i / (n - 1)) * w;
    const y = y0 + h - v * h;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.stroke();
}

export function WeftHud({
  sample, width = 260, height = 88, history = 240,
  theme = 'amber', smoothing = 0.35,
}: WeftHudProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const sampleRef = useRef(sample);
  sampleRef.current = sample;  // latest-ref: the rAF loop never re-subscribes

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    // preallocated rings — the only allocations this component ever does
    const depth = makeRing(history);
    const behind = makeRing(history);
    const drops = makeRing(history);
    let lastDrops = -1;      // -1 = first sample primes the baseline
    let burst = 0;
    const maxes = { depth: 1, behind: 1, drops: 1 };
    const pal = THEME[theme];
    let raf = 0;
    let running = true;
    let samplerErrors = 0;  // advisory: a throwing sampler is a HOST bug;
                            // the overlay is fail-safe by contract — it
                            // counts, flags, and keeps the app alive

    const smooth = (r: MetricRing, v: number): number => {
      r.value = r.value + (v - r.value) * (1 - smoothing);
      return r.value;
    };

    const frame = () => {
      if (!running) return;
      let s: WeftHudSample;
      try {
        s = sampleRef.current() || {};
      } catch {
        samplerErrors++;
        s = {};
      }

      const dRaw = s.depthMax ? (s.depth ?? 0) / s.depthMax : (s.depth ?? 0);
      maxes.depth = Math.max(maxes.depth * 0.999, dRaw, 0.0001);
      pushRing(depth, smooth(depth, dRaw));

      const bRaw = s.behind ?? 0;
      maxes.behind = Math.max(maxes.behind * 0.999, bRaw, 1);
      pushRing(behind, smooth(behind, bRaw));

      // drops are cumulative; the HUD renders BURST DELTAS (visible loss
      // per frame — a steady count line is information-free)
      const dTotal = s.drops ?? 0;
      if (lastDrops >= 0 && dTotal >= lastDrops) burst = dTotal - lastDrops;
      lastDrops = dTotal;
      maxes.drops = Math.max(maxes.drops * 0.999, burst, 1);
      pushRing(drops, burst);

      // ---- paint (imperative, allocation-free) ----
      ctx.clearRect(0, 0, width, height);
      ctx.fillStyle = pal.bg;
      ctx.fillRect(0, 0, width, height);

      const rows = [
        { y0: 4, h: (height - 12) / 3, r: depth, c: pal.fg, label: 'DEPTH' },
        { y0: 4 + (height - 12) / 3 + 2, h: (height - 12) / 3, r: behind, c: pal.fg, label: 'BEHIND' },
        { y0: 4 + 2 * ((height - 12) / 3) + 4, h: (height - 12) / 3, r: drops, c: pal.warn, label: 'DROP/s' },
      ];
      for (const row of rows) {
        ctx.strokeStyle = pal.grid;
        ctx.lineWidth = 1;
        ctx.strokeRect(0.5, row.y0 + 0.5, width - 1, row.h);
        drawRing(ctx, row.r, row.y0, row.h, row.label === 'DEPTH' ? maxes.depth : row.label === 'BEHIND' ? maxes.behind : maxes.drops, row.c, width);
        ctx.fillStyle = pal.fg;
        ctx.font = '8px monospace';
        ctx.fillText(row.label, 3, row.y0 + 9);
      }
      // trend verdict badge (0 stable .. 3 burst); sampler errors flag
      const tv = s.trend ?? 0;
      ctx.fillStyle = tv >= 3 ? pal.warn : pal.fg;
      ctx.fillText(['STABLE', 'RISING', 'FALLING', 'BURST'][tv] ?? '?', width - 46, 10);
      if (samplerErrors > 0) {
        ctx.fillStyle = pal.warn;
        ctx.fillText(`SAMP!${samplerErrors}`, width - 46, height - 4);
      }

      raf = requestAnimationFrame(frame);
    };
    raf = requestAnimationFrame(frame);

    return () => {
      running = false;
      cancelAnimationFrame(raf);
    };
  }, [width, height, history, theme, smoothing]);

  return React.createElement('canvas', {
    ref: canvasRef,
    width,
    height,
    style: {
      position: 'absolute', top: 8, right: 8, zIndex: 9999,
      pointerEvents: 'none', borderRadius: 6,
    },
    'data-weft-hud': '',
  });
}
