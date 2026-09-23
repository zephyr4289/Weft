// src/core.js — framework-agnostic React bindings factory for the HPL1 Hot-Plane.
//
// Everything here takes `React` as a PARAMETER (createHeddleHooks(React)) so the
// whole binding layer is testable with a minimal mount-semantics shim without a
// DOM or a React install (packages/react-heddle/src/index.js wires the real React).
//
// THE ZERO-RE-RENDER CONTRACT (charter Law 1 + Pillar 4 mandate):
//   * Telemetry NEVER flows through React state. Hooks return STABLE objects and
//     ref-binders; per-frame updates mutate DOM nodes / fields in place.
//   * The ONLY setState in any component is on Law-4 fatal boundaries (context
//     loss, worker crash, bad engine) — error paths, never the stream.
//   * Per-frame closures are allocated at MOUNT time (cold) and reused for the
//     lifetime of the binding; the frame loop itself allocates nothing.
//
// Mount-time allocations are honest and bounded: one PlaneContext per buffer,
// one subscriber closure per binding, one scheduler per plane.

import {
  HotPlaneView, HotPlaneProducer, FrameScheduler,
  makeLaneOut, makeHeaderOut, HPL1, Hpl1Error, LANE_FLAG_ACTIVE,
} from '../../heddle-core/src/index.js';

// ---------------------------------------------------------------------------
// PlaneContext — shared per-buffer runtime: one view, one scheduler, N bindings
// ---------------------------------------------------------------------------

export class PlaneContext {
  constructor(buffer, { hz = 240, byteOffset = 0, raf, producer = null } = {}) {
    this.buffer = buffer;
    this.view = new HotPlaneView(buffer, { byteOffset });
    this.producer = producer instanceof HotPlaneProducer ? producer : null;
    this.scheduler = new FrameScheduler({ hz, raf: raf === null ? null : raf });
    // raf === null (explicit) ⇒ MANUAL plane: acquire() arms the scheduler,
    // tests/headless hosts drive frames with scheduler.pump(). undefined ⇒
    // default rAF loop in acquire().
    this.manual = raf === null;
    this.subs = [];
    this.refcount = 0;
    this.laneOut = makeLaneOut();      // shared scratch — subscribers copy out
    this.headerOut = makeHeaderOut();
    this.changed = new Uint32Array(64);
    this.frameCtx = null;              // live during tick, for late readers
    this.eventListeners = [];
    this.lastEpochLo = -1; this.lastEpochHi = -1;
    const self = this;
    this._tick = function tick(frameCtx) {
      self.frameCtx = frameCtx;
      // header + epoch watch (Law 4: restarts are explicit events)
      const code = self.view.readHeader(self.headerOut);
      if (code === HPL1.EPOCH_CHANGED) self.emit(HPL1.EPOCH_CHANGED, 'producer restarted');
      else if (code === HPL1.TORN_SEQLOCK) self.emit(HPL1.TORN_SEQLOCK, 'header');
      self.view.scanDirty(self.changed);
      const subs = self.subs;
      for (let i = 0; i < subs.length; i++) subs[i](self.view, frameCtx);
      self.frameCtx = null;
    };
  }

  acquire() {
    if (++this.refcount === 1) {
      if (this.manual) this.scheduler.arm(this._tick);
      else this.scheduler.start(this._tick);
    }
    return this;
  }

  release() {
    if (--this.refcount <= 0) {
      this.refcount = 0;
      this.scheduler.stop();
    }
    return this;
  }

  addSubscriber(fn) {
    this.subs.push(fn);
    const self = this;
    return function unsubscribe() {
      const i = self.subs.indexOf(fn);
      if (i >= 0) self.subs.splice(i, 1);
    };
  }

  onEvent(fn) {
    this.eventListeners.push(fn);
    const self = this;
    return function off() {
      const i = self.eventListeners.indexOf(fn);
      if (i >= 0) self.eventListeners.splice(i, 1);
    };
  }

  emit(code, detail) {
    const ls = this.eventListeners;
    for (let i = 0; i < ls.length; i++) ls[i](code, detail);
  }

  // Publish one sample through the attached producer (MANUAL/user-driven lanes).
  publish(lane, value, nowNs) {
    if (this.producer === null) {
      throw new Hpl1Error(HPL1.PLANE_DETACHED, 'no producer attached — read-only plane');
    }
    return this.producer.publishLane(lane, value, nowNs);
  }
}

const CONTEXTS = new WeakMap(); // buffer -> PlaneContext (dedupe across hooks)

export function getPlaneContext(buffer, opts) {
  let ctx = CONTEXTS.get(buffer);
  if (ctx === undefined) {
    ctx = new PlaneContext(buffer, opts);
    CONTEXTS.set(buffer, ctx);
  }
  return ctx;
}

// ---------------------------------------------------------------------------
// Hooks factory
// ---------------------------------------------------------------------------

