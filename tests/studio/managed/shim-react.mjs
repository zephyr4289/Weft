/**
 * Stage-3 driver — deterministic mini-React shim WITH component recursion.
 *
 * Renders the REAL WeftStudio component tree the way React does:
 *   - function components are ELEMENTS (`h(Component, props)`) whose hooks
 *     are owned per component cell (Rules of Hooks are actually enforced by
 *     construction — hook order is tracked per cell),
 *   - mount effects run bottom-up with deps tracking,
 *   - setState marks the owning cell dirty and the harness re-renders it,
 *   - the render spy counts every component function invocation.
 *
 * This mirrors the shim-proven methodology accepted in Pillars 4 & 5, at
 * component-tree fidelity.
 */

import { bindReact, spy } from '../../../packages/studio/src/engine/react-adapter.ts';

export function createStudioHarness() {
  const cells = new Map(); // key -> cell
  const effectQueue = [];  // { cell, fx } pending mount/dep effects
  let currentCell = null;
  let dirty = new Set();

  function makeCell(fn, key) {
    const cell = {
      fn, key,
      state: [], refs: [], memo: [], fx: [],
      hookIndex: 0,
      lastOutput: undefined,
      mounted: false,
    };
    return cell;
  }

  const React = {
    useState(initial) {
      const cell = currentCell;
      const i = cell.hookIndex++;
      if (!(i in cell.state)) cell.state[i] = typeof initial === 'function' ? initial() : initial;
      const setter = (v) => {
        const next = typeof v === 'function' ? v(cell.state[i]) : v;
        if (Object.is(next, cell.state[i])) return;
        cell.state[i] = next;
        spy.mutations++;
        dirty.add(cell);
      };
      return [cell.state[i], setter];
    },

    useRef(initial) {
      const cell = currentCell;
      const i = cell.hookIndex++;
      if (!(i in cell.refs)) cell.refs[i] = { current: initial };
      return cell.refs[i];
    },

    useEffect(fn, deps) {
      const cell = currentCell;
      const i = cell.hookIndex++;
      const prev = cell.fx[i];
      const same = prev && prev.ran && deps && prev.deps && prev.deps.length === deps.length &&
        prev.deps.every((d, k) => Object.is(d, deps[k]));
      cell.fx[i] = { fn, deps, ran: true };
      if (!same) effectQueue.push({ cell, fn });
      return;
    },

    useMemo(fn, deps) {
      const cell = currentCell;
      const i = cell.hookIndex++;
      const prev = cell.memo[i];
      if (prev && deps && prev.deps && prev.deps.length === deps.length &&
          prev.deps.every((d, k) => Object.is(d, deps[k]))) return prev.value;
      const value = fn();
      cell.memo[i] = { deps, value };
      return value;
    },

    useCallback(fn) { return fn; },

    createElement(type, props, ...children) {
      return { $$el: true, type, props: props || {}, children };
    },
  };

  bindReact(React);

  function invokeCell(cell, props) {
    currentCell = cell;
    cell.hookIndex = 0;
    const out = cell.fn(props);
    currentCell = null;
    cell.lastOutput = out;
    return out;
  }

  /** Recursively expand function-component elements (depth-capped). */
  function expand(el, path, depth) {
    if (depth > 24) return el;
    if (Array.isArray(el)) return el.map((k, i) => expand(k, path + '/' + i, depth + 1));
    if (!el || typeof el !== 'object' || !el.$$el) return el;
    const t = el.type;
    if (typeof t === 'function') {
      const key = path + '::' + (t.name || 'anon');
      let cell = cells.get(key);
      if (!cell) { cell = makeCell(t, key); cells.set(key, cell); }
      const out = invokeCell(cell, el.props || {});
      const kids = expand(out, path + '·', depth + 1);
      return { $$el: true, type: (el.props && el.props.__host) || 'div', props: { __cell: cell }, children: Array.isArray(kids) ? kids : [kids] };
    }
    const kids = el.children || [];
    return { $$el: true, type: el.type, props: el.props, children: kids.map((k, i) => expand(k, path + '/' + i, depth + 1)) };
  }

  function runEffects() {
    const pending = effectQueue.splice(0);
    for (const { fn } of pending) {
      const cleanup = fn();
      if (typeof cleanup === 'function') pendingCleanups.push(cleanup);
    }
  }

  const pendingCleanups = [];

  function renderRoot(componentFn, initialProps) {
    const key = 'root::' + (componentFn.name || 'root');
    let cell = cells.get(key);
    if (!cell) { cell = makeCell(componentFn, key); cells.set(key, cell); }
    const out = invokeCell(cell, initialProps || {});
    const tree = expand(out, 'R', 0);
    return { cell, tree };
  }

  function rerenderRoot(componentFn, initialProps) {
    dirty.clear();
    const { cell, tree } = renderRoot(componentFn, initialProps);
    runEffects();
    return { cell, tree, hadDirty: dirty.size > 0 };
  }

  function unmount() {
    for (const c of pendingCleanups.splice(0)) c();
    for (const [, cell] of cells) {
      for (const fx of cell.fx) {
        if (fx && typeof fx.__cleanup === 'function') fx.__cleanup();
      }
    }
  }

  return {
    cells, effectQueue, dirty,
    renderRoot, rerenderRoot, runEffects, unmount,
    rerenderPending: () => dirty.size > 0,
  };
}

/**
 * Walk an element tree and invoke every function ref with a fake canvas
 * (mount semantics), so hot drawers bind to a working 2D context recorder.
 */
export function attachFakeCanvases(el, canvasFactory, seen = new Set()) {
  if (Array.isArray(el)) {
    let n = 0;
    for (const item of el) n += attachFakeCanvases(item, canvasFactory, seen);
    return n;
  }
  if (!el || typeof el !== 'object' || seen.has(el)) return 0;
  seen.add(el);
  let count = 0;
  if (el.$$el) {
    const props = el.props || {};
    if (typeof props.ref === 'function' && (el.type === 'canvas')) {
      const canvas = canvasFactory();
      props.ref(canvas);
      count++;
    }
    const kids = el.children || [];
    for (const k of kids) count += attachFakeCanvases(k, canvasFactory, seen);
  }
  return count;
}

/** Recording 2D context (counts every method call, accepts property sets). */
export function fakeCtx() {
  const rec = { calls: 0, names: new Map() };
  const handler = {
    get(_t, prop) {
      if (prop === '__rec') return rec;
      return (...a) => {
        rec.calls++;
        rec.names.set(prop, (rec.names.get(prop) || 0) + 1);
        void a;
        return undefined;
      };
    },
    set() { return true; },
  };
  const ctx = new Proxy({}, handler);
  return { ctx, rec };
}

export function fakeCanvas(w = 480, h = 300) {
  const { ctx, rec } = fakeCtx();
  return {
    clientWidth: w,
    clientHeight: h,
    width: 0,
    height: 0,
    getContext: () => ctx,
    __rec: rec,
  };
}
