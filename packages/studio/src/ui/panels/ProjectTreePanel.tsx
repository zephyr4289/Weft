'use client';

/**
 * Weft Studio — Project / Schema Tree tool window (cold): workspace files and
 * the live schema structure (structs → fields with offsets/types/writers),
 * rebuilt on every schema edit.
 */

import { getReact, SPY } from '../../engine/react-adapter';
import { markRender } from '../hooks';
import { STUDIO_THEME as T } from '../theme';
import type { ParseResult } from '../../engine/schema';
import type { LayoutResult } from '../../engine/layout';

interface Props {
  parse: ParseResult;
  layout: LayoutResult;
  onOpenFile: (name: string) => void;
  activeFile: string;
}

const FILES = [
  { name: 'main.weft', kind: 'weft' },
  { name: 'goldens/', kind: 'dir' },
  { name: 'flight-demo/', kind: 'dir' },
  { name: 'D-73-STUDIO-MANAGED-AUDIT.md', kind: 'doc' },
  { name: 'desktop/tauri/', kind: 'dir' },
];

export function ProjectTreePanel({ parse, layout, onOpenFile, activeFile }: Props): unknown {
  markRender(SPY.PROJECT_TREE);
  return hP('div', { style: { height: '100%', overflow: 'auto', background: T.chromeAlt } },
    hP('div', { style: sectionTitle }, 'WORKSPACE'),
    FILES.map((f) => hP('div', {
      key: f.name,
      onClick: () => f.kind === 'weft' && onOpenFile(f.name),
      style: {
        ...fileRow,
        background: activeFile === f.name ? T.selection : 'transparent',
        color: activeFile === f.name ? T.textBright : T.text,
        cursor: f.kind === 'weft' ? 'pointer' : 'default',
      },
    },
      hP('span', { style: { color: f.kind === 'dir' ? T.yellow : f.kind === 'weft' ? T.accent : T.textDim, width: 14 } },
        f.kind === 'dir' ? '▸' : f.kind === 'weft' ? '◆' : '≡'),
      f.name,
    )),
    hP('div', { style: { ...sectionTitle, borderTop: `1px solid ${T.borderSoft}`, marginTop: 4 } }, 'SCHEMA TREE'),
    parse.doc && parse.ok ? layout.order.map((name) => {
      const L = layout.layouts.get(name)!;
      return hP('div', { key: name },
        hP('div', { style: structRow },
          hP('span', { style: { color: T.purple } }, '◇'),
          hP('span', { style: { color: T.textBright } }, name),
          hP('span', { style: { color: T.textDim, fontSize: 9.5, marginLeft: 'auto', fontFamily: MONOP } }, `${L.size}B · ${Math.round(L.efficiency * 100)}%`),
        ),
        L.leaves.map((l, i) => hP('div', { key: i, style: fieldRow },
          hP('span', { style: { color: T.textDim, fontFamily: MONOP, fontSize: 9.5, width: 44 } }, `+${l.offset}`),
          hP('span', { style: { color: T.tokField, fontFamily: MONOP, fontSize: 10.5, flex: 1 } }, l.field),
          hP('span', { style: { color: T.tokPrim, fontFamily: MONOP, fontSize: 9.5 } }, l.type),
          l.writer >= 0 ? hP('span', { style: { color: T.tokAttr, fontFamily: MONOP, fontSize: 9.5, marginLeft: 4 } }, `w${l.writer}`) : null,
        )),
        L.leaves.length === 0 ? hP('div', { style: { ...fieldRow, color: T.red } }, 'empty struct') : null,
      );
    }) : hP('div', { style: { ...fieldRow, color: T.red } }, 'schema has errors — tree unavailable'),
  );
}

// ---------------------------------------------------------------- helpers --

function hP(type: string, props: Record<string, unknown> | null, ...children: unknown[]): unknown {
  const React = getReact() as unknown as { createElement: (...a: unknown[]) => unknown };
  return React.createElement(type, props ?? null, ...children);
}

export const MONOP = 'ui-monospace, "JetBrains Mono", Menlo, Consolas, monospace';

const sectionTitle: Record<string, string | number> = {
  padding: '8px 12px 4px', color: T.textDim, fontSize: 9.5, letterSpacing: 1,
  fontWeight: 700, fontFamily: MONOP, position: 'sticky' as never, top: 0,
  background: T.chromeAlt, zIndex: 2,
};

const fileRow: Record<string, string | number> = {
  display: 'flex', alignItems: 'center', gap: 6, padding: '3px 12px', fontSize: 11.5,
  fontFamily: MONOP,
};

const structRow: Record<string, string | number> = {
  display: 'flex', alignItems: 'center', gap: 6, padding: '3px 12px', fontSize: 11.5, fontFamily: MONOP,
};

const fieldRow: Record<string, string | number> = {
  display: 'flex', alignItems: 'center', gap: 4, padding: '1px 12px 1px 26px',
};
