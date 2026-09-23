# @weft/react-tensor

Zero-re-render React hooks for real-time AI canvases over Weft tensor rings.
Bounding boxes, segmentation masks and keypoint skeletons update at display
refresh **without a single React re-render, without a single per-frame
allocation** (Law 1) — the pixels are the only thing that changes.

```jsx
import { WeftTensorRing } from '@weft/tensor';
import { useWeftTensorCanvas } from '@weft/react-tensor';

function AiCameraView({ ring }) {
  const canvasRef = useRef(null);
  const statsRef = useWeftTensorCanvas(ring, canvasRef, {
    backend: '2d',                    // or 'webgl' (texSubImage2D path)
    overlay: (frame, scratch, plane) => {
      // Detector-side work on preallocated scratch ONLY.
      // Run your detector here (or read boxes another ring), then:
      scratch.clear();
      scratch.pushBox(x, y, w, h, score, classId);
    },
  });

  // HUD counters are read OUTSIDE React state (rAF/interval), because
  // statsRef.current is the controller's own sealed stats object:
  // useEffect(() => { const id = setInterval(() => fpsEl.current.textContent
  //   = `${statsRef.current.frames} frames`; , 500); return () => clearInterval(id); }, []);

  return <canvas ref={canvasRef} width={640} height={360} />;
}
```

## Why there are no re-renders

- The hook registers exactly ONE effect per `(ring, canvasRef)`; inside it a
  `TensorCanvasController` owns a `RingListener` + render plane.
- Frame callbacks mutate canvas pixels and the controller's **sealed stats
  object**; `statsRef.current` IS that object (same identity, live values) —
  React state is never touched, so React never re-renders.
- The overlay contract: YOUR callback owns the scratch lifecycle
  (`scratch.clear()` then pushes). `OverlayScratch` is ONE preallocated
  `Float32Array` (stride-6 records) — detectors never allocate either.

## Test discipline

`test/controller.test.mjs` drives the full frame path headlessly with an
injected pump (stall ticks repaint last frame + overlay, frame ticks blit
once), and `test/hooks.test.mjs` proves the effect/teardown semantics through
a minimal hooks shim. The `react` peer dependency is exercised only by
consumers; the core has zero dependencies.

## Layout/laws

Consumes WTR1 rings (`docs/weft-tensor/LAYOUT-V1.md`) via `@weft/tensor`
(in-repo relative import; declare it as a dependency when packaging).
Laws: zero-alloc steady state (Law 1), strict LE (Law 2 — inherited),
browser+node (Law 3), ring validation at the boundary (Law 4).
