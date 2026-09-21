// test/shim.mjs — minimal React shim with real useRef/useEffect mount
// semantics (proven pattern from @weft/react-tensor + @weft/react-heddle).
// Tracks useState calls so tests can PROVE telemetry never re-renders.

export function makeShim() {
  const effects = [];
  let stateSeq = 0;
  const React = {
    useRef(init) { return { current: init }; },
    useEffect(fn, deps) { effects.push({ fn, deps }); },
    useState(init) {
      const id = stateSeq++;
      return [typeof init === 'function' ? init() : init, (v) => {
        shim.setStateCalls.push(id);
        return v;
      }];
    },
    createElement(type, props, ...children) {
      return { type, props: props || {}, children };
    },
  };
  const shim = {
    React,
    effects,
    setStateCalls: [],
    mount() { for (const e of effects) e.cleanup = e.fn(); },
    unmount() { for (const e of effects) if (typeof e.cleanup === 'function') e.cleanup(); effects.length = 0; },
  };
  return shim;
}

// test/dom.mjs — tiny deterministic DOM double for HUD construction/paint.
// Nodes support className/textContent/style/appendChild/children; the "host"
// is a plain node. No timers involved — tests call paintSpectrumHud directly.
export function makeNode(tag) {
  return {
    tagName: tag,
    className: '',
    textContent: '',
    style: {},
    children: [],
    appendChild(child) { this.children.push(child); return child; },
    removeChild(child) {
      const i = this.children.indexOf(child);
      if (i >= 0) this.children.splice(i, 1);
    },
  };
}

export function makeDoc() {
  return { createElement: (tag) => makeNode(tag) };
}

export function makeHost() { return makeNode('div'); }

// Walk helper for assertions
export function findNodes(root, className) {
  const out = [];
  const walk = (n) => {
    if (n.className === className) out.push(n);
    for (const c of n.children) walk(c);
  };
  walk(root);
  return out;
}
