'use client';

/**
 * Weft Studio — Panel B: Cache-Line & Memory Alignment Mapper.
 * Interactive 64B/128B cache-line visualization: color-coded field blocks
 * with offset/size, explicit compiler padding, the False-Sharing Hazard
 * Indicator, and the memory-density gauge. The mapper itself is cold
 * (schema-driven); a small hot canvas animates per-line coherency bounce for
 * hazard lines under the live stream.
 */

import { getReact, SPY } from '../../engine/react-adapter';
import { markRender, useHotCanvas, fmtBytes } from '../hooks';
import { STUDIO_THEME as T } from '../theme';
import type { LayoutResult, StructLayout } from '../../engine/layout';

interface Props {
  layout: LayoutResult;
  engine: import('../../engine/studio-engine').StudioEngine | null;
  lineSize: 64 | 128;
}

const LANE_COLORS = ['#3574F0', '#C77DBB', '#4A88C7', '#DCA878'];

export function CacheMapperPanel({ layout, engine, lineSize }: Props): unknown {
  markRender(SPY.CACHE_MAPPER);
  const React = getReact() as unknown as {
    useState: <S>(v: S) => [S, (v: S) => void];
  };
  const [expanded, setExpanded] = React.useState<Record<string, boolean>>({});
  const traffic = useHotCanvas(engine, 'memoryGrid', drawTraffic);

  return hMap('div', { style: { height: '100%', overflow: 'auto', padding: 14, background: T.bg, display: 'flex', flexDirection: 'column', gap: 12 } },
    hMap('div', { style: rowHead },
      hMap('span', { style: headTitle }, 'CACHE-LINE MAPPER'),
      hMap('span', { style: { flex: 1 } }),
      hMap('span', { style: chip(T.accentSoft) }, `${lineSize}B lines`),
      hMap('span', { style: chip(layout.hazards64.length ? 'rgba(224,108,117,.18)' : 'rgba(73,156,84,.18)') },
        `${layout.hazards64.length} false-sharing hazard${layout.hazards64.length === 1 ? '' : 's'}`),
    ),
    layout.order.map((name) => {
      const L = layout.layouts.get(name)!;
      const open = expanded[name] !== false;
      return hMap('div', { key: name, style: structCard },
        hMap('div', { style: { ...structHead, cursor: 'pointer' }, onClick: () => toggle(setExpanded, name) },
          hMap('span', { style: { color: open ? T.accent : T.textDim, fontSize: 10 } }, open ? '▾' : '▸'),
          hMap('span', { style: { color: T.textBright, fontFamily: MONO, fontSize: 12.5, fontWeight: 600 } }, name),
          hMap('span', { style: chip('rgba(53,116,240,.16)') }, `size ${L.size}B`),
          hMap('span', { style: chip('rgba(53,116,240,.16)') }, `align ${L.alignment}B`),
          hMap('span', { style: { flex: 1 } }),
          densityGauge(L),
        ),
        open ? hMap('div', { style: { padding: '10px 12px 12px' } },
          lineRows(L, lineSize),
          hMap('div', { style: { display: 'flex', gap: 14, marginTop: 8, flexWrap: 'wrap' } },
            hMap('span', { style: legendDot('#2F3134') }, 'padding (explicit)'),
            hMap('span', { style: legendDot(LANE_COLORS[0]) }, 'writer lane 0'),
            hMap('span', { style: legendDot(LANE_COLORS[1]) }, 'writer lane 1'),
            hMap('span', { style: legendDot('#4A88C7') }, 'unassigned'),
          ),
        ) : null,
      );
    }),
    // hot coherency-bounce strip
    hMap('div', { style: { ...structCard, padding: 0, flexShrink: 0 } },
      hMap('div', { style: { ...structHead, borderTopLeftRadius: 8, borderTopRightRadius: 8 } },
        hMap('span', { style: headTitle }, 'LIVE COHERENCY BOUNCE'),
        hMap('span', { style: { flex: 1 } }),
        hMap('span', { style: { color: T.textDim, fontSize: 10 } }, 'hazard lines · 240 Hz governor'),
      ),
      hMap('canvas', { ref: traffic.ref as never, style: { display: 'block', width: '100%', height: 72 } }),
    ),
  );
}

