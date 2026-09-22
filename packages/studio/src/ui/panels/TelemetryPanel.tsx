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
      kv(labels.current, 5, 'frames rendered'),
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

const ROW_NAMES = ['p50', 'p99', 'max'] as const;
const BUDGET_TEXT = `frame budget 240 Hz = 4.17 ms (drop-not-queue; late = skip)`;

function draw(ctx: CanvasRenderingContext2D, w: number, h: number, frame: number, eng: StudioEngine, labels: HotLabels): void {
  const budgetNs = eng.budgetNs();
  eng.scheduler.percentiles(P3);

  // --- frame-work scorecard (budget gauge; geometry only, closure-free) ---
  ctx.clearRect(0, 0, w, h);
  const rowH = 26;
  const barW = w - 150;
  for (let ri = 0; ri < 3; ri++) {
    const name = ri === 0 ? 'p50' : ri === 1 ? 'p99' : 'max';
    const val = P3[ri];
    const y = 14 + ri * rowH;
    ctx.fillStyle = T.textDim;
    ctx.font = '10px ui-monospace, monospace';
    ctx.fillText(name, 14, y + 10);
    // budget track
    ctx.fillStyle = '#26282B';
    ctx.fillRect(50, y + 2, barW, 12);
    const t = Math.min(1, val / budgetNs);
    ctx.fillStyle = t > 0.9 ? T.redBright : t > 0.6 ? T.yellow : T.greenBright;
    ctx.fillRect(50, y + 2, Math.max(2, t * barW), 12);
  }
  // budget legend (static string constant — zero runtime allocation)
  ctx.fillStyle = T.textDim;
  ctx.font = '9px ui-monospace, monospace';
  ctx.fillText(BUDGET_TEXT, 14, h - 10);

  if ((frame & 0x3f) === 0) {
    labels.setNum(0, P3[0], fmtNs);
    labels.setNum(1, P3[1], fmtNs);
    labels.setNum(2, P3[2], fmtNs);
    const skipped = eng.scheduler.skipped;
    if (skipped !== lastSkipped) {
      const perSec = (skipped - lastSkipped) * 4;
      lastSkipped = skipped;
      labels.set(3, perSec === 0 ? '240 fps locked' : '240 fps · gov-skip ' + perSec + '/s');
    }
    labels.setNum(4, eng.sim.stats.nominalMsgPerSec, fmtRate);
    labels.setNum(5, eng.scheduler.frameCount, fmtInt);
    labels.setNum(6, eng.live.zeroCopyIndex, zcFmt);
  }
}

let lastSkipped = -1;
function zcFmt(v: number): string { return v.toFixed(4); }

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
