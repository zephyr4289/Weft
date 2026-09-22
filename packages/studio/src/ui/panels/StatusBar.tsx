'use client';

/**
 * Weft Studio — Status bar (hot): /dev/shm buffer occupancy bar, active
 * writer/reader counts, locked FPS, zero-copy memory efficiency index, torn
 * reads, incidents, schema hash. DOM-only updates at 4 Hz via direct text/
 * style mutation. No React re-renders while streaming.
 */

import { getReact, SPY } from '../../engine/react-adapter';
import { markRender, useHotCanvas, HotLabels } from '../hooks';
import { STUDIO_THEME as T } from '../theme';
import type { StudioEngine } from '../../engine/studio-engine';

interface Props { engine: StudioEngine | null; schemaHash: string }

export function StatusBar({ engine, schemaHash }: Props): unknown {
  markRender(SPY.STATUS_BAR);
  const React = getReact() as unknown as { useRef: (v: never) => { current: never } };
  const labels = React.useRef(new HotLabels()) as unknown as { current: HotLabels };
  const occRef = React.useRef(null) as unknown as { current: HTMLDivElement | null };
  const eng = engine;

  useHotCanvas(engine, 'renderSpy', (_ctx, _w, _h, frame) => {
    if (!eng) return;
    if ((frame & 0x3f) !== 0) return;
    const el = (i: number) => labels.set(i, value(i, eng));
    for (let i = 0; i < 8; i++) el(i);
    const occ = occRef.current;
    if (occ) occ.style.width = `${Math.min(100, (eng.live.occupancy / eng.ring.capacity) * 100).toFixed(2)}%`;
  });

  return hB('div', { style: bar },
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
    hB('span', { ref: labels.current.bind(6) as never, style: { ...val, color: T.textDim } }, `schema ${schemaHash.slice(0, 12)}`),
    hB('span', { ref: labels.current.bind(7) as never, style: { ...val, color: T.textDim } }, 'seam: managed-mirror'),
  );
}

function value(i: number, eng: StudioEngine): string {
  switch (i) {
    case 0: return `${eng.live.occupancy}`;
    case 1: return `writers ${eng.live.writers} · readers ${eng.live.readers}`;
    case 2: return `${eng.scheduler.skipped ? `${240 - eng.scheduler.skipped}/240` : '240'} fps`;
    case 3: return `zc-index ${eng.live.zeroCopyIndex.toFixed(4)}`;
    case 4: return `torn ${eng.ring.counters.tornRetries}`;
    case 5: return `dropped ${eng.ring.counters.dropped}`;
    case 6: return `frame ${eng.live.frame}`;
    default: return `heap-hint ${(eng.live.zeroCopyIndex * 0).toFixed(0)} KiB`;
  }
}

// ---------------------------------------------------------------- helpers --

function hB(type: string, props: Record<string, unknown> | null, ...children: unknown[]): unknown {
  const React = getReact() as unknown as { createElement: (...a: unknown[]) => unknown };
  return React.createElement(type, props ?? null, ...children);
}

const bar: Record<string, string | number> = {
  height: 30, flexShrink: 0, display: 'flex', alignItems: 'center', gap: 16,
  padding: '0 12px', background: T.chrome, borderTop: `1px solid ${T.border}`,
  fontFamily: 'ui-monospace, monospace',
};

const cell: Record<string, string | number> = { display: 'flex', alignItems: 'center', gap: 8 };

const dim: Record<string, string | number> = { color: T.textDim, fontSize: 10 };

const val: Record<string, string | number> = { color: T.text, fontSize: 10.5 };
