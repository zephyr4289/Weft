// core.js — @weft/react-hooks: zero-GC React integration for Weft frames.
//
// The zero-rerender contract (Pillar 1 §2.E):
//   Hooks subscribe to high-frequency frame sources and dispatch frames to
//   YOUR code (a canvas painter, a DOM writer, a renderer) via stable refs.
//   React state is NEVER set — no virtual-DOM reconciliation, no component
//   re-renders, no GC pressure from the render pipeline. A component that
//   receives 100,000 frames per second still renders exactly once.
//
// Frame protocol: sources emit (buffer, byteOffset, availBytes) triples.
// Consumers rebind their own weftc-generated flyweight views in place —
// the same instance, zero allocations per frame (Law 1).
//
// This module carries NO react dependency: hook implementations are created
// via createHooks({ useRef, useEffect }). src/index.js wires them to the
// real React; tests inject a compliant shim.

/**
 * Wire the hook implementations to a React-compatible hooks surface.
 * Uses only useRef + useEffect (react >= 18; StrictMode-safe).
 */
export function createHooks({ useRef, useEffect }) {
  /**
   * Subscribe to a frame source without ever triggering a React re-render.
   *
   * `onFrame` is held in a latest-ref: you may pass a new closure every
   * render without resubscribing. The subscription lifecycle follows the
   * component (StrictMode double-mount is safe: unsubscribes+resubscribes).
   */
  function useWeftBuffer(source, onFrame) {
    const onFrameRef = useRef(onFrame);
    // Latest-ref pattern: assignment during render is intentional — the
    // subscription closure below is created once per source.
    onFrameRef.current = onFrame;
    useEffect(() => {
      return source.subscribe((buffer, byteOffset, availBytes) => {
        onFrameRef.current(buffer, byteOffset, availBytes);
      });
    }, [source]);
  }

  /**
   * Paint frames into a canvas at display cadence without re-rendering
   * React. Frames mark the canvas dirty and coalesce into the next
   * animation tick; the newest frame wins. The rAF loop parks itself when
   * idle or when the document is hidden, and resumes on visibilitychange.
   *
   * Returns a ref to spread onto a <canvas> element.
   */
  function useWeftCanvas(source, draw) {
    const canvasRef = useRef(null);
    const drawRef = useRef(draw);
    drawRef.current = draw;

    useEffect(() => {
      const s = {
        dirty: false,
        buffer: null,
        byteOffset: 0,
        avail: 0,
        raf: 0,
        running: false,
        ctx: null,
      };

      const pump = () => {
        s.raf = 0;
        s.running = false;
        if (typeof document !== 'undefined' && document.hidden) {
          return; // park; visibilitychange resumes if dirty
        }
        if (!s.dirty) return;
        s.dirty = false;
        const canvas = canvasRef.current;
        if (canvas) {
          if (s.ctx === null || s.ctx.canvas !== canvas) {
            s.ctx = canvas.getContext('2d');
          }
          drawRef.current(s.ctx, s.buffer, s.byteOffset, s.avail);
        }
        s.running = true;
        s.raf = requestAnimationFrame(pump);
      };

      const ensureLoop = () => {
        if (!s.running) {
          s.running = true;
          s.raf = requestAnimationFrame(pump);
        }
      };

      const unsubscribe = source.subscribe((buffer, byteOffset, availBytes) => {
        s.buffer = buffer;
        s.byteOffset = byteOffset;
        s.avail = availBytes;
        s.dirty = true;
        ensureLoop();
      });

      const onVisibility = () => {
        if (typeof document !== 'undefined' && !document.hidden && s.dirty) {
          ensureLoop();
        }
      };
      if (typeof document !== 'undefined') {
        document.addEventListener('visibilitychange', onVisibility);
      }

      return () => {
        unsubscribe();
        if (s.raf) cancelAnimationFrame(s.raf);
        s.raf = 0;
        s.running = false;
        s.ctx = null;
        if (typeof document !== 'undefined') {
          document.removeEventListener('visibilitychange', onVisibility);
        }
      };
    }, [source]);

    return canvasRef;
  }

  return { useWeftBuffer, useWeftCanvas };
}

/**
 * Create a frame source: a tiny pub/sub over (buffer, byteOffset,
 * availBytes) triples with reference-stable subscribe/emit identities.
 *
 * Listeners are iterated live (spec-safe against unsubscribe-during-emit);
 * do not add listeners from inside emit on the same tick you rely on them.
 */
export function createFrameSource() {
  const listeners = new Set();
  return {
    /** Subscribe; returns an unsubscribe function (idempotent). */
    subscribe(fn) {
      listeners.add(fn);
      let active = true;
      return () => {
        if (!active) return;
        active = false;
        listeners.delete(fn);
      };
    },
    /** Hot path: dispatch a frame window to all listeners. */
    emit(buffer, byteOffset, availBytes) {
      for (const fn of listeners) fn(buffer, byteOffset, availBytes);
    },
    get listenerCount() {
      return listeners.size;
    },
  };
}

/** Attach a WebSocket as a frame producer (binary messages only). */
export function attachWebSocket(source, ws) {
  const onMessage = (e) => {
    const data = e.data;
    if (typeof data === 'string') return; // control frames are ignored
    source.emit(data, 0, data.byteLength);
  };
  ws.addEventListener('message', onMessage);
  return () => ws.removeEventListener('message', onMessage);
}

/**
 * Fixed-capacity ring of Float32 samples for oscilloscope-style rendering.
 * Preallocated once; push() overwrites the oldest slot (zero allocation in
 * steady state, Law 1). Pairs naturally with useWeftCanvas + drawFrameGraph.
 */
export function createSampleRing(capacity) {
  const data = new Float32Array(capacity);
  let head = 0;
  let filled = 0;
  return {
    push(v) {
      data[head] = v;
      head = (head + 1) % capacity;
      if (filled < capacity) filled += 1;
    },
    /** Iterates oldest -> newest WITHOUT allocating (callback style). */
    forEach(fn) {
      const start = filled < capacity ? 0 : head;
      for (let i = 0; i < filled; i++) fn(data[(start + i) % capacity], i);
    },
    get length() {
      return filled;
    },
    get capacity() {
      return capacity;
    },
  };
}

/**
 * Canvas graph painter: renders one Float32 channel as a polyline with
 * auto-scaling. Reads only primitives from the ring; allocates nothing.
 */
export function drawFrameGraph(ctx, ring, width, height) {
  ctx.clearRect(0, 0, width, height);
  ctx.beginPath();
  let min = Infinity;
  let max = -Infinity;
  ring.forEach((v) => {
    if (v < min) min = v;
    if (v > max) max = v;
  });
  if (!Number.isFinite(min) || min === max) {
    min = 0;
    max = 1;
  }
  const span = max - min;
  const n = ring.length;
  if (n < 2) return;
  let i = 0;
  ring.forEach((v) => {
    const x = (i * width) / (n - 1);
    const y = height - ((v - min) / span) * (height - 2) - 1;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
    i += 1;
  });
  ctx.stroke();
}
