// helpers/react-shim.mjs — a minimal React hooks runtime for tests.
//
// Implements exactly the semantics the weft hooks rely on:
//   - useRef: persistent slots across renders (slot order is stable because
//     the hooks call useRef in fixed order)
//   - useEffect: DEP-AWARE (skips re-run when deps are Object.is-equal,
//     runs the previous cleanup before re-running on dep change, flushes
//     after render) — matching real React semantics
//   - StrictMode simulation: mount, unmount, mount
//   - requestAnimationFrame/cancelAnimationFrame: manual stepping
//   - document: hidden flag + visibilitychange listeners

export function createShim() {
  const refSlots = [];
  const effectSlots = []; // i -> { deps, cleanup } | undefined
  const pendingEffects = [];
  let refIdx = 0;
  let effectIdx = 0;
  let renders = 0;

  function depsEqual(a, b) {
    if (a === b) return true;
    if (!a || !b || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
      if (!Object.is(a[i], b[i])) return false;
    }
    return true;
  }

  const raf = {
    queue: [],
    nextId: 0,
    cancelCount: 0,
    /** Runs all queued callbacks; returns how many ran. */
    step() {
      const cbs = this.queue;
      this.queue = [];
      for (const cb of cbs) cb(16.6);
      return cbs.length;
    },
  };

  const document = {
    hidden: false,
    listeners: new Map(),
    addEventListener(type, fn) {
      if (!this.listeners.has(type)) this.listeners.set(type, new Set());
      this.listeners.get(type).add(fn);
    },
    removeEventListener(type, fn) {
      const set = this.listeners.get(type);
      if (set) set.delete(fn);
    },
    dispatch(type) {
      for (const fn of [...(this.listeners.get(type) ?? [])]) fn();
    },
  };

  const React = {
    useRef(init) {
      const i = refIdx++;
      if (!(i in refSlots)) refSlots[i] = { current: init };
      return refSlots[i];
    },
    useEffect(fn, deps) {
      const i = effectIdx++;
      pendingEffects.push({ i, fn, deps });
    },
  };

  return {
    React,
    document,
    raf,
    get renderCount() {
      return renders;
    },
    /** Run the component once; flush queued dep-aware effects afterwards. */
    render(Component) {
      refIdx = 0;
      effectIdx = 0;
      renders += 1;
      const ret = Component();
      for (const { i, fn, deps } of pendingEffects.splice(0)) {
        const prev = effectSlots[i];
        if (prev && depsEqual(prev.deps, deps)) continue; // deps unchanged: skip
        if (prev && typeof prev.cleanup === 'function') prev.cleanup(); // cleanup before re-run
        const cleanup = fn();
        effectSlots[i] = { deps, cleanup: typeof cleanup === 'function' ? cleanup : null };
      }
      return ret;
    },
    /** StrictMode double-mount simulation: mount, unmount, mount. */
    strictRender(Component) {
      this.render(Component);
      this.unmount();
      this.render(Component);
    },
    unmount() {
      for (const slot of effectSlots) {
        if (slot && typeof slot.cleanup === 'function') slot.cleanup();
      }
      effectSlots.length = 0;
    },
    requestAnimationFrame(cb) {
      raf.nextId += 1;
      const id = raf.nextId;
      cb.__weftRafId = id;
      raf.queue.push(cb);
      return id;
    },
    cancelAnimationFrame(id) {
      raf.cancelCount += 1;
      raf.queue = raf.queue.filter((cb) => cb.__weftRafId !== id);
    },
  };
}

/** Minimal canvas factory for useWeftCanvas tests. */
export function createCanvas() {
  const ctx = {
    canvas: null,
    cleared: 0,
    ops: [],
    clearRect(x, y, w, h) {
      this.cleared += 1;
      this.ops.push(['clearRect', x, y, w, h]);
    },
    beginPath() {
      this.ops.push(['beginPath']);
    },
    moveTo(x, y) {
      this.ops.push(['moveTo', x, y]);
    },
    lineTo(x, y) {
      this.ops.push(['lineTo', x, y]);
    },
    stroke() {
      this.ops.push(['stroke']);
    },
  };
  const canvas = {
    width: 256,
    height: 64,
    getContext() {
      return ctx;
    },
  };
  ctx.canvas = canvas;
  return { canvas, ctx };
}