function toggle(set: (fn: (p: Record<string, boolean>) => Record<string, boolean>) => void, name: string): void {
  set((p) => ({ ...p, [name]: p[name] === false }));
}

function lineRows(L: StructLayout, lineSize: number): unknown[] {
  const rows: unknown[] = [];
  const nLines = Math.ceil(L.size / lineSize);
  const hazards = new Set<number>();
  // recompute hazard lines for THIS struct from leaves
  const byLine = new Map<number, { writers: Set<number>; labels: string[] }>();
  for (const leaf of L.leaves) {
    if (leaf.writer < 0) continue;
    const first = Math.floor(leaf.offset / lineSize);
    const last = Math.floor((leaf.offset + leaf.size - 1) / lineSize);
    for (let ln = first; ln <= last; ln++) {
      let b = byLine.get(ln);
      if (!b) { b = { writers: new Set(), labels: [] }; byLine.set(ln, b); }
      b.writers.add(leaf.writer);
      b.labels.push(`${leaf.struct}.${leaf.field}`);
    }
  }
  for (const [ln, b] of byLine) if (b.writers.size >= 2) hazards.add(ln);

  for (let ln = 0; ln < nLines; ln++) {
    const start = ln * lineSize;
    const isHazard = hazards.has(ln);
    const hz = byLine.get(ln);
    rows.push(hMap('div', { key: ln, style: { marginBottom: 4 } },
      hMap('div', { style: { display: 'flex', alignItems: 'center', gap: 8, marginBottom: 2 } },
        hMap('span', { style: { color: T.textDim, fontSize: 9.5, fontFamily: MONO, width: 52 } }, `0x${start.toString(16).padStart(3, '0')}`),
        isHazard ? hMap('span', { style: { color: T.red, fontSize: 9.5, fontFamily: MONO } },
          `⚡ FALSE SHARING — writers {${Array.from(hz!.writers).join(', ')}} on this line (${hz!.labels.join(', ')})`) : null,
      ),
      hMap('div', {
        title: isHazard ? 'fields from different concurrent writers share one cache line' : '',
        style: {
          position: 'relative', height: 22, borderRadius: 4, overflow: 'hidden',
          background: isHazard ? 'rgba(224,108,117,.10)' : '#232527',
          border: `1px solid ${isHazard ? 'rgba(224,108,117,.55)' : T.borderSoft}`,
          marginLeft: 60,
        },
      },
        // pads + leaves for this line
        L.pads.filter((p) => p.offset >= start && p.offset < start + lineSize).map((p, i) =>
          hMap('div', {
            key: 'p' + i,
            style: {
              position: 'absolute', left: `${((p.offset - start) / lineSize) * 100}%`,
              width: `${(Math.min(p.size, start + lineSize - p.offset) / lineSize) * 100}%`, top: 0, bottom: 0,
              background: 'repeating-linear-gradient(45deg, #26282B 0 3px, #2F3134 3px 6px)',
              borderRight: '1px dashed #3A3D42',
            },
          }, p.size >= 10 ? hMap('span', { style: padLabel }, `pad +${p.size}`) : null)),
        L.leaves.filter((l) => l.offset >= start && l.offset < start + lineSize).map((l, i) => {
          const color = l.writer >= 0 ? LANE_COLORS[l.writer % LANE_COLORS.length] : '#4A88C7';
          const width = Math.min(l.size, start + lineSize - l.offset);
          return hMap('div', {
            key: 'l' + i,
            title: `${l.struct}.${l.field} : ${l.type}  @ +${l.offset}  (${l.size}B, align ${l.align}B${l.writer >= 0 ? `, writer ${l.writer}` : ''})`,
            style: {
              position: 'absolute', left: `${((l.offset - start) / lineSize) * 100}%`,
              width: `${(width / lineSize) * 100}%`, top: 0, bottom: 0,
              background: color + '2E', borderRight: '1px solid ' + color + '66',
              borderLeft: `2px solid ${color}`, display: 'flex', alignItems: 'center', overflow: 'hidden',
            },
          }, width >= 34 ? hMap('span', { style: { fontSize: 9.5, fontFamily: MONO, color: T.textBright, paddingLeft: 6, whiteSpace: 'nowrap' as never } },
            `${l.field} +${l.offset}·${l.size}B`) : null);
        }),
      ),
    ));
  }
  return rows;
}

