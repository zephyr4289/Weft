// react.js — <WeftOrderBook /> and <WeftTelemetryBar /> factories.
//
// Zero-re-render contract (Law 4 / W4-01, W4-06):
//   - The component function runs EXACTLY ONCE. It renders one <canvas>
//     plus one <div> telemetry bar and wires a controller to them. Book
//     state and telemetry NEVER pass through props/state — the rAF loop
//     mutates canvas pixels and DOM text nodes directly.
//   - `React` is injected (factory pattern, Pillar 4 precedent) so the
//     component is testable with a recording shim — no React dependency
//     at module scope, no JSX build step.
//   - Transparent degradation (W4-04): a context-loss callback flips the
//     controller to FALLBACK; a banner node is mutated directly. No state,
//     no re-render, no uncaught exception.

export function createWeftOrderBook(React) {
  const { useRef, useEffect, createElement } = React;
  return function WeftOrderBook(props) {
    const { mdp1, telemetry, width = 420, height = 320, theme, clock, controllerRef } = props;
    const canvasRef = useRef(null);
    const bannerRef = useRef(null);
    useEffect(function mount() {
      const canvas = canvasRef.current;
      const ctx = canvas.getContext('2d');
      const controller = props.createController({
        mdp1, telemetry, ctx, width, height, theme, clock,
      });
      if (controllerRef !== undefined) controllerRef.current = controller;
      const stop = props.bindLoop !== undefined
        ? props.bindLoop(canvas, controller)
        : null;
      return function unmount() {
        if (stop !== null) stop();
        controllerRef && (controllerRef.current = null);
      };
    }, []);
    return createElement('div', { className: 'weft-orderbook', style: { position: 'relative', width, height } },
      createElement('canvas', { ref: canvasRef, width, height, key: 'c' }),
      createElement('div', {
        ref: bannerRef,
        'data-weft-fallback': 'hidden',
        style: { display: 'none', position: 'absolute', top: 0, left: 0 },
        key: 'b',
      }, 'FEED FALLBACK'),
    );
  };
}

// Telemetry bar: msgs/sec + microsecond processing latency. One render;
// per-frame updates mutate three text nodes in place (id-tagged children).
export function createWeftTelemetryBar(React) {
  const { useRef, useEffect, createElement } = React;
  return function WeftTelemetryBar(props) {
    const { telemetry, clock } = props;
    const rateRef = useRef(null);
    const latRef = useRef(null);
    const msgRef = useRef(null);
    useEffect(function mount() {
      let raf = 0;
      let running = true;
      // own timer; updates THREE text nodes; zero setState (W4-01)
      function tick() {
        if (!running) return;
        const s = telemetry.slots;
        if (rateRef.current !== null) {
          rateRef.current.nodeValue = (s[2] / 1e6).toFixed(3) + ' M msg/s';
        }
        if (latRef.current !== null) {
          latRef.current.nodeValue = (s[3] / 1000).toFixed(2) + ' us';
        }
        if (msgRef.current !== null) {
          msgRef.current.nodeValue = String(s[0]);
        }
        raf = requestAnimationFrame(tick);
      }
      raf = requestAnimationFrame(tick);
      return function unmount() { running = false; if (raf) cancelAnimationFrame(raf); };
    }, []);
    return createElement('div', { className: 'weft-telemetry-bar' },
      createElement('span', { key: 'm' }, 'msgs '),
      createElement('span', { key: 'm2', ref: msgRef }, '0'),
      createElement('span', { key: 'r' }, ' rate '),
      createElement('span', { key: 'r2', ref: rateRef }, '0.000 M msg/s'),
      createElement('span', { key: 'l' }, ' lat '),
      createElement('span', { key: 'l2', ref: latRef }, '0.00 us'),
    );
  };
}
