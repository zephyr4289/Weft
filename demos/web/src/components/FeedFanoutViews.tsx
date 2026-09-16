// FeedFanoutViews.tsx — RFC-0004 fan-out feed views: one writer, three
// independent consumers.
//
// WHY EXISTS: The Series-2 fan-out PR deliberately left demo integration
// as a follow-up (multi-canvas sketch in PR-5). W6 is the workload that
// needs it: one feed writer drives three views — a depth ladder, a trade
// tape, and a stats HUD — which is exactly the 1-writer/N-reader case the
// kernel Triad does not cover and RFC-0004 (accepted as a driver-layer
// pattern, round-6 §4) exists for. Each WeftFanoutCanvas mounts its own
// WeftFanoutReader against the shared WeftFanoutBroadcaster; each view
// keeps its own fresh/drop accounting (claim.dropped), so a slow view
// degrades alone — never blocking the writer (Law 1) and never allocating
// on the data path (Law 2).
//
// HONESTY LABELS: the producer runs on the MAIN thread at rAF cadence
// (60 Hz x 50 msgs = ~3,000 msgs/s simulated — labeled in the panel
// header). The cross-thread writer case (feed in a worker thread) is
// covered by the package-level litmus tradition: demos/web/test/
// feedfanout.test.ts runs an INDEPENDENT worker-side writer against real
// readers, per the F-series cross-thread precedent.
//
// The draw phase may allocate (fillText/toFixed strings) — that is the
// sanctioned Draw-phase discipline (A3: reads and renders happen inside
// the frame callback); the DATA path (feed -> engine -> publish -> claim)
// allocates nothing.

import React, { useEffect, useRef } from 'react';
import { WeftFanoutBroadcaster, FreshnessGovernor, GovernorActionKind, type FanoutClaim } from '@weft/core';
import { drawW6FeedLadder } from '../workloads/draw';
import { decideDraw } from '../modes/governorPolicy';
import {
  SyntheticL2Feed,
  L2BookEngine,
  W6_FLOAT_COUNT,
  W6_FEED_TO_DISPLAY,
  W6_MSG_FIELDS,
  W6_HEADER_FLOATS,
  W6_LEVELS,
  W6_LADDER_FIELDS,
} from '../workloads/l2feed';

// ---------------------------------------------------------------------------
// View draws. The ladder reuses drawW6FeedLadder (the FAIRNESS PIN's one
// draw implementation per workload, extended across surfaces). The tape
// and stats draws are view-specific and local to this panel.
// ---------------------------------------------------------------------------

function drawLadderView(ctx: CanvasRenderingContext2D, floats: Float32Array): void {
  drawW6FeedLadder(ctx, ctx.canvas.width, ctx.canvas.height, floats);
}

function drawTapeView(ctx: CanvasRenderingContext2D, floats: Float32Array): void {
  const width = ctx.canvas.width;
  const height = ctx.canvas.height;
  ctx.clearRect(0, 0, width, height);
  const TAPE_BASE = W6_HEADER_FLOATS + W6_LEVELS * W6_LADDER_FIELDS;
  const midY = height / 2;
  // Price scale from the tape's own min/max (falls back to mid band when
  // fewer than two trades have printed).
  let lo = Infinity;
  let hi = -Infinity;
  for (let j = 0; j < 64; j++) {
    const side = floats[TAPE_BASE + j * 3 + 2];
    if (side === 0) continue;
    const px = floats[TAPE_BASE + j * 3];
    if (px < lo) lo = px;
    if (px > hi) hi = px;
  }
  const span = hi > lo ? hi - lo : 1;
  const pad = height * 0.15;
  for (let j = 0; j < 64; j++) {
    const o = TAPE_BASE + j * 3;
    const side = floats[o + 2];
    if (side === 0) continue;
    const px = floats[o];
    const sz = floats[o + 1];
    const x = (width * (j + 1)) / 65;
    const y = pad + ((hi - px) / span) * (height - 2 * pad);
    ctx.fillStyle = side > 0 ? '#4ade80' : '#f87171';
    ctx.fillRect(x, y - 2, 3, 3 + Math.min(sz, 5));
  }
  // Mid reference line.
  ctx.fillStyle = '#334155';
  ctx.fillRect(0, midY, width, 1);
}