function densityGauge(L: StructLayout): unknown {
  const pct = Math.round(L.efficiency * 100);
  return hMap('div', { style: { display: 'flex', alignItems: 'center', gap: 8 } },
    hMap('div', { style: { width: 120, height: 8, borderRadius: 4, background: '#26282B', overflow: 'hidden' } },
      hMap('div', { style: { width: `${pct}%`, height: '100%', background: pct > 75 ? T.greenBright : pct > 45 ? T.yellow : T.red } })),
    hMap('span', { style: { color: pct > 75 ? T.greenBright : pct > 45 ? T.yellow : T.red, fontSize: 10, fontFamily: MONO, width: 108 } },
      `${pct}% used · +${L.paddingBytes}B pad (${fmtBytes(L.paddingBytes)})`),
  );
}

/** Hot: deterministic coherency-bounce dots on hazard lines. */
function drawTraffic(ctx: CanvasRenderingContext2D, w: number, h: number, frame: number): void {
  ctx.clearRect(0, 0, w, h);
  // two writer lanes bouncing against each other every ~40 frames
  const period = 40;
  const ph0 = ((frame % period) / period) * Math.PI * 2;
  const ph1 = ph0 + Math.PI;
  const y0 = h * 0.5 + Math.sin(ph0) * h * 0.3;
  const y1 = h * 0.5 + Math.sin(ph1) * h * 0.3;
  ctx.fillStyle = 'rgba(224,108,117,.14)';
  ctx.fillRect(0, 0, w, h);
  ctx.strokeStyle = 'rgba(224,108,117,.5)';
  ctx.beginPath();
  ctx.moveTo(0, h / 2); ctx.lineTo(w, h / 2);
  ctx.stroke();
  // lane 0 dots sweeping right, lane 1 sweeping left — collisions flash
  const x0 = ((frame * 7) % (w + 40)) - 20;
  const x1 = w - (((frame * 7) % (w + 40)) - 20);
  ctx.fillStyle = LANE_COLORS[0];
  ctx.fillRect(x0 - 5, y0 - 5, 10, 10);
  ctx.fillStyle = LANE_COLORS[1];
  ctx.fillRect(x1 - 5, y1 - 5, 10, 10);
  const collide = Math.abs(x0 - x1) < 18 && Math.abs(y0 - y1) < 18;
  if (collide) {
    ctx.fillStyle = 'rgba(242,113,120,.9)';
    ctx.fillRect(0, 0, w, h);
  }
  ctx.fillStyle = '#6F737A';
  ctx.font = '9px ui-monospace, monospace';
  ctx.fillText('W0', 6, 12);
  ctx.fillText('W1', 6, h - 6);
  void collide;
}

// ---------------------------------------------------------------- helpers --

function hMap(type: string, props: Record<string, unknown> | null, ...children: unknown[]): unknown {
  const React = getReact() as unknown as { createElement: (...a: unknown[]) => unknown };
  return React.createElement(type, props ?? null, ...children);
}

export const MONO = 'ui-monospace, "JetBrains Mono", Menlo, Consolas, monospace';

const rowHead: Record<string, string | number> = {
  display: 'flex', alignItems: 'center', gap: 8, flexShrink: 0,
};

const headTitle: Record<string, string | number> = {
  color: T.textBright, fontSize: 11, fontWeight: 600, letterSpacing: 0.5, fontFamily: MONO,
};

const structCard: Record<string, string | number> = {
  background: T.chromeAlt, border: `1px solid ${T.border}`, borderRadius: 8, flexShrink: 0,
};

const structHead: Record<string, string | number> = {
  display: 'flex', alignItems: 'center', gap: 8, padding: '8px 12px', flexWrap: 'wrap' as never,
};

const padLabel: Record<string, string | number> = {
  fontSize: 9, fontFamily: MONO, color: T.textDim, paddingLeft: 5, whiteSpace: 'nowrap' as never,
};

function chip(bg: string): Record<string, string | number> {
  return { padding: '1px 8px', borderRadius: 10, fontSize: 10, fontFamily: MONO, color: T.textBright, background: bg };
}

function legendDot(color: string): Record<string, string | number> {
  return { color: T.textDim, fontSize: 10, fontFamily: MONO, borderLeft: `10px solid ${color}`, paddingLeft: 6 };
}
