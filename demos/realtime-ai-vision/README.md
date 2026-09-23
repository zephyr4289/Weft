# demos/realtime-ai-vision

End-to-end flight demo for Pillar 2 (weft-tensor): camera → WTR1 ring →
<1 ms detection → 120 FPS zero-re-render canvas.

## Terminal edition (`demo.mjs`)

```bash
node demos/realtime-ai-vision/demo.mjs            # painted ASCII flight (~30 Hz HUD)
node demos/realtime-ai-vision/demo.mjs --no-paint # Law-1 pure flight (GC pauses = verdict)
```

What it proves (evidence JSON at the end of each run, also in `evidence/`):

- `produced/rendered/stalls` — 120 Hz producer through the seqlock ring
- `detection.p99_us` — REAL connected-component detector, p99 ≈ 0.1 ms
  (directive gate: < 1 ms)
- `gc` + `heapDeltaBytes` — in `--no-paint` mode these are the Law-1 flight
  verdict for the whole pipeline; in painted mode the delta is the terminal
  ANSI string fabric (display layer only — the web path has no such layer;
  the zero-alloc property of the render path itself is probe-verified in
  `packages/weft-tensor` `alloc_probe` `render` mode)

## Browser edition (`web/`)

Serve the repo root (`python3 -m http.server`) and open
`demos/realtime-ai-vision/web/index.html`. Same pipeline via
`Canvas2DPlane` + `RingListener` + `OverlayScratch` — no bundler, plain
ES modules importing `@weft/tensor` by relative path. The **use webcam**
button routes a real camera through `VideoFrame.copyTo` (WebCodecs) straight
into the ring slot memory — the zero-intermediate ingestion path.
