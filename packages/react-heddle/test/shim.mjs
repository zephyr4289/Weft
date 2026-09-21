// test/shim.mjs — minimal React shim with real useRef/useEffect mount
// semantics (proven pattern from @weft/react-tensor). Tracks useState calls
// so tests can PROVE that telemetry never triggers a re-render.
export function makeShim() {
  const effects = [];
  let stateSeq = 0;
  const React = {
    useRef(init) {
      return { current: init }; // fresh per hook call — sufficient for mount-once tests
    },
    useEffect(fn, deps) { effects.push({ fn, deps }); },
    useState(init) {
      const id = stateSeq++;
      return [typeof init === 'function' ? init() : init, (v) => {
        shim.setStateCalls.push(id);
        return v;
      }];
    },
    createContext(defaultValue) {
      return { defaultValue, providers: [] };
    },
    useContext(ctx) {
      return ctx.providers.length > 0 ? ctx.providers[ctx.providers.length - 1].value : ctx.defaultValue;
    },
    createElement(type, props, ...children) {
      return { type, props: props || {}, children };
    },
  };
  const shim = {
    React,
    effects,
    setStateCalls: [],
    mount() {
      for (const e of effects) e.cleanup = e.fn();
    },
    unmount() {
      for (const e of effects) if (typeof e.cleanup === 'function') e.cleanup();
      effects.length = 0;
    },
    // Provider element whose value useContext resolves
    provider(ctx, value) {
      return { __provider: true, ctx, value };
    },
    pushProvider(el) { el.ctx.providers.push(el); },
    popProvider(el) {
      const i = el.ctx.providers.indexOf(el);
      if (i >= 0) el.ctx.providers.splice(i, 1);
    },
  };
  return shim;
}
