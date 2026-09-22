'use client';

/**
 * Weft Studio — Panel C: Live Ring Occupancy & Seqlock Contention HUD.
 * Hot plane: a circular 1,000,000-slot map visualizer (slot states FREE /
 * WRITING / COMMITTED / READ / DROPPED), live write-head marker, seqlock
 * version + torn-read retry meters, and a producer backpressure contention
 * heatmap. Geometry-only canvas; numeric labels mutate DOM text nodes at
 * ~4 Hz with change detection. ZERO React re-renders while streaming.
 */

import { getReact, SPY } from '../../engine/react-adapter';
import { markRender, useHotCanvas, HotLabels, fmtInt, fmtRate } from '../hooks';
import { STUDIO_THEME as T } from '../theme';
import type { StudioEngine } from '../../engine/studio-engine';
import { SLOT_FREE, SLOT_WRITING, SLOT_COMMITTED, SLOT_READ, SLOT_DROPPED } from '../../engine/ring';

interface Props { engine: StudioEngine | null }

const STATE_COLORS: Record<number, string> = {
  [SLOT_FREE]: '#2F3134',
  [SLOT_WRITING]: '#DCA878',
  [SLOT_COMMITTED]: '#499C54',
  [SLOT_READ]: '#3574F0',
  [SLOT_DROPPED]: '#E06C75',
};

const CELLS = 240;
const HEAT_COLS = 240;

/** Rolling per-frame producer pressure (msgs/frame). */
const heatCols = new Float64Array(HEAT_COLS);
let heatPrevPublished = 0;

export function RingMonitorPanel({ engine }: Props): unknown {
  markRender(SPY.RING_MONITOR);
  const React = getReact() as unknown as { useRef: (v: never) => { current: never } };
  const labels = React.useRef(new HotLabels()) as unknown as { current: HotLabels };
  const eng = engine;

  const ring = useHotCanvas(engine, 'ring', (ctx, w, h, frame) => {
    if (!eng) return;
    drawRing(ctx, w, h, eng);
    updateLabels(labels.current, eng, frame);
  });
  const heat = useHotCanvas(engine, 'ring', (ctx, w, h) => {
    if (!eng) return;
    drawHeat(ctx, w, h, eng);
  });

  return hRing('div', { style: { height: '100%', overflow: 'auto', padding: 14, display: 'flex', flexDirection: 'column', gap: 12, background: T.bg } },
    hRing('div', { style: { display: 'flex', gap: 12, flex: 1, minHeight: 0, flexWrap: 'wrap' as never } },
      hRing('div', { style: { ...card, flex: '1 1 340px', display: 'flex', flexDirection: 'column' } },
        hRing('div', { style: head },
          hRing('span', { style: headTitle }, 'RING MAP — 1,000,000 SLOTS × 32B'),
          hRing('span', { style: { flex: 1 } }),
          hRing('span', { style: { color: T.textDim, fontSize: 10, fontFamily: MONOR } }, 'live seqlock states'),
        ),
        hRing('div', { style: { flex: 1, position: 'relative', minHeight: 260 } },
          hRing('canvas', { ref: ring.ref as never, style: { position: 'absolute', inset: 0, width: '100%', height: '100%' } }),
        ),
        hRing('div', { style: { display: 'flex', gap: 12, padding: '8px 12px', flexWrap: 'wrap' as never } },
          legend(SLOT_FREE, 'FREE'),
          legend(SLOT_WRITING, 'WRITING'),
          legend(SLOT_COMMITTED, 'COMMITTED'),
          legend(SLOT_READ, 'READ'),
          legend(SLOT_DROPPED, 'DROPPED'),
        ),
      ),
      hRing('div', { style: { ...card, flex: '1 1 320px', display: 'flex', flexDirection: 'column' } },
        hRing('div', { style: head },
          hRing('span', { style: headTitle }, 'SEQLOCK & CONTENTION'),
          hRing('span', { style: { flex: 1 } }),
          hRing('span', { style: { color: T.textDim, fontSize: 10, fontFamily: MONOR } }, 'torn retries · backpressure rungs'),
        ),
        hRing('div', { style: { padding: '12px 14px', display: 'flex', flexDirection: 'column', gap: 10 } },
          meter(labels.current, 0, 'occupancy'),
          meter(labels.current, 1, 'published'),
          meter(labels.current, 2, 'dropped'),
          meter(labels.current, 3, 'torn-read retries'),
          meter(labels.current, 4, 'seqlock head seq'),
          meter(labels.current, 5, 'message rate'),
          meter(labels.current, 6, 'zero-copy index'),
        ),
        hRing('div', { style: { flex: 1, position: 'relative', minHeight: 140, borderTop: `1px solid ${T.borderSoft}` } },
          hRing('canvas', { ref: heat.ref as never, style: { position: 'absolute', inset: 0, width: '100%', height: '100%' } }),
        ),
      ),
    ),
  );
}

function meter(labels: HotLabels, i: number, name: string): unknown {
  return hRing('div', { key: name, style: { display: 'flex', alignItems: 'baseline', gap: 10 } },
    hRing('span', { style: { color: T.textDim, fontSize: 11, width: 132, fontFamily: MONOR } }, name),
    hRing('span', { ref: labels.bind(i) as never, style: { color: T.textBright, fontSize: 14, fontFamily: MONOR, fontWeight: 600 } }, '—'),
  );
}

function legend(state: number, name: string): unknown {
  return hRing('span', { key: name, style: legendStyle },
    hRing('span', { style: { width: 9, height: 9, borderRadius: 2, background: STATE_COLORS[state], display: 'inline-block', marginRight: 5 } }),
    name,
  );
}

// --------------------------------------------------------------- drawing --

