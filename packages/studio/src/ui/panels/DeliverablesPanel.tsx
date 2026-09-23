'use client';

/**
 * Weft Studio — Deliverables tool window (cold): the Pillar release archive
 * index, served from the studio webapp's static root. Pillar 7 sits on top.
 */

import { getReact, SPY } from '../../engine/react-adapter';
import { markRender } from '../hooks';
import { STUDIO_THEME as T } from '../theme';

interface Entry { file: string; label: string; note: string }

const ENTRIES: Entry[] = [
  { file: 'weftc-pillar7-managed.zip', label: 'Pillar 7 — Weft Studio', note: 'this release · sources, tests, evidence, D-73 audit' },
  { file: 'weftc-pillar6-managed.zip', label: 'Pillar 6 — Adapters (ACCEPTED)', note: 'fintech + robotics connectors · D-63 audit' },
  { file: 'weftc-pillar5-managed.zip', label: 'Pillar 5 — Spectrum (ACCEPTED)', note: 'managed spectrum SDKs + governors' },
  { file: 'weftc-pillar4-managed.zip', label: 'Pillar 4 — heddle-2.0 (ACCEPTED)', note: 'zero-re-render UI connectors' },
  { file: 'weftc-pillar3-managed.zip', label: 'Pillar 3 — Cluster (ACCEPTED)', note: 'cluster SDKs, topology, observability' },
  { file: 'weftc-pillar2-managed.zip', label: 'Pillar 2 — Tensor (ACCEPTED)', note: 'GPU-resident tensor accelerators' },
  { file: 'weftc-pillar1.zip', label: 'Pillar 1 — weftc (ACCEPTED)', note: 'managed codegen compiler' },
];

export function DeliverablesPanel(): unknown {
  markRender(SPY.DELIVERABLES);
  return hD('div', { style: { height: '100%', overflow: 'auto', background: T.chromeAlt, padding: '8px 6px' } },
    ENTRIES.map((e) => hD('a', {
      key: e.file,
      href: '/' + e.file,
      download: true,
      style: row,
    },
      hD('span', { style: { color: T.yellow, width: 14 } }, '▣'),
      hD('span', { style: { flex: 1, minWidth: 0 } },
        hD('div', { style: { color: T.textBright, fontSize: 11, fontFamily: MONOD, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' as never } }, e.label),
        hD('div', { style: { color: T.textDim, fontSize: 9.5, fontFamily: MONOD, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' as never } }, e.note),
      ),
      hD('span', { style: { color: T.accent, fontSize: 10, fontFamily: MONOD } }, '↓ zip'),
    )),
  );
}

// ---------------------------------------------------------------- helpers --

function hD(type: string, props: Record<string, unknown> | null, ...children: unknown[]): unknown {
  const React = getReact() as unknown as { createElement: (...a: unknown[]) => unknown };
  return React.createElement(type, props ?? null, ...children);
}

export const MONOD = 'ui-monospace, "JetBrains Mono", Menlo, Consolas, monospace';

const row: Record<string, string | number> = {
  display: 'flex', alignItems: 'center', gap: 8, padding: '6px 8px', borderRadius: 6,
  textDecoration: 'none',
};
