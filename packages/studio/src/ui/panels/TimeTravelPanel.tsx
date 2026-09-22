'use client';

/**
 * Weft Studio — Panel D: Flight Recorder Time-Travel Debugger (.weftrec).
 * Microsecond-precision scrub bar with frame-by-frame step, interactive
 * memory state diff viewer (before/after lane payloads on mutation events),
 * checkpoint timeline, and One-Click Crash Export (SREC1 + SBURST bundle).
 * The record-rate sparkline is hot; scrub/diff are cold interactions.
 */

import { getReact, SPY } from '../../engine/react-adapter';
import { markRender, useHotCanvas, HotLabels, fmtInt } from '../hooks';
import { STUDIO_THEME as T } from '../theme';
import type { StudioEngine } from '../../engine/studio-engine';
import { TimeTravelReplayer, REPLAY_LANES, diffStates } from '../../engine/flightrec';
import { CHECKPOINT_INTERVAL } from '../../engine/types';

interface Props { engine: StudioEngine | null }

const rateRing = new Float64Array(240);

export function TimeTravelPanel({ engine }: Props): unknown {
  markRender(SPY.TIME_TRAVEL);
  const React = getReact() as unknown as {
    useRef: (v: never) => { current: never };
    useState: <S>(v: S) => [S, (v: S) => void];
    useMemo: <T>(fn: () => T, deps: unknown[]) => T;
  };
  const labels = React.useRef(new HotLabels()) as unknown as { current: HotLabels };
  const [attached, setAttached] = React.useState(-1); // -1 = following tail
  const [replayerVersion, setReplayerVersion] = React.useState(0);
  const eng = engine;

  const count = engine ? engine.flight.recordCount : 0;
  const pos = attached < 0 ? count : Math.min(attached, count);

  const replayer = React.useMemo(() => {
    void replayerVersion;
    if (!eng || eng.flight.recordCount === 0) return null;
    return eng.freezeReplay();
  }, [eng, replayerVersion, Math.floor(count / CHECKPOINT_INTERVAL)]) as TimeTravelReplayer | null;

  const cur = React.useMemo(() => {
    if (!replayer) return null;
    const s = new Uint32Array(REPLAY_LANES * 2);
    replayer.scrubTo(pos, s);
    return s;
  }, [replayer, pos]) as Uint32Array | null;

  const prev = React.useMemo(() => {
    if (!replayer || pos <= 0) return null;
    const s = new Uint32Array(REPLAY_LANES * 2);
    replayer.scrubTo(pos - 1, s);
    return s;
  }, [replayer, pos]) as Uint32Array | null;

  const diffOut = React.useMemo(() => {
    if (!cur || !prev) return null;
    const out = new Int32Array(REPLAY_LANES * 3);
    const n = diffStates(prev, cur, out);
    return { out, n };
  }, [cur, prev]);

  const spark = useHotCanvas(engine, 'telemetry', (ctx, w, h, frame) => {
    if (!eng) return;
    drawSpark(ctx, w, h, frame, eng, labels.current);
  });

  const attach = () => {
    if (!eng) return;
    eng.freezeReplay();
    setReplayerVersion((v) => v + 1);
    setAttached(eng.flight.recordCount);
  };

  const followTail = () => setAttached(-1);

  const exportCrash = () => {
    if (!eng) return;
    const bundle = eng.exportCrashBundle();
    try {
      const blob = new Blob([bundle.slice().buffer as ArrayBuffer], { type: 'application/octet-stream' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = 'weft-studio-crash.srecburst';
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 4000);
    } catch { /* download unavailable in this host — bundle computed above */ }
  };

  return hTT('div', { style: { height: '100%', overflow: 'auto', padding: 14, display: 'flex', flexDirection: 'column', gap: 12, background: T.bg } },
    // header + sparkline
    hTT('div', { style: card },
      hTT('div', { style: head },
        hTT('span', { style: headTitle }, 'FLIGHT LOG — SREC1 (.weftrec sidecar)'),
        hTT('span', { style: { flex: 1 } }),
        hTT('span', { ref: labels.current.bind(0) as never, style: { color: T.textBright, fontSize: 11, fontFamily: MONOT } }, '0 records'),
        hTT('button', { onClick: attach as never, style: btn }, 'ATTACH REPLAYER'),
        hTT('button', { onClick: exportCrash as never, style: { ...btn, borderColor: T.red, color: T.redBright } }, 'ONE-CLICK CRASH EXPORT'),
      ),
      hTT('div', { style: { position: 'relative', height: 88 } },
        hTT('canvas', { ref: spark.ref as never, style: { position: 'absolute', inset: 0, width: '100%', height: '100%' } }),
      ),
    ),
    // scrub
    hTT('div', { style: card },
      hTT('div', { style: { ...head } },
        hTT('span', { style: headTitle }, 'TIME-TRAVEL SCRUB'),
        hTT('span', { style: { flex: 1 } }),
        hTT('span', { style: { color: attached < 0 ? T.greenBright : T.yellow, fontSize: 10, fontFamily: MONOT } },
          attached < 0 ? 'FOLLOWING TAIL' : `PARKED @ ${pos}`),
        hTT('button', { onClick: followTail as never, style: btn }, 'FOLLOW TAIL'),
        hTT('button', { onClick: () => setAttached(Math.max(0, pos - 1)) as never, style: btn }, '◀ STEP'),
        hTT('button', { onClick: () => setAttached(Math.min(count, pos + 1)) as never, style: btn }, 'STEP ▶'),
      ),
      hTT('div', { style: { padding: '14px 16px' } },
        hTT('input', {
          type: 'range', min: 0, max: Math.max(0, count), value: pos,
          onChange: (e: Event) => {
            const v = parseInt((e.target as HTMLInputElement).value, 10) | 0;
            setAttached(v);
          },
          style: { width: '100%', accentColor: T.accent, cursor: 'pointer' },
        }),
        hTT('div', { style: { display: 'flex', justifyContent: 'space-between', marginTop: 6 } },
          hTT('span', { style: { color: T.textDim, fontSize: 10, fontFamily: MONOT } }, 'record 0'),
          hTT('span', { style: { color: T.textDim, fontSize: 10, fontFamily: MONOT } },
            `checkpoint every ${CHECKPOINT_INTERVAL} records`),
          hTT('span', { style: { color: T.textDim, fontSize: 10, fontFamily: MONOT } }, `record ${count}`),
        ),
        engine && engine.replayer && pos > 0 && engine.flight.recordCount > 0
          ? hTT('div', { style: { marginTop: 10, color: T.textDim, fontSize: 10.5, fontFamily: MONOT } },
            `scrub ts: ${(engine.flight.tsAt(Math.min(pos - 1, engine.flight.recordCount - 1)) / 1e6).toFixed(3)} ms  ·  addr 0x${engine.flight.addrAt(Math.min(pos - 1, engine.flight.recordCount - 1)).toString(16)}  ·  seq ${engine.flight.seqOldAt(Math.min(pos - 1, engine.flight.recordCount - 1))}→${engine.flight.seqNewAt(Math.min(pos - 1, engine.flight.recordCount - 1))}`)
          : null,
      ),
    ),
    // state diff
    hTT('div', { style: card },
      hTT('div', { style: head },
        hTT('span', { style: headTitle }, 'MEMORY STATE DIFF — 8-LANE SHADOW VECTOR (XOR-DELTA FOLD)'),
        hTT('span', { style: { flex: 1 } }),
        diffOut && diffOut.n > 0
          ? hTT('span', { style: { color: T.yellow, fontSize: 10, fontFamily: MONOT } }, `${diffOut.n} lane${diffOut.n === 1 ? '' : 's'} mutated`)
          : hTT('span', { style: { color: T.textDim, fontSize: 10, fontFamily: MONOT } }, 'no mutation at this step'),
      ),
      hTT('div', { style: { padding: '10px 14px', display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: 8 } },
        Array.from({ length: REPLAY_LANES }, (_, lane) => {
          const laneKey = lane;
          const changed = diffOut ? Array.from(diffOut.out.slice(0, diffOut.n * 3)).some((x, i) => i % 3 === 0 && x === laneKey) : false;
          const before = prev ? `0x${hex(prev[lane * 2])}${hex(prev[lane * 2 + 1])}` : '—';
          const after = cur ? `0x${hex(cur[lane * 2])}${hex(cur[lane * 2 + 1])}` : '—';
          return hTT('div', { key: lane, style: {
            background: changed ? 'rgba(224,108,117,.10)' : '#232527',
            border: `1px solid ${changed ? 'rgba(224,108,117,.5)' : T.borderSoft}`,
            borderRadius: 6, padding: '8px 10px',
          } },
            hTT('div', { style: { color: changed ? T.redBright : T.textDim, fontSize: 10, fontFamily: MONOT, marginBottom: 4 } }, `lane ${lane}${changed ? '  ⚡ mutated' : ''}`),
            hTT('div', { style: { color: T.textDim, fontSize: 10.5, fontFamily: MONOT } }, `before ${before}`),
            hTT('div', { style: { color: changed ? T.textBright : T.text, fontSize: 12, fontFamily: MONOT, fontWeight: 600 } }, `after  ${after}`),
          );
        }),
      ),
    ),
  );
}

function hex(v: number): string {
  return (v >>> 0).toString(16).padStart(8, '0');
}

let lastRecordCount = 0;

/** Hot: record-rate sparkline + checkpoint ticks + tail cursor. */
function drawSpark(ctx: CanvasRenderingContext2D, w: number, hh: number, _frame: number, eng: StudioEngine, labels: HotLabels): void {
  const count = eng.flight.recordCount;
  rateRing.copyWithin(0, 1);
  rateRing[rateRing.length - 1] = count - lastRecordCount;
  lastRecordCount = count;
  ctx.clearRect(0, 0, w, hh);
  ctx.fillStyle = '#232527';
  ctx.fillRect(0, 0, w, hh);
  const max = Math.max(4, ...Array.from(rateRing));
  const colW = w / rateRing.length;
  for (let i = 0; i < rateRing.length; i++) {
    const v = rateRing[i] / max;
    ctx.fillStyle = `rgba(53,116,240,${0.25 + v * 0.65})`;
    const bh = Math.max(1, v * (hh - 24));
    ctx.fillRect(i * colW, hh - 14 - bh, Math.max(1, colW - 1), bh);
  }
  // checkpoint ticks (fixed grid note: the sparkline is per-frame, not per-record)
  ctx.fillStyle = 'rgba(220,168,120,.75)';
  ctx.fillStyle = '#6F737A';
  ctx.font = '9px ui-monospace, monospace';
  ctx.fillText(`flight log ingest — records/frame (peak ${fmtInt(max)})`, 8, hh - 3);
  if ((eng.live.frame & 0x3f) === 0) {
    labels.set(0, `${fmtInt(count)} records`);
  }
}

// ---------------------------------------------------------------- helpers --

function hTT(type: string, props: Record<string, unknown> | null, ...children: unknown[]): unknown {
  const React = getReact() as unknown as { createElement: (...a: unknown[]) => unknown };
  return React.createElement(type, props ?? null, ...children);
}

export const MONOT = 'ui-monospace, "JetBrains Mono", Menlo, Consolas, monospace';

const card: Record<string, string | number> = {
  background: T.chromeAlt, border: `1px solid ${T.border}`, borderRadius: 8, flexShrink: 0, overflow: 'hidden',
};

const head: Record<string, string | number> = {
  display: 'flex', alignItems: 'center', gap: 8, padding: '8px 12px',
  background: T.chrome, borderBottom: `1px solid ${T.border}`, flexWrap: 'wrap' as never,
};

const headTitle: Record<string, string | number> = {
  color: T.textBright, fontSize: 11, fontWeight: 600, letterSpacing: 0.5, fontFamily: MONOT,
};

const btn: Record<string, string | number> = {
  padding: '3px 10px', fontSize: 10, fontFamily: MONOT, background: T.accentSoft, color: T.textBright,
  border: `1px solid ${T.accent}`, borderRadius: 4, cursor: 'pointer',
};
