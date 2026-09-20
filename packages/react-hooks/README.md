# @weft/react-hooks

Zero-GC React integration for Weft shared-memory frames. Subscribe to frame
sources at 120/240+ FPS and paint through refs and canvases — **React state
is never set**, so a component receiving 100,000 frames per second still
renders exactly once. No virtual-DOM reconciliation, no re-render spirals,
no GC pauses from the render pipeline.

Part of the weftc managed layer (Pillar 1). Pairs with weftc-generated
flyweight views (`tools/weftc/codegen/ts`): the same view instance is
re-bound per frame — zero allocations in steady state (Law 1).

## Install

```sh
pnpm add @weft/react-hooks   # peer dep: react >= 18
```

## `useWeftBuffer` — subscribe without re-rendering

```tsx
import { createFrameSource, useWeftBuffer } from '@weft/react-hooks';
import { WeftEnvelopeView, TelemetryFrameView } from './generated';

const source = createFrameSource();
ws.onmessage = (e) => source.emit(e.data, 0, e.data.byteLength);

function Component() {
  const env = new WeftEnvelopeView();     // one instance per thread
  const frame = new TelemetryFrameView(); // rebound per frame — zero alloc

  useWeftBuffer(source, (buffer, byteOffset, avail) => {
    if (!env.bind(buffer, byteOffset).validateHeader(avail)) return;
    if (!frame.bind(buffer, byteOffset + env.headerSize).validateHeader()) return;
    // imperative DOM/Canvas/serial updates here — NOT setState
    statusEl.textContent = `${frame.timestampNsLo} ${frame.pressurePa}`;
  });

  return <canvas ref={...} />; // never re-renders from frames
}
```

## `useWeftCanvas` — paint at display cadence

Frames coalesce into animation ticks (newest wins); the rAF loop parks when
idle or when the document is hidden, and resumes on visibilitychange.

```tsx
const canvasRef = useWeftCanvas(source, (ctx, buffer, byteOffset, avail) => {
  if (!frame.bind(buffer, byteOffset).validateHeader()) return;
  ring.push(frame.pressurePa);            // preallocated Float32 ring
  drawFrameGraph(ctx, ring, 256, 64);     // auto-scaling oscilloscope
});

return <canvas ref={canvasRef} width={256} height={64} />;
```

## API

| Export | Purpose |
|---|---|
| `createFrameSource()` | Reference-stable pub/sub over `(buffer, byteOffset, availBytes)` triples |
| `attachWebSocket(source, ws)` | Wire a WebSocket as a binary frame producer |
| `useWeftBuffer(source, onFrame)` | Subscribe without re-rendering; latest-ref callback |
| `useWeftCanvas(source, draw)` | Coalescing canvas painter; returns `<canvas>` ref |
| `createSampleRing(capacity)` | Fixed-capacity Float32 oscilloscope ring (zero steady-state alloc) |
| `drawFrameGraph(ctx, ring, w, h)` | Auto-scaling polyline painter |

## Verification

`node --test test/` — 11 tests: zero-rerender contract (10k frames → 1
render), StrictMode double-mount safety, latest-ref resubscribe avoidance,
canvas coalescing/parking/resume/cancellation, ring wraparound, painter
auto-scale, WebSocket attach/detach. Run against a dep-aware React hooks
shim; the package itself carries zero runtime dependencies.