function drawStatsView(
  ctx: CanvasRenderingContext2D,
  floats: Float32Array,
  claim: FanoutClaim
): void {
  const width = ctx.canvas.width;
  const height = ctx.canvas.height;
  ctx.clearRect(0, 0, width, height);
  const imb =
    floats[4] + floats[5] > 0 ? (floats[4] - floats[5]) / (floats[4] + floats[5]) : 0;
  const rows: Array<[string, string]> = [
    ['mid', floats[2].toFixed(2)],
    ['spread', floats[3].toFixed(2)],
    ['bid depth', String(Math.round(floats[4]))],
    ['ask depth', String(Math.round(floats[5]))],
    ['last', floats[6].toFixed(2)],
    ['imbalance', imb.toFixed(3)],
    ['folded/frame', String(Math.round(floats[1]))],
    ['frame seq', String(claim.seq)],
    ['this view drops', String(claim.dropped)],
  ];
  ctx.font = '12px monospace';
  const rowH = height / (rows.length + 1);
  ctx.fillStyle = '#38bdf8';
  ctx.fillText('FEED STATS (own reader)', 8, rowH * 0.75);
  for (let i = 0; i < rows.length; i++) {
    const y = rowH * (i + 1.75);
    ctx.fillStyle = '#94a3b8';
    ctx.fillText(rows[i][0], 8, y);
    ctx.fillStyle = '#f8fafc';
    ctx.fillText(rows[i][1], width * 0.5, y);
  }
}

// ---------------------------------------------------------------------------
// GovernedFanoutView — the RFC-0009 wiring: framesBehind -> gov.step().
//
// One view = one reader + one FreshnessGovernor. Per tick:
//   claim = reader.claim()                       (the fan-out analog of
//    framesBehind: claim.dropped — frames published between this view's
//    claims that it never saw, same semantics, per RFC-0008)
//   action = gov.step(claim.dropped, performance.now())
//   decision = decideDraw(action, seq !== lastDrawn)   (governorPolicy.ts)
// Idempotent redraws are elided (FastPath + same seq -> no memcpy, no
// raster — the saved-draw counter is on the HUD); Skip(n) draws the newest
// frame once while the n intermediates are counted as DECIDED drops in the
// governor's own counter; Snapshot forces one draw; Reseed (rate-limited,
// 250 ms) resets the view's baseline after drawing.
// ---------------------------------------------------------------------------

interface GovernedViewProps {
  broadcaster: WeftFanoutBroadcaster;
  draw: (ctx: CanvasRenderingContext2D, floats: Float32Array, claim: FanoutClaim) => void;
  /** Poll cadence: 'raf' (display rate) or a fixed ms period (slow views). */
  cadenceMs: 'raf' | number;
  width: number;
  height: number;
  style?: React.CSSProperties;
}

const ACTION_LABEL = ['FAST', 'SKIP', 'SNAP', 'RESEED'] as const;
const ACTION_COLOR = ['#34d399', '#fbbf24', '#38bdf8', '#f87171'] as const;

function GovernedFanoutView({ broadcaster, draw, cadenceMs, width, height, style }: GovernedViewProps) {
  const canvasRef = React.useRef<HTMLCanvasElement>(null);
  const drawRef = React.useRef(draw);
  drawRef.current = draw;

  React.useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const reader = broadcaster.createReader();
    const gov = new FreshnessGovernor();
    let lastDrawnSeq = -1;
    let savedDraws = 0;
    let drawn = 0;
    let raf = 0;
    let timer = 0;
    let disposed = false;

    const tick = () => {
      if (disposed) return;
      const claim = reader.claim();
      const action = gov.step(claim.dropped, performance.now());
      const seqChanged = claim.seq !== lastDrawnSeq;
      const d = decideDraw(action, seqChanged);
      if (d.draw) {
        drawRef.current(ctx, reader.view(), claim);
        drawn++;
        lastDrawnSeq = claim.seq;
        // Governor HUD strip (top 14px): action + live counters.
        ctx.font = '10px monospace';
        ctx.fillStyle = '#020617';
        ctx.fillRect(0, 0, width, 14);
        ctx.fillStyle = ACTION_COLOR[action.kind] ?? '#94a3b8';
        const label = action.kind === GovernorActionKind.Skip
          ? `SKIP(${action.skipN})`
          : ACTION_LABEL[action.kind];
        ctx.fillText(
          `${label}  behind=${claim.dropped}  draws=${drawn}  elided=${savedDraws}  decided-drops=${gov.decidedDrops}  reseeds=${gov.reseeds}`,
          4, 10
        );
      } else {
        savedDraws++;
      }
      if (d.reset) {
        // Reseed: the view rebuilds its baseline (drawn once above); the
        // cooldown makes this at most one per 250 ms.
        lastDrawnSeq = claim.seq;
      }
      schedule();
    };

    const schedule = () => {
      if (cadenceMs === 'raf') {
        raf = requestAnimationFrame(tick);
      } else {
        timer = window.setTimeout(tick, cadenceMs);
      }
    };
    tick();

    return () => {
      disposed = true;
      cancelAnimationFrame(raf);
      window.clearTimeout(timer);
    };
  }, [broadcaster, cadenceMs, width]); // draw intentionally excluded — latest-ref.

  return <canvas ref={canvasRef} width={width} height={height} style={style} />;
}

