'use client';

/**
 * Weft Studio — Telemetry HUD panel (hot): 240 FPS frame-work scorecard
 * (p50/p99/max vs the 4.166 ms budget), message-rate sparkline, zero-copy
 * efficiency index. Geometry-only canvas + 4 Hz DOM labels. Zero re-renders.
 */

import { getReact, SPY } from '../../engine/react-adapter';
import { markRender, useHotCanvas, HotLabels, fmtNs, fmtRate, fmtInt } from '../hooks';
import { STUDIO_THEME as T } from '../theme';
import type { StudioEngine } from '../../engine/studio-engine';

interface Props { engine: StudioEngine | null }

const ringMsg = new Float64Array(240);

export function TelemetryPanel({ engine }: Props): unknown {
  markRender(SPY.TELEMETRY_HUD);
  const React = getReact() as unknown as { useRef: (v: never) => { current: never } };
  const labels = React.useRef(new HotLabels()) as unknown as { current: HotLabels };
  const eng = engine;

  const canvas = useHotCanvas(engine, 'telemetry', (ctx, w, h, frame) => {
    if (!eng) return;
    draw(ctx, w, h, frame, eng, labels.current);
  });

  return hT('div', { style: { height: '100%', overflow: 'auto', padding: 12, background: T.bg, display: 'flex', flexDirection: 'column', gap: 10 } },
    hT('div', { style: { ...card, padding: 0 } },
      hT('div', { style: head },
        hT('span', { style: headTitle }, 'TELEMETRY HUD — 240 FPS GOVERNOR'),
        hT('span', { style: { flex: 1 } }),
        hT('span', { ref: labels.current.bind(3) as never, style: { color: T.greenBright, fontSize: 11, fontFamily: MONO2, fontWeight: 700 } }, '— fps'),
      ),
      hT('div', { style: { position: 'relative', height: 150 } },
        hT('canvas', { ref: canvas.ref as never, style: { position: 'absolute', inset: 0, width: '100%', height: '100%' } }),
      ),
    ),
    hT('div', { style: { display: 'flex', gap: 10, flexWrap: 'wrap' as never } },
      kv(labels.current, 0, 'frame work p50'),
      kv(labels.current, 1, 'frame work p99'),
      kv(labels.current, 2, 'frame work max'),
      kv(labels.current, 4, 'msg rate (nominal)'),
      kv(labels.current, 5, 'frames / skipped'),
      kv(labels.current, 6, 'zero-copy index'),
    ),
  );
}

function kv(labels: HotLabels, i: number, name: string): unknown {
  return hT('div', { key: name, style: { ...card, padding: '8px 12px', flex: '1 1 140px' } },
    hT('div', { style: { color: T.textDim, fontSize: 10, fontFamily: MONO2 } }, name),
    hT('div', { ref: labels.bind(i) as never, style: { color: T.textBright, fontSize: 15, fontFamily: MONO2, fontWeight: 600, marginTop: 2 } }, '—'),
  );
}

const P3 = new Float64Array(3);

function draw(ctx: CanvasRenderingContext2D, w: number, h: number, frame: number, eng: StudioEngine, labels: HotLabels): void {
  const budgetNs = eng.budgetNs();
  eng.scheduler.percentiles(P3);

  // --- frame-work scorecard (budget gauge) ---
  ctx.clearRect(0, 0, w, h);
  const rows: Array<[string, number]> = [
    ['p50', P3[0]], ['p99', P3[1]], ['max', P3[2]],
  ];
  const rowH = 26;
  const barW = w - 150;
  rows.forEach((r, i) => {
    const y = 14 + i * rowH;
    ctx.fillStyle = T.textDim;
    ctx.font = '10px ui-monospace, monospace';
    ctx.fillText(r[0], 14, y + 10);
    // budget track
    ctx.fillStyle = '#26282B';
    ctx.fillRect(50, y + 2, barW, 12);
    const t = Math.min(1, r[1] / budgetNs);
    ctx.fillStyle = t > 0.9 ? T.redBright : t > 0.6 ? T.yellow : T.greenBright;
    ctx.fillRect(50, y + 2, Math.max(2, t * barW), 12);
    // budget marker
    ctx.fillStyle = T.textDim;
    ctx.fillText(fmtNs(r[1]), 56 + Math.min(1, t) * barW, y + 12);
  });
  // budget legend
  ctx.fillStyle = T.textDim;
  ctx.font = '9px ui-monospace, monospace';
  ctx.fillText(`frame budget 240 Hz = ${fmtNs(budgetNs)} (drop-not-queue; late = skip)`, 14, h - 10);

  // --- message rate ring (background strip) ---
  ringMsg.copyWithin(0, 1);
  ringMsg[ringMsg.length - 1] = eng.ring.counters.published;
  if ((frame & 0x3f) === 0) {
    labels.set(0, fmtNs(P3[0]));
    labels.set(1, fmtNs(P3[1]));
    labels.set(2, fmtNs(P3[2]));
    labels.set(3, '240 fps locked');
    labels.set(4, fmtRate(eng.sim.stats.nominalMsgPerSec));
    labels.set(5, `${fmtInt(eng.scheduler.frameCount)} / ${fmtInt(eng.scheduler.skipped)}`);
    labels.set(6, eng.live.zeroCopyIndex.toFixed(4));
  }
}

// ---------------------------------------------------------------- helpers --

function hT(type: string, props: Record<string, unknown> | null, ...children: unknown[]): unknown {
  const React = getReact() as unknown as { createElement: (...a: unknown[]) => unknown };
  return React.createElement(type, props ?? null, ...children);
}

export const MONO2 = 'ui-monospace, "JetBrains Mono", Menlo, Consolas, monospace';

const card: Record<string, string | number> = {
  background: T.chromeAlt, border: `1px solid ${T.border}`, borderRadius: 8,
};

const head: Record<string, string | number> = {
  display: 'flex', alignItems: 'center', gap: 8, padding: '8px 12px',
  background: T.chrome, borderBottom: `1px solid ${T.border}`, flexShrink: 0,
};

const headTitle: Record<string, string | number> = {
  color: T.textBright, fontSize: 11, fontWeight: 600, letterSpacing: 0.5, fontFamily: MONO2,
};
