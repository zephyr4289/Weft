# Weft-WASM — the C core compiled to WebAssembly (issue #18-1)

The frozen C core (`weft.c`, `fanout.c`, `fanout_simd.c`, `fanout_batch.c`)
compiled by Emscripten, with TypeScript bindings and a browser demo.

## Build

```sh
core/wasm/build.sh            # single-threaded flavor (any host, no headers needed)
core/wasm/build.sh pthreads   # SharedArrayBuffer flavor (serve with COOP/COEP)
```

Output: `core/wasm/dist/libweft.{js,wasm}` — an ES module (MODULARIZE +
EXPORT_ES6) exporting `initWeftWasm()`. Built artifacts are NOT committed
(the repo does not track binaries); CI builds from source.

## Zero-copy contract

The wasm linear memory IS the shared memory between C and JS:
- **Reads** — `payload()`, `view()`, `ringBytesView()`, `ctrlView()` return
  typed-array VIEWS over wasm memory. No copies, ever.
- **Writes** — `frameBuffer(n)` hands you a persistent buffer inside wasm
  memory: fill it (Uint32Array view) and `publish()` it — the frame never
  crosses the boundary.
- **Ring access** — the whole RFC-0004 ring is viewable byte-for-byte
  (`ctrlView()` exposes latestSeq/publishes/slotSeq at their documented
  offsets) — the same wire layout the TS SAB port, the Rust port, and WFSH
  IPC sessions speak.

Growth caveat: views detach if the wasm heap grows (16 MiB initial — far
beyond canvas/telemetry workloads); re-fetch views after allocations if
you push past it.

## Battery

`node --test --experimental-strip-types core/wasm/test/litmus.ts` — the
L1–L8 litmus analogs (WL-series) plus the fan-out battery (WF-series:
roundtrip, drop telescoping, the cross-port wire layout, the #17-3
100-frame batch, zero-copy frame buffers). 12/12 green on the evidence
host; CI runs it on every push.

Browser leg: `scripts/verify-wasm-demo-ci.sh` opens
`demos/wasm-canvas` in headless chromium and requires zero page errors +
advancing counters (MEASURED locally: seq 241→421 over 7 s, consumers A/C
each fresh=121/dropped=120 — exactly-accounted drops at half cadence).

## The demo

`demos/wasm-canvas` — a collaborative canvas: a painter (auto-bot or your
pointer) publishes stroke frames into an 8-slot fan-out ring; three
independent consumers claim from the same ring at their own cadence (live
view, minimap, persistent trail). Serve it:

```sh
cd demos/wasm-canvas && python3 -m http.server 8788
# open http://localhost:8788
```

## Honest boundaries

- The single-threaded flavor covers the full kernel + fan-out protocol
  (the protocol tests are single-threaded by construction). Multi-worker
  fan-out (one reader per worker) uses the pthreads flavor and needs
  COOP/COEP — same requirements as the TS SAB port; the browser-sab CI
  leg pattern applies.
- Firefox/Safari execution is expected (standard ESM + wasm BigInt — both
  Baseline) but evidence-gated to their CI legs; the local evidence is
  Chromium (headless). Declared, not assumed.
- The claim-copy dispatcher resolves **scalar** on wasm32 (no SIMD path in
  the port yet — WebAssembly SIMD is the natural fanout_simd addition;
  the seam from issue #17-1 is where it lands).
