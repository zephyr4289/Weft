/**
 * Weft Studio — hot-plane React glue.
 *
 * useHotCanvas: registers a draw callback into the engine's frame governor and
 * binds it to a canvas ref. The component renders EXACTLY ONCE; every frame
 * mutates the canvas surface through the ref. No setState, no subscriptions,
 * no per-frame allocation. Canvas resize is handled by mutating the canvas
 * backing-store size directly (never via React state).
 *
 * HotLabels: throttled DOM text node mutators (4 Hz) with change-detection —
 * a string is only produced when the displayed value actually changes.
 */

import { getReact, spy, markRender } from '../engine/react-adapter';

export { markRender };
export { spy };
import type { StudioEngine } from '../engine/studio-engine';

type R = ReturnType<typeof getReact>;

/** Shared 2D-context options — module-level so the frame wrapper allocates nothing. */
const CTX_2D_OPTS: { alpha: boolean } = { alpha: true };

export function useHotCanvas(
  engine: StudioEngine | null,
  kind: 'ring' | 'telemetry' | 'cloud' | 'attitude' | 'renderSpy' | 'memoryGrid',
  draw: (ctx: CanvasRenderingContext2D, w: number, h: number, frame: number) => void,
): { ref: (el: HTMLCanvasElement | null) => void } {
  const React = getReact() as unknown as {
    useRef: R['useRef'];
    useEffect: R['useEffect'];
  };
  const canvasRef = React.useRef<HTMLCanvasElement | null>(null);
  const drawRef = React.useRef(draw);
  React.useEffect(() => {
    drawRef.current = draw;
  });

  React.useEffect(() => {
    if (!engine) return;
    const drawWrapper = (frame: number) => {
      const canvas = canvasRef.current;
      if (!canvas) return;
      const ctx = canvas.getContext('2d', CTX_2D_OPTS);
      if (!ctx) return;
      const dpr = Math.min((typeof window !== 'undefined' && window.devicePixelRatio) || 1, 2);
      const w = canvas.clientWidth | 0;
      const h = canvas.clientHeight | 0;
      if (w > 0 && h > 0 && (canvas.width !== ((w * dpr) | 0) || canvas.height !== ((h * dpr) | 0))) {
        canvas.width = (w * dpr) | 0;
        canvas.height = (h * dpr) | 0;
      }
      if (canvas.width === 0 || canvas.height === 0) return;
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      drawRef.current(ctx, w, h, frame);
    };
    const arr = engine.drawers[kind];
    arr.push(drawWrapper);
    return () => {
      const idx = arr.indexOf(drawWrapper);
      if (idx >= 0) arr.splice(idx, 1);
    };
  }, [engine, kind]);

  return {
    ref: (el: HTMLCanvasElement | null) => { canvasRef.current = el; },
  };
}

/** Throttled, change-detected DOM label writer (hot-plane safe). */
export class HotLabels {
  private els: (HTMLElement | null)[] = [];
  private last: (string | undefined)[] = [];
  private lastNum = new Float64Array(64);

  bind(i: number): (el: HTMLElement | null) => void {
    return (el) => { this.els[i] = el; };
  }

  /** Write only when the formatted value changed. Returns the string used. */
  set(i: number, value: string): void {
    if (this.last[i] === value) return;
    this.last[i] = value;
    const el = this.els[i];
    if (el) el.textContent = value;
  }

  /**
   * Numeric label: compares the RAW number first; the formatted string is
   * produced ONLY when the displayed value actually changes. fmt must be a
   * stable module-level function reference (never an inline closure).
   */
  setNum(i: number, v: number, fmt: (n: number) => string): void {
    if (this.lastNum[i] === v && this.last[i] !== undefined) return;
    this.lastNum[i] = v;
    this.set(i, fmt(v));
  }
}

/** Deterministic formatters (called only on displayed-value change). */
export function fmtRate(v: number): string {
  if (v >= 1e6) return (v / 1e6).toFixed(2) + 'M/s';
  if (v >= 1e3) return (v / 1e3).toFixed(1) + 'K/s';
  return v.toFixed(0) + '/s';
}

export function fmtNs(v: number): string {
  if (v >= 1e6) return (v / 1e6).toFixed(2) + 'ms';
  if (v >= 1e3) return (v / 1e3).toFixed(1) + 'µs';
  return v.toFixed(0) + 'ns';
}

export function fmtBytes(v: number): string {
  if (v >= 1048576) return (v / 1048576).toFixed(1) + ' MiB';
  if (v >= 1024) return (v / 1024).toFixed(1) + ' KiB';
  return v + ' B';
}

export function fmtInt(v: number): string {
  return Math.round(v).toString();
}

/** Panel scaffolding hook: mark + render-once assertion helper. */
export function usePanelMount(spyId: number): void {
  markRender(spyId);
}
