// react.js — <WeftPointCloudViewer /> and <WeftAttitudeIndicator />
// factories (injected React, Pillar 4/6 precedent).
//
// Zero-re-render contract (Law 4 / W4-01, W4-06):
//   - Each component function runs EXACTLY ONCE. It renders one <canvas>
//     plus a hidden FALLBACK banner and wires an engine to them. Telemetry
//     (msg rate, frame stats) mutates DOM text nodes in place — React
//     reconciliation is NEVER triggered by feed updates.
//   - The rAF loop calls source.acquire() (drop-not-block) and repaints
//     only on a new record seq (edge-trigger).
//   - A dead/gone canvas or context loss degrades to the FALLBACK banner
//     (mutated directly) — never an uncaught exception (W4-04).

import { makeMvp } from './viewer-engine.js';

export function createWeftPointCloudViewer(React) {
  const { useRef, useEffect, createElement } = React;
  return function WeftPointCloudViewer(props) {
    const { source, width = 640, height = 480, controllerRef } = props;
    const canvasRef = useRef(null);
    const statsRef = useRef(null);
    const bannerRef = useRef(null);
    useEffect(function mount() {
      const canvas = canvasRef.current;
      const gl = canvas !== null && canvas !== undefined &&
        typeof canvas.getContext === 'function'
        ? canvas.getContext('webgl2', { antialias: false })
        : null;
      if (gl === null) {
        if (bannerRef.current !== null) {
          bannerRef.current.style.display = 'block';
          bannerRef.current.dataset.weftFallback = 'shown';
        }
        return undefined;
      }
      const engine = props.createEngine({ gl });
      const stats = statsRef.current;
      let raf = 0;
      let lastSeq = -1;
      const mvp = new Float32Array(16);
      function frame(t) {
        const rec = source.acquire();
        if (rec !== null && rec !== undefined && rec.seq !== lastSeq) {
          lastSeq = rec.seq;
          const f32 = rec.pointsView();
          engine.draw(f32, (rec.payloadLen / 12) | 0,
            makeMvp(mvp, t * 1e-4, 0.5, 3));
          if (stats !== null && stats !== undefined) {
            stats.textContent = `${engine.pointsDrawn} pts @ ${engine.frames}f`;
          }
        }
        raf = requestAnimationFrame(frame);
      }
      raf = requestAnimationFrame(frame);
      if (controllerRef !== undefined) {
        controllerRef.current = { engine, stop() { cancelAnimationFrame(raf); } };
      }
      return function unmount() {
        cancelAnimationFrame(raf);
        if (controllerRef !== undefined) controllerRef.current = null;
      };
    }, []);
    return createElement('div', {
      className: 'weft-pointcloud',
      style: { position: 'relative', width, height },
    },
      createElement('canvas', { ref: canvasRef, width, height, key: 'c' }),
      createElement('div', { ref: statsRef, key: 's',
        style: { position: 'absolute', top: 4, left: 4, color: '#8ecae6' } }),
      createElement('div', {
        ref: bannerRef,
        'data-weft-fallback': 'hidden',
        style: { display: 'none', position: 'absolute', top: 0, left: 0 },
        key: 'b',
      }, 'VIEWER FALLBACK'),
    );
  };
}

export function createWeftAttitudeIndicator(React) {
  const { useRef, useEffect, createElement } = React;
  return function WeftAttitudeIndicator(props) {
    const { source, width = 240, height = 240 } = props;
    const canvasRef = useRef(null);
    const bannerRef = useRef(null);
    useEffect(function mount() {
      const canvas = canvasRef.current;
      const ctx = canvas !== null && canvas !== undefined &&
        typeof canvas.getContext === 'function'
        ? canvas.getContext('2d')
        : null;
      const engine = props.createEngine({ ctx });
      if (engine.ok !== true) {
        if (bannerRef.current !== null) {
          bannerRef.current.style.display = 'block';
          bannerRef.current.dataset.weftFallback = 'shown';
        }
        return undefined;
      }
      let raf = 0;
      let lastSeq = -1;
      const quat = new Float64Array(8);
      function frame(t) {
        const rec = source.acquire();
        if (rec !== null && rec !== undefined && rec.seq !== lastSeq) {
          lastSeq = rec.seq;
          rec.imuInto(quat);
          engine.draw(quat[1], quat[2], quat[3], quat[4], width, height);
        }
        raf = requestAnimationFrame(frame);
      }
      raf = requestAnimationFrame(frame);
      return function unmount() { cancelAnimationFrame(raf); };
    }, []);
    return createElement('div', {
      className: 'weft-attitude',
      style: { position: 'relative', width, height },
    },
      createElement('canvas', { ref: canvasRef, width, height, key: 'c' }),
      createElement('div', {
        ref: bannerRef,
        'data-weft-fallback': 'hidden',
        style: { display: 'none', position: 'absolute', top: 0, left: 0 },
        key: 'b',
      }, 'IMU FALLBACK'),
    );
  };
}
