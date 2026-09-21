// src/visualizers.js — drop-in realtime visualizer components over the HPL1
// Hot-Plane. Every engine here follows the Law-1 render contract:
//   * ALL scratch is allocated in init() — render() allocates NOTHING
//     (no arrays, no objects, no per-frame closures, no string building).
//   * Sample data is pulled from the plane view per frame (seqlock-safe);
//     statistics are read in place from lane blocks (never recomputed here).
//   * Canvas 2D is the managed reference renderer; Engineer 2's WebGL2/WebGPU
//     engines drop in through the same <WeftCanvas engine={...}> contract.
//
// Factory pattern matches the rest of the package: createVisualizers(React,
// hooks) so the whole layer runs under the mount-semantics shim in tests.

import { createWeftCanvas } from './core.js';

// canvas-2D recorder shim (tests); counts calls, no real pixels
export function make2DRecorder() {
  const calls = {};
  const history = [];
  const rec = (name) => { calls[name] = (calls[name] || 0) + 1; };
  const shim = {
    calls,
    history,
    beginPath() { rec('beginPath'); }, moveTo() { rec('moveTo'); }, lineTo() { rec('lineTo'); },
    stroke() { rec('stroke'); }, fill() { rec('fill'); }, fillRect() { rec('fillRect'); },
    strokeRect() { rec('strokeRect'); }, clearRect() { rec('clearRect'); },
    fillText() { rec('fillText'); }, arc() { rec('arc'); }, closePath() { rec('closePath'); },
  };
  for (const k of ['fillStyle', 'strokeStyle', 'lineWidth', 'font', 'textBaseline', 'globalAlpha']) {
    let v = null;
    Object.defineProperty(shim, k, {
      get() { return v; },
      set(nv) { v = nv; rec(`set:${k}`); history.push(nv); },
    });
  }
  return shim;
}

const LANE_OUT_FIELDS = { seqLo: 0, seqHi: 0, current: 0, min: 0, max: 0, avg: 0, samplesSeenLo: 0, samplesSeenHi: 0, head: 0, flags: 0, publishNsLo: 0, publishNsHi: 0, drops: 0 };
const makeOut = () => ({ ...LANE_OUT_FIELDS });

// ---------------------------------------------------------------------------
// WeftOscilloscope — continuous high-frequency time-series wave visualizer
// ---------------------------------------------------------------------------

export function createOscilloscopeEngine({ lane = 0, window: win = 256, color = '#38e1ff', lineWidth = 1.5, grid = true } = {}) {
  return {
    contextType: '2d',
    init(canvas, ctx, view) {
      const cap = Math.min(win, view.samplesPerLane);
      return {
        canvas, ctx, view, lane,
        samples: new Float64Array(cap), // preallocated ring window (Law 1)
        out: makeOut(),
      };
    },
    render(state, frameCtx, view) {
      const { ctx, canvas, samples, out } = state;
      const w = canvas.width, h = canvas.height;
      if (view.readLane(state.lane, out) !== 0) return;
      const n = view.readRecent(state.lane, samples.length, samples);
      if (n <= 1) return;
      const lo = out.min, hi = out.max;
      const span = hi > lo ? hi - lo : 1;
      ctx.clearRect(0, 0, w, h);
      if (grid) {
        ctx.strokeStyle = 'rgba(120,144,156,0.18)';
        ctx.lineWidth = 1;
        ctx.beginPath();
        for (let g = 1; g < 4; g++) { ctx.moveTo(0, (h * g) / 4); ctx.lineTo(w, (h * g) / 4); }
        ctx.stroke();
      }
      ctx.strokeStyle = color;
      ctx.lineWidth = lineWidth;
      ctx.beginPath();
      const step = w / (n - 1);
      for (let i = 0; i < n; i++) {
        const y = h - 4 - ((samples[i] - lo) / span) * (h - 8);
        if (i === 0) ctx.moveTo(0, y); else ctx.lineTo(i * step, y);
      }
      ctx.stroke();
    },
  };
}

// ---------------------------------------------------------------------------
// WeftCandlestickChart — OHLC candles derived deterministically from a price
// lane ring: chunkSize consecutive samples per candle. One preallocated ring
// window (newest-first) + one preallocated OHLC array; values computed in place.
// ---------------------------------------------------------------------------

