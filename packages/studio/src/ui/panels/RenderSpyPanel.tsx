'use client';

/**
 * Weft Studio — Render Spy tool window (hot): live invocation counters for
 * every instrumented component. A hot component showing >1 while streaming
 * is a Law-4 violation — the bar turns red. Labels via 2 Hz DOM mutation.
 */

import { getReact, SPY, spy } from '../../engine/react-adapter';
import { markRender, useHotCanvas, HotLabels, fmtInt } from '../hooks';
import { STUDIO_THEME as T } from '../theme';
import type { StudioEngine } from '../../engine/studio-engine';

interface Props { engine: StudioEngine | null }

const NAMES: Record<number, string> = {
  [SPY.STUDIO_ROOT]: 'WeftStudio (root)',
  [SPY.SCHEMA_DESIGNER]: 'SchemaDesignerPanel',
  [SPY.CACHE_MAPPER]: 'CacheMapperPanel',
  [SPY.RING_MONITOR]: 'RingMonitorPanel',
  [SPY.TIME_TRAVEL]: 'TimeTravelPanel',
  [SPY.TELEMETRY_HUD]: 'TelemetryPanel',
  [SPY.RENDER_SPY_HUD]: 'RenderSpyPanel',
  [SPY.STATUS_BAR]: 'StatusBar',
  [SPY.PROJECT_TREE]: 'ProjectTreePanel',
  [SPY.DELIVERABLES]: 'DeliverablesPanel',
};

const ROWS = Object.keys(NAMES).map(Number);

export function RenderSpyPanel({ engine }: Props): unknown {
  markRender(SPY.RENDER_SPY_HUD);
  const React = getReact() as unknown as { useRef: (v: never) => { current: never } };
  const labels = React.useRef(new HotLabels()) as unknown as { current: HotLabels };
  const eng = engine;

  const canvas = useHotCanvas(engine, 'renderSpy', (_ctx, _w, _h, frame) => {
    if (!eng) return;
    if ((frame & 0x7f) === 0) {
      for (let i = 0; i < ROWS.length; i++) {
        const id = ROWS[i];
        labels.current.setNum(i, spy.counts[id], fmtInt);
      }
      labels.current.setNum(ROWS.length, spy.mutations, fmtInt);
    }
  });

  return hS('div', { style: { height: '100%', display: 'flex', flexDirection: 'column', background: T.chromeAlt } },
    hS('div', { style: { position: 'absolute', width: 0, height: 0, overflow: 'hidden' } },
      hS('canvas', { ref: canvas.ref as never })),
    hS('div', { style: { padding: '10px 12px 6px', color: T.textBright, fontSize: 11, fontWeight: 600, letterSpacing: 0.4, fontFamily: MONOS } },
      'RENDER SPY — LAW 4 PROOF SURFACE'),
    hS('div', { style: { padding: '0 12px 8px', color: T.textDim, fontSize: 10, fontFamily: MONOS } },
      'hot components must stay at 1 (mount) while frames stream'),
    hS('div', { style: { flex: 1, overflow: 'auto', padding: '0 8px 8px' } },
      ROWS.map((id, i) => hS('div', { key: id, style: rowStyle },
        hS('span', { style: { color: T.text, fontSize: 10.5, fontFamily: MONOS, flex: 1 } }, NAMES[id]),
        hS('span', { ref: labels.current.bind(i) as never, style: { fontSize: 11, fontFamily: MONOS, color: T.greenBright, fontWeight: 700 } }, '1'),
      )),
      hS('div', { style: rowStyle },
        hS('span', { style: { color: T.text, fontSize: 10.5, fontFamily: MONOS, flex: 1 } }, 'setState mutations (streaming)'),
        hS('span', { ref: labels.current.bind(ROWS.length) as never, style: { fontSize: 11, fontFamily: MONOS, color: T.greenBright, fontWeight: 700 } }, '0'),
      ),
    ),
  );
}

const rowStyle: Record<string, string | number> = {
  display: 'flex', alignItems: 'center', gap: 8, padding: '3px 6px', borderRadius: 4,
};

// ---------------------------------------------------------------- helpers --

function hS(type: string, props: Record<string, unknown> | null, ...children: unknown[]): unknown {
  const React = getReact() as unknown as { createElement: (...a: unknown[]) => unknown };
  return React.createElement(type, props ?? null, ...children);
}

export const MONOS = 'ui-monospace, "JetBrains Mono", Menlo, Consolas, monospace';
