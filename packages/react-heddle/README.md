# @weft/react-heddle

**Zero-re-render React bindings for the Weft Hot-Plane (HPL1).** Telemetry streams
at 100k+ events/second *never* pass through `useState`, reconciliation, or the GC:
per-frame updates mutate DOM nodes and preallocated buffers in place, and canvas
frames are driven by a drop-not-queue 240 Hz scheduler straight from shared memory.

```jsx
import { WeftCanvas, useWeftSignal, useWeftStats, useWeftBuffer, useWeftPlane } from '@weft/react-heddle';

function PriceTicker({ plane }) {
  const ref = useWeftSignal(0, { plane });       // micro-DOM mutator — no re-render
  return <span ref={ref} />;
}

function LiveStats({ plane }) {
  const stats = useWeftStats(0, { plane });      // STABLE object mutated in place
  return <canvas ref={bindStatsToDom(stats)} />; // read it anywhere — identity never changes
}

function Trend({ plane, engine }) {
  return <WeftCanvas plane={plane} engine={engine} hz={240} fallback="CHART OFFLINE" />;
}
```

## Hook contract

| Hook | Returns | Allocation behavior |
|---|---|---|
| `useWeftSignal(lane, {textDivider?, mutate?})` | stable ref callback — binds a node, writes `node.nodeValue` every Nth frame (default ≈60 Hz text cadence) | zero per frame |
| `useWeftStats(lane)` | stable live stats object `{current,min,max,avg,samples,seq,…}` mutated in place | zero per frame |
| `useWeftBuffer(lane)` | stable `{buffer, publish}` — preallocated scratch for interaction handlers (MANUAL lanes) | zero per frame |
| `useWeftPlane(bufferOrCtx)` | shared `PlaneContext` (one view + one scheduler per buffer, refcounted) | once per buffer |

## `<WeftCanvas />`

Mounts a render engine (`{contextType, init(canvas, ctx, view), render(state, frameCtx, view)}`)
directly against the Hot-Plane view. **The telemetry path contains zero `setState`** —
the only exceptional re-render is the Law-4 fallback view on fatal boundaries:
GPU context loss (`webglcontextlost`), a null context, or a malformed engine.
Page Visibility pauses the scheduler explicitly (resume renders exactly one
frame — never a catch-up burst).

## Testing without React

Everything is exported as factories (`createHeddleHooks(React)`,
`createWeftCanvas(React, hooks)`); the CI suite drives the full binding layer
through a ~50-line mount-semantics shim (`test/shim.mjs`) and **asserts zero
`setState` calls across 100k streamed frames**.
