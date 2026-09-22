/**
 * Stage-3 driver — deterministic mini-React shim.
 * Implements the hook subset used by Studio panels with SYNCHRONOUS
 * re-renders and effect execution, so the managed suite can:
 *   - mount the REAL WeftStudio component tree (no DOM),
 *   - execute mount effects (engine creation, drawer registration),
 *   - pump 10,000 stream frames through the engine governor,
 *   - count every component function invocation via the render spy,
 *   - assert ZERO setState-initiated re-renders during streaming.
 * This mirrors the shim-proven methodology accepted in Pillars 4 & 5.
 */

import { bindReact, spy } from '../../../packages/studio/src/engine/react-adapter.ts';

export function createStudioHarness() {
  const state = {
    effects: [],           // pending mount effects: { fn, deps }
    cleanups: [],          // collected cleanup fns
    renderScheduled: false,
    flushCount: 0,
  };

  let currentComponent = null;
  let rerenderRequested = false;

  const React = {
    useState(initial) {
      const cell = currentComponent; // capture: setters run later (effects)
      const hookIndex = cell._hookIndex++;
      if (!(hookIndex in cell._state)) {
        cell._state[hookIndex] = typeof initial === 'function' ? initial() : initial;
      }
      const setter = (v) => {
        const next = typeof v === 'function' ? v(cell._state[hookIndex]) : v;
        if (Object.is(next, cell._state[hookIndex])) return;
        cell._state[hookIndex] = next;
        spy.mutations++;
        rerenderRequested = true;
      };
      return [cell._state[hookIndex], setter];
    },

    useRef(initial) {
      const hookIndex = currentComponent._hookIndex++;
      if (!(hookIndex in currentComponent._refs)) currentComponent._refs[hookIndex] = { current: initial };
      return currentComponent._refs[hookIndex];
    },

    useEffect(fn, deps) {
      state.effects.push({ fn, deps, component: currentComponent });
      return;
    },

    useMemo(fn, deps) {
      const hookIndex = currentComponent._hookIndex++;
      if (!currentComponent._memo) currentComponent._memo = {};
      const prev = currentComponent._memo[hookIndex];
      if (prev && prev.deps && deps && prev.deps.length === deps.length &&
          prev.deps.every((d, i) => Object.is(d, deps[i]))) {
        return prev.value;
      }
      const value = fn();
      currentComponent._memo[hookIndex] = { deps, value };
      return value;
    },

    useCallback(fn, deps) {
      void deps;
      return fn;
    },

    createElement(type, props, ...children) {
      return { $$el: true, type, props: props || {}, children };
    },
  };

  bindReact(React);

  function renderComponent(componentFn, key) {
    const cell = {
      fn: componentFn,
      key,
      _state: {},
      _refs: {},
      _memo: {},
      _hookIndex: 0,
      lastOutput: undefined,
    };
    const invoke = () => {
      currentComponent = cell;
      cell._hookIndex = 0;
      rerenderRequested = false;
      const out = cell.fn();
      cell.lastOutput = out;
      currentComponent = null;
      return out;
    };
    cell.invoke = invoke;
    return cell;
  }

  function rerenderPending() {
    return rerenderRequested;
  }

  function runEffects() {
    const pending = state.effects.splice(0);
    for (const { fn } of pending) {
      const cleanup = fn();
      if (typeof cleanup === 'function') state.cleanups.push(cleanup);
    }
  }

  function unmount() {
    for (const c of state.cleanups.splice(0)) c();
  }

  return { state, renderComponent, runEffects, unmount, React, rerenderPending };
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