export function createHeddleHooks(React) {
  const PlaneCtxContext = React.createContext(null);

  function usePlaneContext(planeProp) {
    const fromContext = React.useContext(PlaneCtxContext);
    return planeProp !== undefined ? planeProp : fromContext;
  }

  // Attaches (or reuses) the PlaneContext for a buffer and manages refcount
  // lifecycle for this component. Returns the STABLE context object.
  function useWeftPlane(plane, { hz = 240, producer = null } = {}) {
    if (plane === null || plane === undefined) {
      throw new Hpl1Error(
        HPL1.PLANE_DETACHED,
        'no plane provided — pass a buffer/PlaneContext prop or a <WeftPlaneProvider value>',
      );
    }
    const stable = React.useRef(null);
    if (stable.current === null) {
      stable.current = plane instanceof PlaneContext
        ? plane
        : getPlaneContext(plane, { hz, producer });
    }
    const ctx = stable.current;
    React.useEffect(() => {
      ctx.acquire();
      return () => ctx.release();
    }, [ctx]);
    return ctx;
  }

  // `useWeftSignal(laneIndex)` — micro-DOM ref mutator. Binds a DOM node and
  // writes the lane's live value straight into it (node.nodeValue), bypassing
  // useState/reconciliation entirely. Returns a STABLE ref callback.
  // opts.textDivider: update text every Nth frame (default: ~60 Hz regardless
  // of render hz — text nodes are the one path where the engine materializes a
  // string; divider caps that cost. Canvas lanes should use the canvas path).
  // opts.mutate(node, out, laneOut): custom zero-alloc mutator hook-up.
  function useWeftSignal(laneIndex, opts = {}) {
    const plane = usePlaneContext(opts.plane);
    const stable = React.useRef(null);
    if (stable.current === null) {
      const laneOut = makeLaneOut(); // private per-binding scratch (mount-time)
      let frameCounter = 0;
      const explicit = opts.textDivider !== undefined;
      stable.current = {
        laneOut,
        node: null,
        unsubscribe: null,
        mutate: opts.mutate || null,
        divider: explicit ? opts.textDivider : -1, // -1 = resolve at first tick
        auto: !explicit,
        tick(view) {
          if (view.readLane(laneIndex, laneOut) !== HPL1.OK) return;
          const node = stable.current.node;
          if (node === null) return;
          if (stable.current.mutate !== null) {
            stable.current.mutate(node, laneOut, view);
            return;
          }
          let div = stable.current.divider;
          if (div < 0) {
            // auto-resolve: aim ≈60 Hz text updates from the plane's cadence
            div = Math.max(1, Math.round((plane ? plane.scheduler.hz : 240) / 60));
            stable.current.divider = div;
          }
          if ((frameCounter++ % div) !== 0) return;
          node.nodeValue = laneOut.current;
        },
        ref(node) { stable.current.node = node; },
      };
    }
    const ctx = useWeftPlane(plane, {});
    React.useEffect(() => {
      const s = stable.current;
      if (s.auto) s.divider = -1; // re-resolve ONLY auto dividers (explicit wins)
      const un = ctx.addSubscriber((view) => s.tick(view));
      return () => { un(); s.node = null; };
    }, [ctx]);
    return stable.current.ref;
  }

  // `useWeftStats(laneIndex)` — stable, IN-PLACE-mutated live stats object.
  // Never triggers a render: components read stats.current during their own
  // render (rare) or bind the DOM through useWeftSignal; engines read it in
  // the frame loop. Fields: current/min/max/avg/samples/seq/publishNs/flags.
  function useWeftStats(laneIndex, opts = {}) {
    const plane = usePlaneContext(opts.plane);
    const stable = React.useRef(null);
    if (stable.current === null) {
      stable.current = {
        current: 0, min: 0, max: 0, avg: 0,
        samples: 0, seq: 0, publishNs: 0, flags: 0, drops: 0,
        lastCode: HPL1.OK,
      };
    }
    const ctx = useWeftPlane(opts.plane !== undefined ? opts.plane : plane, {});
    React.useEffect(() => {
      const s = stable.current;
      const laneOut = makeLaneOut(); // per-binding scratch (mount-time)
      return ctx.addSubscriber((view) => {
        s.lastCode = view.readLane(laneIndex, laneOut);
        if (s.lastCode !== HPL1.OK) return;
        s.current = laneOut.current;
        s.min = laneOut.min;
        s.max = laneOut.max;
        s.avg = laneOut.avg;
        s.samples = laneOut.samplesSeenLo + laneOut.samplesSeenHi * 0x100000000;
        s.seq = laneOut.seqLo + laneOut.seqHi * 0x100000000;
        s.publishNs = laneOut.publishNsLo + laneOut.publishNsHi * 0x100000000;
        s.flags = laneOut.flags;
        s.drops = laneOut.drops;
      });
    }, [ctx]);
    return stable.current;
  }

  // `useWeftBuffer(laneIndex)` — pre-allocated TypedArray scratch for user
  // interaction handlers (Law 1: handlers never allocate). Returns a STABLE
  // { buffer, publish } pair: write into buffer, then publish() pushes
  // buffer[0] through the plane's producer (MANUAL lane pattern).
  function useWeftBuffer(laneIndex, opts = {}) {
    const plane = usePlaneContext(opts.plane);
    const ctxRef = React.useRef(null);
    ctxRef.current = plane; // latest-ref pattern: publish() always sees live ctx
    const stable = React.useRef(null);
    if (stable.current === null) {
      stable.current = {
        buffer: new Float64Array(16),
        publish(nowNs) {
          ctxRef.current.publish(laneIndex, stable.current.buffer[0], nowNs);
        },
      };
    }
    return stable.current;
  }

  function WeftPlaneProvider({ value, children }) {
    return React.createElement(PlaneCtxContext.Provider, { value }, children);
  }

  return { useWeftPlane, useWeftSignal, useWeftStats, useWeftBuffer, WeftPlaneProvider, PlaneCtxContext };
}

