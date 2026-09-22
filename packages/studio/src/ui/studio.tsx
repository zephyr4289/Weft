'use client';

/**
 * Weft Studio — root IDE shell.
 * Android Studio-grade chrome: menu bar, collapsible docked tool windows
 * (Project / Schema Tree, Cache Inspector, Render Spy, Telemetry HUD,
 * Problems, Deliverables), center editor tabs, and the bottom status bar.
 *
 * The rAF loop lives here (started ONCE per mount) and drives the engine's
 * governed 240 Hz wall cadence. Streaming data NEVER enters React state —
 * the two-plane law (STUDIO-SEAMS-V1 §1).
 */

import { getReact, spy, SPY } from '../engine/react-adapter';
import { markRender } from './hooks';
import { STUDIO_THEME as T } from './theme';
import { StudioEngine } from '../engine/studio-engine';
import { parseSchema, validateRefs, type ParseResult } from '../engine/schema';
import { computeLayout, type LayoutResult } from '../engine/layout';
import { SchemaDesignerPanel, FONT } from './panels/SchemaDesignerPanel';
import { CacheMapperPanel } from './panels/CacheMapperPanel';
import { RingMonitorPanel, MONOR } from './panels/RingMonitorPanel';
import { TimeTravelPanel, MONOT } from './panels/TimeTravelPanel';
import { TelemetryPanel, MONO2 } from './panels/TelemetryPanel';
import { RenderSpyPanel, MONOS } from './panels/RenderSpyPanel';
import { StatusBar } from './panels/StatusBar';
import { ProjectTreePanel, MONOP } from './panels/ProjectTreePanel';
import { DeliverablesPanel } from './panels/DeliverablesPanel';

export const CANONICAL_SCHEMA = `// Weft Studio workspace — main.weft
// Nested struct + deliberate 64B false-sharing hazard (writer 0 vs 1).
struct Level {
  price: i64;    // fixed-point 1e-9
  qty: u32;
  flags: u8;
}

struct MarketTick {
  @writer(0) ts_ns: u64;
  @writer(0) bid: Level;
  @writer(1) ask: Level;      // HAZARD: shares cache line 0 with writer 0
  @writer(1) last_px: i64;
  venue: u16;
  @align(64) seq: u64;
}
`;

type CenterTab = 'schema' | 'cache' | 'ring' | 'travel';

interface MenuDef {
  label: string;
  items: Array<{ label: string; onClick?: () => void; checked?: boolean; sep?: boolean }>;
}