function drawRing(ctx: CanvasRenderingContext2D, w: number, hh: number, engine: StudioEngine): void {
  ctx.clearRect(0, 0, w, hh);
  const cx = w / 2, cy = hh / 2;
  const R = Math.min(w, hh) / 2 - 26;
  if (R <= 20) return;
  const rIn = R - 13;
  const ring = engine.ring;
  const cap = ring.capacity;
  const stride = Math.max(1, Math.floor(cap / CELLS));
  const head = ring.writeIndex;

  for (let i = 0; i < CELLS; i++) {
    const slot = (i * stride) % cap;
    const state = ring.slotState(slot);
    const a0 = (i / CELLS) * Math.PI * 2 - Math.PI / 2;
    const a1 = ((i + 0.82) / CELLS) * Math.PI * 2 - Math.PI / 2;
    ctx.beginPath();
    ctx.arc(cx, cy, R, a0, a1);
    ctx.arc(cx, cy, rIn, a1, a0, true);
    ctx.closePath();
    ctx.fillStyle = STATE_COLORS[state];
    ctx.globalAlpha = state === SLOT_FREE ? 0.5 : 1;
    ctx.fill();
    ctx.globalAlpha = 1;
  }

  // write head marker
  const headCell = Math.floor((head / cap) * CELLS) % CELLS;
  const ha = (headCell / CELLS) * Math.PI * 2 - Math.PI / 2 + (0.41 / CELLS) * Math.PI * 2;
  ctx.beginPath();
  ctx.moveTo(cx + Math.cos(ha) * (R + 4), cy + Math.sin(ha) * (R + 4));
  ctx.lineTo(cx + Math.cos(ha) * (R + 16), cy + Math.sin(ha) * (R + 16));
  ctx.strokeStyle = T.yellow;
  ctx.lineWidth = 2.5;
  ctx.stroke();

  // center readout (geometry only)
  ctx.textAlign = 'center';
  ctx.fillStyle = T.textBright;
  ctx.font = '600 20px ui-monospace, monospace';
  ctx.fillText(String(engine.live.occupancy), cx, cy - 4);
  ctx.font = '10px ui-monospace, monospace';
  ctx.fillStyle = T.textDim;
  ctx.fillText('slots resident', cx, cy + 12);
  ctx.fillText(`w-head ${head}`, cx, cy + 30);
  ctx.textAlign = 'left';
}

function drawHeat(ctx: CanvasRenderingContext2D, w: number, hh: number, engine: StudioEngine): void {
  ctx.clearRect(0, 0, w, hh);
  ctx.fillStyle = '#232527';
  ctx.fillRect(0, 0, w, hh);
  const published = engine.ring.counters.published;
  const per = published - heatPrevPublished;
  heatPrevPublished = published;
  heatCols.copyWithin(0, 1);
  heatCols[HEAT_COLS - 1] = per;
  const max = 60000;
  const colW = w / HEAT_COLS;
  for (let i = 0; i < HEAT_COLS; i++) {
    const v = heatCols[i] / max;
    if (v <= 0) continue;
    const t = v > 1 ? 1 : v;
    const r = Math.round(73 + t * 151);
    const g = Math.round(156 - t * 48);
    const b = Math.round(84 - t * 9);
    ctx.fillStyle = `rgb(${r},${g},${b})`;
    const bh = Math.max(2, t * (hh - 18));
    ctx.fillRect(i * colW, hh - 12 - bh, Math.max(1, colW), bh);
  }
  ctx.fillStyle = '#6F737A';
  ctx.font = '9px ui-monospace, monospace';
  ctx.fillText('producer backpressure — msgs/frame (rung scale 0…60k)', 8, hh - 3);
}

function updateLabels(labels: HotLabels, engine: StudioEngine, frame: number): void {
  if ((frame & 0x3f) !== 0) return; // ~4 Hz at 240 Hz
  const occ = engine.live.occupancy;
  labels.set(0, `${occ} · ${((occ / engine.ring.capacity) * 100).toFixed(2)}%`);
  labels.set(1, fmtInt(engine.ring.counters.published));
  labels.set(2, fmtInt(engine.ring.counters.dropped));
  labels.set(3, fmtInt(engine.ring.counters.tornRetries));
  labels.set(4, fmtInt(engine.ring.slotSeq((engine.ring.writeIndex + engine.ring.capacity - 1) % engine.ring.capacity)));
  labels.set(5, fmtRate(engine.live.msgPerSec || engine.sim.stats.producedTicks * 240));
  labels.set(6, engine.live.zeroCopyIndex.toFixed(4));
}

// ---------------------------------------------------------------- helpers --

function hRing(type: string, props: Record<string, unknown> | null, ...children: unknown[]): unknown {
  const React = getReact() as unknown as { createElement: (...a: unknown[]) => unknown };
  return React.createElement(type, props ?? null, ...children);
}

export const MONOR = 'ui-monospace, "JetBrains Mono", Menlo, Consolas, monospace';

const card: Record<string, string | number> = {
  background: T.chromeAlt, border: `1px solid ${T.border}`, borderRadius: 8, overflow: 'hidden',
};

const head: Record<string, string | number> = {
  display: 'flex', alignItems: 'center', gap: 8, padding: '8px 12px',
  background: T.chrome, borderBottom: `1px solid ${T.border}`, flexShrink: 0,
};

const headTitle: Record<string, string | number> = {
  color: T.textBright, fontSize: 11, fontWeight: 600, letterSpacing: 0.5, fontFamily: MONOR,
};

const legendStyle: Record<string, string | number> = {
  display: 'inline-flex', alignItems: 'center', fontSize: 10, fontFamily: MONOR, color: T.textDim,
};