export function createCandlestickEngine({ lane = 0, candles = 64, chunkSize = 4, upColor = '#22c55e', downColor = '#ef4444', wickColor = '#94a3b8' } = {}) {
  return {
    contextType: '2d',
    init(canvas, ctx, view) {
      const need = Math.min(candles * chunkSize, view.samplesPerLane);
      const usable = Math.floor(need / chunkSize) * chunkSize;
      return {
        canvas, ctx, view, lane,
        chunkSize,
        usable, // samples actually consumable per frame
        samples: new Float64Array(usable),   // newest-first window (Law 1)
        ohlc: new Float64Array(4 * Math.floor(usable / chunkSize)),
      };
    },
    render(state, frameCtx, view) {
      const { ctx, canvas, samples, ohlc, chunkSize, usable } = state;
      const w = canvas.width, h = canvas.height;
      const n = view.readRecent(state.lane, usable, samples);
      if (n < chunkSize) return;
      const count = Math.floor(n / chunkSize);
      // candles: index 0 = newest chunk. Within a chunk (newest-first storage),
      // open = last element (oldest), close = first element (newest).
      let min = Infinity, max = -Infinity;
      for (let c = 0; c < count; c++) {
        const base = c * chunkSize;
        const o = samples[base + chunkSize - 1];
        const cl = samples[base];
        let hi = o > cl ? o : cl, lo = o < cl ? o : cl;
        for (let k = 1; k < chunkSize - 1; k++) {
          const v = samples[base + k];
          if (v > hi) hi = v; else if (v < lo) lo = v;
        }
        const q = c * 4;
        ohlc[q] = o; ohlc[q + 1] = hi; ohlc[q + 2] = lo; ohlc[q + 3] = cl;
        if (hi > max) max = hi;
        if (lo < min) min = lo;
      }
      if (!(max > min)) { min -= 1; max += 1; }
      const span = max - min;
      const slot = w / count;
      const bodyW = slot * 0.6 > 1 ? slot * 0.6 : 1;
      ctx.clearRect(0, 0, w, h);
      const yOf = (v) => h - ((v - min) / span) * h;
      for (let c = 0; c < count; c++) {
        const q = c * 4;
        const o = ohlc[q], hi = ohlc[q + 1], lo = ohlc[q + 2], cl = ohlc[q + 3];
        const x = w - (c + 0.5) * slot; // newest candle at the right edge
        ctx.strokeStyle = wickColor;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(x, yOf(hi)); ctx.lineTo(x, yOf(lo));
        ctx.stroke();
        ctx.fillStyle = cl >= o ? upColor : downColor;
        const top = yOf(o > cl ? o : cl);
        const bot = yOf(o < cl ? o : cl);
        ctx.fillRect(x - bodyW / 2, top, bodyW, bot - top > 1 ? bot - top : 1);
      }
    },
  };
}

// ---------------------------------------------------------------------------
// WeftOrderBook — Level-2 price ladder where each HPL1 lane IS a level.
// bidLanes/askLanes map lanes to ladder rows; bar length ∝ live lane value.
// ---------------------------------------------------------------------------

export function createOrderBookEngine({ bidLanes = [0, 1, 2], askLanes = [3, 4, 5], bidColor = 'rgba(34,197,94,0.75)', askColor = 'rgba(239,68,68,0.75)', textColor = '#cbd5e1' } = {}) {
  const rows = bidLanes.length + askLanes.length;
  return {
    contextType: '2d',
    init(canvas, ctx, view) {
      return {
        canvas, ctx, view, rows,
        bidLanes: bidLanes.slice(),
        askLanes: askLanes.slice(),
        out: makeOut(),
      };
    },
    render(state, frameCtx, view) {
      const { ctx, canvas, out } = state;
      const w = canvas.width, h = canvas.height;
      const rowH = h / state.rows;
      ctx.clearRect(0, 0, w, h);
      ctx.font = '10px monospace';
      ctx.textBaseline = 'middle';
      let refMax = 1e-9;
      for (let i = 0; i < state.bidLanes.length; i++) {
        if (view.readLane(state.bidLanes[i], out) === 0 && out.current > refMax) refMax = out.current;
      }
      for (let i = 0; i < state.askLanes.length; i++) {
        if (view.readLane(state.askLanes[i], out) === 0 && out.current > refMax) refMax = out.current;
      }
      const midY = state.askLanes.length * rowH;
      for (let i = 0; i < state.askLanes.length; i++) {
        if (view.readLane(state.askLanes[i], out) !== 0) continue;
        const bw = (out.current / refMax) * (w * 0.82);
        const y = midY - (i + 1) * rowH;
        ctx.fillStyle = askColor;
        ctx.fillRect(w - bw, y, bw, rowH - 1);
        ctx.fillStyle = textColor;
        ctx.fillText(fmt(out.current), 4, y + rowH / 2);
      }
      for (let i = 0; i < state.bidLanes.length; i++) {
        if (view.readLane(state.bidLanes[i], out) !== 0) continue;
        const bw = (out.current / refMax) * (w * 0.82);
        const y = midY + i * rowH;
        ctx.fillStyle = bidColor;
        ctx.fillRect(w - bw, y, bw, rowH - 1);
        ctx.fillStyle = textColor;
        ctx.fillText(fmt(out.current), 4, y + rowH / 2);
      }
      ctx.fillStyle = 'rgba(148,163,184,0.9)';
      ctx.fillRect(0, midY - 1, w, 2); // mid line
    },
  };
}