export function WeftStudio(): unknown {
  markRender(SPY.STUDIO_ROOT);
  const React = getReact() as unknown as {
    useState: <S>(v: S | (() => S)) => [S, (v: S | ((p: S) => S)) => void];
    useEffect: (fn: () => void | (() => void), deps?: unknown[]) => void;
    useMemo: <T>(fn: () => T, deps: unknown[]) => T;
    useRef: (v: never) => { current: never };
  };

  const [mounted, setMounted] = React.useState(false);
  const [engine, setEngine] = React.useState<StudioEngine | null>(null);
  const [src, setSrc] = React.useState(CANONICAL_SCHEMA);
  const [tab, setTab] = React.useState<CenterTab>('schema');
  const [running, setRunning] = React.useState(true);
  const [rate, setRate] = React.useState(1_000_000);
  const [showLeft, setShowLeft] = React.useState(true);
  const [showRight, setShowRight] = React.useState(true);
  const [showBottom, setShowBottom] = React.useState(true);
  const [openMenu, setOpenMenu] = React.useState<string | null>(null);
  const [lineSize, setLineSize] = React.useState<64 | 128>(64);

  React.useEffect(() => {
    setMounted(true);
    return () => setMounted(false);
  }, []);

  React.useEffect(() => {
    if (!mounted || engine) return;
    const e = new StudioEngine('studio-canonical', 1_000_000, 1_000_000);
    setEngine(e);
  }, [mounted, engine]);

  // rAF drive loop — started once; calls engine.driveWall() per animation frame
  React.useEffect(() => {
    if (!engine || !running) return;
    let raf = 0;
    let alive = true;
    const loop = () => {
      if (!alive) return;
      engine.driveWall();
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => {
      alive = false;
      cancelAnimationFrame(raf);
    };
  }, [engine, running]);

  React.useEffect(() => {
    if (engine) engine.sim.setRate(rate);
  }, [engine, rate]);

  const parse: ParseResult = React.useMemo(() => {
    const r = parseSchema(src);
    if (r.doc) r.diagnostics.push(...validateRefs(r.doc));
    r.ok = r.ok && r.diagnostics.every((d) => d.severity !== 'error');
    return r;
  }, [src]);

  const layout: LayoutResult = React.useMemo(
    () => computeLayout(parse.doc ?? { structs: [], hash: '' }),
    [parse],
  );

  if (!mounted || !engine) {
    return hR('div', { style: { ...root, display: 'flex', alignItems: 'center', justifyContent: 'center' } },
      hR('div', { style: { color: T.textDim, fontFamily: MONOR, fontSize: 12 } }, 'Weft Studio — attaching to the memory map…'),
    );
  }

  const menus: MenuDef[] = [
    {
      label: 'File',
      items: [
        { label: 'Reset schema to canonical', onClick: () => setSrc(CANONICAL_SCHEMA) },
        { label: 'Reset ring + flight log', onClick: () => window.location.reload() },
      ],
    },
    {
      label: 'View',
      items: [
        { label: 'Tool Window: Project / Schema', checked: showLeft, onClick: () => setShowLeft((v) => !v) },
        { label: 'Tool Window: Render Spy', checked: showRight, onClick: () => setShowRight((v) => !v) },
        { label: 'Tool Window: Telemetry HUD', checked: showBottom, onClick: () => setShowBottom((v) => !v) },
        { label: 'Cache line size: 128B', checked: lineSize === 128, onClick: () => setLineSize((s) => (s === 64 ? 128 : 64)) },
      ],
    },
    {
      label: 'Run',
      items: [
        { label: running ? 'Pause stream' : 'Start stream', onClick: () => setRunning((v) => !v) },
        { label: 'Rate: 1,000,000 msg/s', checked: rate === 1_000_000, onClick: () => setRate(1_000_000) },
        { label: 'Rate: 10,000,000 msg/s (burst)', checked: rate === 10_000_000, onClick: () => setRate(10_000_000) },
      ],
    },
    {
      label: 'Help',
      items: [
        { label: 'Weft Studio — Pillar 7 (SEAMS-V1)' },
        { label: 'Two-plane law: hot planes never re-render' },
      ],
    },
  ];

  const tabs: Array<[CenterTab, string]> = [
    ['schema', 'Schema Designer'],
    ['cache', 'Cache-Line Mapper'],
    ['ring', 'Ring & Seqlock HUD'],
    ['travel', 'Time-Travel Debugger'],
  ];

  return hR('div', {
    style: { ...root },
    onMouseDown: () => openMenu && setOpenMenu(null),
  },
    // ---------------- menu bar ----------------
    hR('div', { style: menuBar },
      hR('span', { style: { display: 'flex', alignItems: 'center', gap: 8 } },
        hR('span', { style: { color: T.accent, fontWeight: 700, fontSize: 12.5, fontFamily: MONOR } }, '⧗ Weft Studio'),
        hR('span', { style: { color: T.textDim, fontSize: 9.5, fontFamily: MONOR } }, 'Pillar 7 · zero-copy visual flagship'),
      ),
      hR('span', { style: { flex: 1 } }),
      menus.map((m) => hR('div', { key: m.label, style: { position: 'relative' } },
        hR('button', {
          onMouseDown: (e: Event) => { e.stopPropagation(); setOpenMenu((cur) => (cur === m.label ? null : m.label)); },
          onMouseEnter: () => openMenu && setOpenMenu(m.label),
          style: {
            ...menuBtn,
            background: openMenu === m.label ? T.hover : 'transparent',
            color: openMenu === m.label ? T.textBright : T.text,
          },
        }, m.label),
        openMenu === m.label ? hR('div', { style: menuPopup, onMouseDown: (e: Event) => e.stopPropagation() },
          m.items.map((it, i) => it.sep
            ? hR('div', { key: i, style: { height: 1, background: T.borderSoft, margin: '4px 0' } })
            : hR('button', { key: i, onClick: () => { it.onClick?.(); setOpenMenu(null); }, style: menuItemStyle },
              hR('span', { style: { width: 14, color: T.accent } }, it.checked ? '✓' : ''),
              it.label,
            )),
        ) : null,
      )),
      hR('span', { style: { flex: 1 } }),
      hR('button', { onClick: () => setRunning((v) => !v), style: { ...menuBtn, color: running ? T.greenBright : T.yellow } },
        running ? '■ PAUSE' : '▶ RUN'),
    ),
    // ---------------- main area ----------------
    hR('div', { style: { flex: 1, display: 'flex', minHeight: 0 } },
      // left dock
      showLeft ? hR('div', { style: dockLeft },
        hR('div', { style: twHeader }, 'PROJECT · SCHEMA TREE'),
        hR('div', { style: { flex: 1, minHeight: 0 } },
          ProjectTreePanel({ parse, layout, onOpenFile: () => setTab('schema'), activeFile: 'main.weft' })),
        hR('div', { style: twHeader }, 'DELIVERABLES'),
        hR('div', { style: { height: 210, flexShrink: 0 } }, DeliverablesPanel()),
      ) : null,
      // center
      hR('div', { style: { flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' } },
        hR('div', { style: tabBar },
          tabs.map(([id, label]) => hR('button', {
            key: id,
            onClick: () => setTab(id),
            style: { ...tabBtn, background: tab === id ? T.bg : 'transparent', color: tab === id ? T.textBright : T.textDim, borderTop: tab === id ? `2px solid ${T.accent}` : '2px solid transparent' },
          }, label)),
        ),
        hR('div', { style: { flex: 1, minHeight: 0, position: 'relative' } },
          tab === 'schema' ? SchemaDesignerPanel({ src, onSrc: setSrc, parse, layout, schemaHash: parse.doc?.hash ?? '' })
            : tab === 'cache' ? CacheMapperPanel({ layout, engine, lineSize })
            : tab === 'ring' ? RingMonitorPanel({ engine })
            : TimeTravelPanel({ engine }),
        ),
      ),
      // right dock
      showRight ? hR('div', { style: dockRight },
        hR('div', { style: twHeader }, 'RENDER SPY'),
        hR('div', { style: { height: 260, flexShrink: 0, position: 'relative' } }, RenderSpyPanel({ engine })),
        hR('div', { style: twHeader }, 'PROBLEMS'),
        hR('div', { style: { flex: 1, minHeight: 0, overflow: 'auto', padding: '6px 10px' } },
          parse.diagnostics.length === 0
            ? hR('div', { style: { color: T.greenBright, fontSize: 10.5, fontFamily: MONOS } }, '✓ no schema problems')
            : parse.diagnostics.map((d, i) => hR('div', { key: i, style: { padding: '3px 0', fontSize: 10.5, fontFamily: MONOS, color: d.severity === 'error' ? T.redBright : T.yellow } },
              `${d.severity === 'error' ? '⨯' : '⚠'} L${d.line}:${d.col} — ${d.message}`)),
        ),
      ) : null,
    ),
    // ---------------- bottom dock ----------------
    showBottom ? hR('div', { style: { height: 280, flexShrink: 0, borderTop: `1px solid ${T.border}`, background: T.bg } },
      TelemetryPanel({ engine }),
    ) : null,
    // ---------------- status bar ----------------
    StatusBar({ engine, schemaHash: parse.doc?.hash ?? '' }),
  );
}

// ---------------------------------------------------------------- helpers --

function hR(type: string, props: Record<string, unknown> | null, ...children: unknown[]): unknown {
  const React = getReact() as unknown as { createElement: (...a: unknown[]) => unknown };
  return React.createElement(type, props ?? null, ...children);
}

const root: Record<string, string | number> = {
  height: '100vh', width: '100%', display: 'flex', flexDirection: 'column',
  background: T.bg, color: T.text, overflow: 'hidden', userSelect: 'none' as never,
};

const menuBar: Record<string, string | number> = {
  height: 36, flexShrink: 0, display: 'flex', alignItems: 'center', gap: 2,
  padding: '0 10px', background: T.chromeDeep, borderBottom: `1px solid ${T.border}`,
};

const menuBtn: Record<string, string | number> = {
  padding: '4px 10px', fontSize: 12, borderRadius: 5, cursor: 'pointer', border: 'none',
  fontFamily: FONT,
};

const menuPopup: Record<string, string | number> = {
  position: 'absolute', top: '100%', left: 0, zIndex: 60, minWidth: 280,
  background: T.chromeDeep, border: `1px solid ${T.border}`, borderRadius: 8,
  boxShadow: '0 12px 32px rgba(0,0,0,.55)', padding: 5,
};

const menuItemStyle: Record<string, string | number> = {
  display: 'flex', alignItems: 'center', gap: 6, width: '100%', textAlign: 'left' as never,
  padding: '6px 8px', fontSize: 12, color: T.text, background: 'transparent',
  border: 'none', borderRadius: 5, cursor: 'pointer', fontFamily: FONT,
};

const dockLeft: Record<string, string | number> = {
  width: 280, flexShrink: 0, borderRight: `1px solid ${T.border}`, background: T.chromeAlt,
  display: 'flex', flexDirection: 'column', minHeight: 0,
};

const dockRight: Record<string, string | number> = {
  width: 300, flexShrink: 0, borderLeft: `1px solid ${T.border}`, background: T.chromeAlt,
  display: 'flex', flexDirection: 'column', minHeight: 0,
};

const twHeader: Record<string, string | number> = {
  padding: '7px 12px', fontSize: 10, letterSpacing: 1, fontWeight: 700, color: T.textDim,
  background: T.chrome, borderBottom: `1px solid ${T.borderSoft}`, flexShrink: 0,
  fontFamily: MONOP,
};

const tabBar: Record<string, string | number> = {
  display: 'flex', gap: 1, background: T.chrome, borderBottom: `1px solid ${T.border}`,
  flexShrink: 0,
};

const tabBtn: Record<string, string | number> = {
  padding: '8px 16px', fontSize: 12, cursor: 'pointer', border: 'none', fontFamily: FONT,
};

void spy;