// ---------------------------------------------------------------------------
// <WeftCanvas /> factory — direct-render engine mount
// ---------------------------------------------------------------------------

// Render-engine contract (what Engineer 2's engines plug into):
//   {
//     contextType: 'webgl2' | 'webgpu' | '2d',
//     init(canvas, ctx, view)   -> state          (allocate everything here)
//     render(state, frameCtx, view)               (ZERO allocation per frame)
//     dispose?(state)
//   }
export function assertRenderEngine(engine) {
  if (!engine || typeof engine.render !== 'function' || typeof engine.init !== 'function') {
    throw new Hpl1Error(HPL1.BAD_RENDER_ENGINE, 'engine must implement init(canvas, ctx, view) and render(state, frameCtx, view)');
  }
  if (engine.contextType !== 'webgl2' && engine.contextType !== 'webgpu' && engine.contextType !== '2d') {
    throw new Hpl1Error(HPL1.BAD_RENDER_ENGINE, `unsupported contextType ${engine.contextType}`);
  }
  return engine;
}

export function createWeftCanvas(React, hooks = createHeddleHooks(React)) {
  const { useWeftPlane } = hooks;

  function WeftCanvas({
    plane, engine, hz = 240, onEvent, onFatal,
    fallback = 'WEFT CANVAS OFFLINE', style, className, ...pass
  }) {
    const canvasRef = React.useRef(null);
    const stateRef = React.useRef(null);
    // fatal is the ONLY React state — set exclusively on Law-4 boundaries,
    // never on the telemetry stream (0 re-renders while streaming).
    const [fatal, setFatal] = React.useState(null);

    React.useEffect(() => {
      const canvas = canvasRef.current;
      if (canvas === null || engine === undefined) return undefined;
      let disposed = false;
      let unsub = null;
      let offEvents = null;
      let offVis = null;
      let offLost = null;
      const die = (code, detail) => {
        if (disposed) return;
        if (onEvent) onEvent(code, detail);
        if (onFatal) onFatal(code, detail);
        setFatal({ code, detail }); // exceptional re-render (Law 4 fallback view)
      };
      try {
        assertRenderEngine(engine);
        const ctx = useWeftPlane(plane, { hz });
        const view = ctx.view;
        const gpuCtx = canvas.getContext(engine.contextType);
        if (gpuCtx === null) {
          die(HPL1.CONTEXT_LOST, `getContext('${engine.contextType}') returned null`);
          return undefined;
        }
        stateRef.current = engine.init(canvas, gpuCtx, view);
        unsub = ctx.addSubscriber((v, frameCtx) => {
          engine.render(stateRef.current, frameCtx, v);
        });
        offEvents = ctx.onEvent((code, detail) => { if (onEvent) onEvent(code, detail); });
        // Law 4: Page Visibility — pause when hidden, resume (no catch-up)
        if (typeof document !== 'undefined' && document.addEventListener) {
          const vis = () => {
            if (document.hidden) { ctx.scheduler.pause(); if (onEvent) onEvent(HPL1.TAB_HIDDEN, 'hidden'); }
            else { ctx.scheduler.resume(); if (onEvent) onEvent(HPL1.OK, 'visible'); }
          };
          document.addEventListener('visibilitychange', vis);
          offVis = () => document.removeEventListener('visibilitychange', vis);
        }
        // Law 4: GPU context loss — explicit fallback view
        if (typeof canvas.addEventListener === 'function') {
          const lost = (e) => { e.preventDefault(); die(HPL1.CONTEXT_LOST, 'webglcontextlost'); };
          canvas.addEventListener('webglcontextlost', lost);
          offLost = () => canvas.removeEventListener('webglcontextlost', lost);
        }
      } catch (e) {
        die(e instanceof Hpl1Error ? e.code : HPL1.BAD_RENDER_ENGINE, e.message);
      }
      return () => {
        disposed = true;
        if (unsub) unsub();
        if (offEvents) offEvents();
        if (offVis) offVis();
        if (offLost) offLost();
        if (stateRef.current && typeof engine.dispose === 'function') engine.dispose(stateRef.current);
        stateRef.current = null;
      };
    }, [plane, engine, hz]);

    if (fatal !== null) {
      return React.createElement('div', {
        'data-weft-fatal': fatal.code, style, className,
      }, fallback);
    }
    return React.createElement('canvas', { ref: canvasRef, style, className, ...pass });
  }

  return WeftCanvas;
}