// ---------------------------------------------------------------------------
// Panel — the single writer (feed + engine + broadcaster) lives here.
// ---------------------------------------------------------------------------

interface ProducerState {
  feed: SyntheticL2Feed;
  engine: L2BookEngine;
  batch: Float64Array;
  tick: number;
}

export const FeedFanoutViews: React.FC<{ running: boolean }> = ({ running }) => {
  const bRef = useRef<WeftFanoutBroadcaster | null>(null);
  if (bRef.current === null) {
    bRef.current = new WeftFanoutBroadcaster(W6_FLOAT_COUNT, 4);
  }
  const stRef = useRef<ProducerState | null>(null);
  if (stRef.current === null) {
    stRef.current = {
      feed: new SyntheticL2Feed(),
      engine: new L2BookEngine(),
      batch: new Float64Array(W6_FEED_TO_DISPLAY * W6_MSG_FIELDS),
      tick: 0,
    };
  }

  // The single writer loop: fold the next tick's 50 messages and publish
  // one frame into the fan-out ring. Zero allocation per tick.
  useEffect(() => {
    if (!running) return;
    const b = bRef.current;
    const st = stRef.current;
    if (!b || !st) return;
    let raf = 0;
    const tickFn = () => {
      st.tick++;
      st.feed.nextTick(st.batch);
      st.engine.setGrid(st.feed.midC, st.feed.bidOff, st.feed.askOff);
      st.engine.beginTick();
      st.engine.applyBatch(st.batch, W6_FEED_TO_DISPLAY);
      st.engine.exportFrame(b.begin(), st.tick);
      b.publish();
      raf = requestAnimationFrame(tickFn);
    };
    raf = requestAnimationFrame(tickFn);
    return () => cancelAnimationFrame(raf);
  }, [running]);

  const b = bRef.current;
  const canvasStyle: React.CSSProperties = {
    background: '#020617',
    border: '1px solid #334155',
    borderRadius: '6px',
    display: 'block',
  };

  return (
    <div
      style={{
        background: '#0f172a',
        border: '1px solid #334155',
        borderRadius: '8px',
        padding: '16px',
        marginBottom: '20px',
      }}
    >
      <div style={{ fontSize: '13px', color: '#38bdf8', marginBottom: '4px', fontWeight: 'bold' }}>
        RFC-0004 Fan-Out Feed Views + RFC-0009 Freshness Governor — 1 writer · 3 governed consumers
      </div>
      <div style={{ fontSize: '11px', color: '#94a3b8', marginBottom: '12px' }}>
        SIMULATED FEED · main-thread rAF producer @ ~3,000 msgs/s (60 Hz × 50 msgs folded) ·
        cross-thread writer covered by test/feedfanout.test.ts · each view owns its reader,
        its drop accounting, and its FreshnessGovernor: framesBehind (claim.dropped) →
        gov.step() → FastPath / Skip(n) / Snapshot / Reseed (250 ms cooldown) · the HUD
        strip on each canvas shows the live ladder · the measured savings proof is
        scripts/governor_bench.ts (B4 display-adversarial matrix)
      </div>
      <div style={{ display: 'flex', gap: '12px', flexWrap: 'wrap' }}>
        <div>
          <div style={{ fontSize: '11px', color: '#94a3b8', marginBottom: '4px' }}>
            Depth Ladder · rAF · mostly FastPath
          </div>
          {b && <GovernedFanoutView broadcaster={b} draw={drawLadderView} cadenceMs="raf" width={420} height={250} style={canvasStyle} />}
        </div>
        <div>
          <div style={{ fontSize: '11px', color: '#94a3b8', marginBottom: '4px' }}>
            Trade Tape · 90 ms · Skip territory
          </div>
          {b && <GovernedFanoutView broadcaster={b} draw={drawTapeView} cadenceMs={90} width={270} height={250} style={canvasStyle} />}
        </div>
        <div>
          <div style={{ fontSize: '11px', color: '#94a3b8', marginBottom: '4px' }}>
            Stats HUD · 250 ms · Snapshot/Reseed territory
          </div>
          {b && <GovernedFanoutView broadcaster={b} draw={drawStatsView} cadenceMs={250} width={210} height={250} style={canvasStyle} />}
        </div>
      </div>
    </div>
  );
};
