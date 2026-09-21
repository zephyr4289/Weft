// orderbook-canvas.js — zero-re-render 240 FPS depth-ladder renderer.
//
// Law 4 contract (docs/adapters/MANAGED-SEAMS-V1.md §7):
//   - The controller reads the MDP1 record DIRECTLY (DataView) every frame.
//     Book state NEVER crosses into React/Flutter/SwiftUI state (W4-06).
//   - Steady state allocates NOTHING: geometry from preallocated scratch,
//     price labels served from a bounded string cache (Map hit path has
//     zero allocations; Pillar 4 lesson — fillText string churn removed).
//   - Transparent degradation (W4-04): a torn record (CRC fail) skips the
//     frame and counts it; a lost context flips to FALLBACK and draws a
//     static snapshot — never an uncaught exception.
//
// The controller is DOM-free and shim-testable: tests drive render(nowNs)
// with a virtual clock and a recording 2D context. DOM shells (React /
// Flutter / SwiftUI) are thin binders around this controller.

import { Mdp1View, MDP1_TOP_LEVELS } from './mdp1.js';

export const DEFAULT_THEME = {
  bg: '#0b0e14',
  bid: '#2ecc71',
  ask: '#e74c3c',
  best: '#f1c40f',
  spread: '#586a8a',
  text: '#c8d3e0',
};

// Bounded price->label string cache. Miss allocates one string (cold),
// hit path is allocation-free. Flush keeps the map bounded forever.
class LabelCache {
  constructor(cap = 4096) { this.map = new Map(); this.cap = cap; }
  label(price) {
    let s = this.map.get(price);
    if (s === undefined) {
      if (this.map.size >= this.cap) this.map.clear();
      s = (price / 10000).toFixed(4);
      this.map.set(price, s);
    }
    return s;
  }
}

export function createOrderBookController(opts) {
  const mdp1 = opts.mdp1 instanceof Mdp1View ? opts.mdp1 : new Mdp1View(opts.mdp1);
  const theme = opts.theme ?? DEFAULT_THEME;
  const width = opts.width ?? 420;
  const height = opts.height ?? 320;
  const telemetry = opts.telemetry ?? null;

  const ctx = opts.ctx ?? null;
  const clock = opts.clock ?? null; // () -> ns, for frame-cost accounting

  const rows = opts.rows ?? MDP1_TOP_LEVELS;
  const rowH = height / (rows * 2 + 2);

  // preallocated render state — zero steady-state allocation
  const state = {
    lastSeq: -1,
    frames: 0,
    drops: 0,       // render cost exceeded frame budget
    torn: 0,        // CRC-failed snapshots skipped
    fallback: false,
    lastCostNs: 0,
    maxCostNs: 0,
    drawCalls: 0,
    bidMax: 1,
    askMax: 1,
  };
  const labels = new LabelCache(opts.labelCacheCap ?? 4096);
  const scratch = new Float64Array(8);

  function draw(nowNs) {
    if (ctx === null) return;
    if (!mdp1.valid() || !mdp1.crcOk()) { state.torn++; return; }
    const seq = mdp1.seq;

    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, width, height);
    state.drawCalls++;

    // depth bars: bids stack downward from center, asks upward
    const barW = width - 8;
    let max = 1;
    for (let i = 0; i < rows; i++) {
      const s = mdp1.bidSize(i); if (s > max) max = s;
      const a = mdp1.askSize(i); if (a > max) max = a;
    }
    const centerY = height / 2;
    ctx.fillStyle = theme.bid;
    for (let i = 0; i < rows; i++) {
      const size = mdp1.bidSize(i);
      if (size === 0) break;
      const w = Math.max(2, (size / max) * (barW / 2));
      const y = centerY + rowH * (i + 0.5);
      ctx.fillRect(4, y, w, rowH - 1);
    }
    ctx.fillStyle = theme.ask;
    for (let i = 0; i < rows; i++) {
      const size = mdp1.askSize(i);
      if (size === 0) break;
      const w = Math.max(2, (size / max) * (barW / 2));
      const y = centerY - rowH * (i + 1.5);
      ctx.fillRect(4, y, w, rowH - 1);
    }

    // spread indicator + best marks
    ctx.fillStyle = theme.spread;
    ctx.fillRect(0, centerY - 1, width, 2);
    const bb = mdp1.bestBid, ba = mdp1.bestAsk;
    ctx.fillStyle = theme.best;
    ctx.fillRect(0, centerY + rowH * 0.5 - 1, 6, 2);
    ctx.fillRect(0, centerY - rowH * 1.5 - 1, 6, 2);

    // price labels (cached strings — allocation-free hit path)
    if (opts.drawLabels !== false && ctx.fillText) {
      ctx.fillStyle = theme.text;
      ctx.font = '10px monospace';
      ctx.fillText(labels.label(bb), 10, centerY + rowH * 1.4);
      ctx.fillText(labels.label(ba), 10, centerY - rowH * 0.6);
      scratch[0] = seq;
    }
  }

  function drawFallback() {
    if (ctx === null) return;
    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, width, height);
    ctx.fillStyle = theme.spread;
    ctx.fillRect(0, height / 2 - 1, width, 2);
  }

  return {
    state,
    mdp1,
    // One scheduled frame at `nowNs`. Frame budget = 1e9/frameRate.
    // Returns true when the frame PRESENTED (drew), false when skipped
    // (torn record — counted in state.torn, never fatal).
    render(nowNs) {
      const t0 = clock !== null ? clock() : 0;
      state.frames++;
      const before = state.drawCalls;
      draw(nowNs);
      if (clock !== null) {
        const cost = clock() - t0;
        state.lastCostNs = cost;
        if (cost > state.maxCostNs) state.maxCostNs = cost;
        const budget = opts.frameBudgetNs ?? (1e9 / (opts.frameRate ?? 240));
        if (cost > budget) state.drops++;
      }
      if (telemetry !== null && telemetry.onFrame !== undefined) telemetry.onFrame(nowNs);
      return state.drawCalls > before;
    },
    // Transparent degradation: flip to FALLBACK static snapshot (W4-04).
    degrade(reason) {
      state.fallback = true;
      drawFallback();
      return reason ?? 'degraded';
    },
    restore() {
      state.fallback = false;
      return true;
    },
  };
}

// DOM binder: rAF loop presenting controller frames at display rate.
// Returns a stop() function. Never calls into React.
export function bindOrderBookCanvas(canvas, controller, opts = {}) {
  let raf = 0;
  let running = true;
  const frameRate = opts.frameRate ?? 240;
  const budget = opts.frameBudgetNs ?? Math.floor(1e9 / frameRate);
  const startNs = opts.nowNs ?? 0;
  let frames = 0;
  let lastBudgetStart = startNs;
  let acc = 0;
  function tick() {
    if (!running) return;
    const nowNs = (opts.clock ? opts.clock() : Date.now() * 1e6);
    // fixed-cadence presentation: up to budget catch-up, drop beyond (Law 4)
    if (nowNs - lastBudgetStart >= budget - 1) {
      acc = nowNs - lastBudgetStart;
      controller.render(nowNs);
      frames++;
      lastBudgetStart = nowNs;
    }
    raf = requestAnimationFrame(tick);
  }
  raf = requestAnimationFrame(tick);
  return function stop() {
    running = false;
    if (raf) cancelAnimationFrame(raf);
  };
}
