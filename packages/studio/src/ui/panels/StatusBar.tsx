'use client';

/**
 * Weft Studio — Status bar (hot): /dev/shm buffer occupancy bar, active
 * writer/reader counts, locked FPS, zero-copy memory efficiency index, torn
 * reads, incidents, schema hash. DOM-only updates at 4 Hz; numeric labels are
 * value-change-gated (strings produced only when the displayed value
 * changes). No React re-renders while streaming.
 */

import { getReact, SPY } from '../../engine/react-adapter';
import { markRender, useHotCanvas, HotLabels } from '../hooks';
import { STUDIO_THEME as T } from '../theme';
import type { StudioEngine } from '../../engine/studio-engine';

interface Props { engine: StudioEngine | null; schemaHash: string }

export function StatusBar({ engine, schemaHash }: Props): unknown {
  markRender(SPY.STATUS_BAR);
  const React = getReact() as unknown as { useRef: (v: never) => { current: never }; useEffect: (fn: () => void | (() => void), deps?: unknown[]) => void };
  const labels = React.useRef(new HotLabels()) as unknown as { current: HotLabels };
  const statics = React.useRef({ skipped: -1 }) as unknown as { current: { skipped: number } };
  const occRef = React.useRef(null) as unknown as { current: HTMLDivElement | null };
  const eng = engine;

  const statusCanvas = useHotCanvas(engine, 'renderSpy', (_ctx, _w, _h, frame) => {
    if (!eng) return;
    if ((frame & 0x3f) !== 0) return;
    const L = labels.current;
    L.setNum(0, eng.live.occupancy, fmtIntS);
    // per-window skip rate (skipped is cumulative; rate = delta × 4 per second)
    const skipped = eng.scheduler.skipped;
    if (skipped !== statics.current.skipped) {
      const delta = skipped - statics.current.skipped;
      statics.current.skipped = skipped;
      const perSec = delta * 4;
      L.set(2, perSec === 0 ? '240 fps' : `240 fps · gov-skip ${perSec}/s`);
    }
    L.setNum(3, eng.live.zeroCopyIndex, zcS);
    L.setNum(4, eng.ring.counters.tornRetries, tornFmt);
    L.setNum(5, eng.ring.counters.dropped, dropFmt);
    L.setNum(6, eng.live.frame, frameFmt);
    const occ = occRef.current;
    if (occ) {
      const pct = (eng.live.occupancy / eng.ring.capacity) * 100;
      if (occ.style.width !== pctStr(pct)) occ.style.width = pctStr(pct);
    }
  });

  // one-time static labels (mount lifecycle — cold plane)
  React.useEffect(() => {
    if (!eng) return;
    const L = labels.current;
    L.set(1, `writers ${eng.live.writers} · readers ${eng.live.readers}`);
    L.set(7, `schema ${eng.schemaHashShort()}`);
    L.set(8, 'seam: managed-mirror');
  }, [eng]);

  return hB('div', { style: bar },
    // hidden hot-plane canvas: drives the 4 Hz status mutation loop (Law 4)
    hB('div', { style: { position: 'absolute', width: 0, height: 0, overflow: 'hidden' } },
      hB('canvas', { ref: statusCanvas.ref as never })),
    // /dev/shm occupancy
    hB('div', { style: cell },
      hB('span', { style: dim }, '/dev/shm'),
      hB('div', { style: { width: 110, height: 8, borderRadius: 4, background: '#26282B', overflow: 'hidden' } },
        hB('div', { ref: occRef as never, style: { height: '100%', width: '0%', background: `linear-gradient(90deg, ${T.accent}, ${T.green})` } })),
      hB('span', { ref: labels.current.bind(0) as never, style: val }, '—'),
    ),
    hB('span', { ref: labels.current.bind(1) as never, style: val }, 'writers 2 · readers 1'),
    hB('span', { ref: labels.current.bind(2) as never, style: { ...val, color: T.greenBright } }, '240 fps'),
    hB('span', { ref: labels.current.bind(3) as never, style: val }, 'zc-index —'),
    hB('span', { ref: labels.current.bind(4) as never, style: val }, 'torn —'),
    hB('span', { ref: labels.current.bind(5) as never, style: val }, 'dropped —'),
    hB('span', { flex: 1 }),
    hB('span', { ref: labels.current.bind(6) as never, style: { ...val, color: T.textDim } }, 'frame —'),
    hB('span', { ref: labels.current.bind(7) as never, style: { ...val, color: T.textDim } }, 'schema —'),
    hB('span', { ref: labels.current.bind(8) as never, style: { ...val, color: T.textDim } }, 'seam: managed-mirror'),
  );
}

function pctStr(p: number): string { return (p < 0.01 ? 0 : p).toFixed(2) + '%'; }
function fmtIntS(v: number): string { return Math.round(v).toString(); }
function zcS(v: number): string { return 'zc-index ' + v.toFixed(4); }
function tornFmt(v: number): string { return 'torn ' + Math.round(v); }
function dropFmt(v: number): string { return 'dropped ' + Math.round(v); }
function frameFmt(v: number): string { return 'frame ' + Math.round(v); }

// ---------------------------------------------------------------- helpers --

function hB(type: string, props: Record<string, unknown> | null, ...children: unknown[]): unknown {
  const React = getReact() as unknown as { createElement: (...a: unknown[]) => unknown };
  return React.createElement(type, props ?? null, ...children);
}

const bar: Record<string, string | number> = {
  height: 30, flexShrink: 0, display: 'flex', alignItems: 'center', gap: 16,
  padding: '0 12px', background: T.chrome, borderTop: `1px solid ${T.border}`,
  fontFamily: 'ui-monospace, monospace', position: 'relative' as never,
};

const cell: Record<string, string | number> = { display: 'flex', alignItems: 'center', gap: 8 };

const dim: Record<string, string | number> = { color: T.textDim, fontSize: 10 };

const val: Record<string, string | number> = { color: T.text, fontSize: 10.5 };