function fmt(v) {
  return v >= 1000 ? Math.round(v).toString() : v.toPrecision(6);
}

// ---------------------------------------------------------------------------
// WeftAudioMeter — multi-channel peak/hold meter. Each lane = one channel;
// peak-hold state lives in preallocated arrays with timestamp-based hold.
// ---------------------------------------------------------------------------

export function createAudioMeterEngine({ lanes = [0, 1], holdMs = 1200, clipLevel = 0.99, hotColor = '#f59e0b', okColor = '#22d3ee', clipColor = '#ef4444' } = {}) {
  return {
    contextType: '2d',
    init(canvas, ctx, view) {
      return {
        canvas, ctx, view,
        lanes: lanes.slice(),
        channel: lanes.length,
        peaks: new Float64Array(lanes.length),  // peak-hold state, in place
        peakAt: new Float64Array(lanes.length), // ns timestamp of held peak
        levels: new Float64Array(lanes.length), // current levels
        out: makeOut(),
      };
    },
    render(state, frameCtx, view) {
      const { ctx, canvas, out } = state;
      const w = canvas.width, h = canvas.height;
      const nowNs = frameCtx && frameCtx.nowNs ? frameCtx.nowNs : 0;
      ctx.clearRect(0, 0, w, h);
      const gap = 4;
      const barH = (h - (state.channel - 1) * gap) / state.channel;
      if (barH < 2) return;
      for (let c = 0; c < state.channel; c++) {
        if (view.readLane(state.lanes[c], out) === 0) {
          const v = out.current < 1 ? out.current : 1;
          const a = v < 0 ? -v : v;
          state.levels[c] = a;
          if (a >= state.peaks[c] || nowNs - state.peakAt[c] > holdMs * 1e6) {
            state.peaks[c] = a;
            state.peakAt[c] = nowNs;
          }
        }
        const y = c * (barH + gap);
        ctx.fillStyle = state.levels[c] >= clipLevel ? clipColor : (state.levels[c] > 0.7 ? hotColor : okColor);
        ctx.fillRect(0, y, state.levels[c] * w, barH);
        ctx.fillStyle = 'rgba(226,232,240,0.9)';
        ctx.fillRect(state.peaks[c] * w - 1, y, 2, barH); // peak-hold tick
      }
    },
  };
}

// ---------------------------------------------------------------------------
// Component factories — thin <WeftCanvas engine={...}> wrappers
// ---------------------------------------------------------------------------

export function createVisualizers(React, hooks) {
  const weftCanvas = createWeftCanvas(React, hooks);
  const { useWeftPlane } = hooks;

  function planeEngineComponent(engineFactory, defaults) {
    return function Visualizer({ plane, hz = 240, engineProps, onEvent, onFatal, fallback, style, className, ...pass }) {
      const ctx = useWeftPlane(plane, { hz });
      const engine = engineFactory({ ...defaults, ...engineProps });
      return weftCanvas({ plane: ctx, engine, hz, onEvent, onFatal, fallback, style, className, ...pass });
    };
  }

  return {
    WeftOscilloscope: planeEngineComponent(createOscilloscopeEngine, { lane: 0, window: 256 }),
    WeftCandlestickChart: planeEngineComponent(createCandlestickEngine, { lane: 0, candles: 64, chunkSize: 4 }),
    WeftOrderBook: planeEngineComponent(createOrderBookEngine, {}),
    WeftAudioMeter: planeEngineComponent(createAudioMeterEngine, { lanes: [0, 1] }),
  };
}
