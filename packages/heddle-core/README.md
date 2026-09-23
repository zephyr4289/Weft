# @weft/heddle-core

Framework-agnostic engine for the **HPL1 Weft Hot-Plane** — the zero-copy,
zero-allocation memory plane that UI frameworks bind to directly
(`docs/heddle2/HPL1-LAYOUT-V1.md`).

## What's inside

| Module | Role |
|---|---|
| `layout.js` | Pinned HPL1 offsets, geometry derivation, fail-closed plane validation |
| `plane.js`  | `HotPlaneView` — consumer side: seqlock lane/header reads into caller-owned `out` structs, newest-first ring reads, dirty-mask transition scans |
| `producer.js` | `HotPlaneProducer` — single-writer side: per-lane seqlock publish, in-place min/max/avg statistics, dirty broadcast, epoch restart |
| `scheduler.js` | `FrameScheduler` — 60/120/240 Hz fixed cadence, drop-not-queue, injectable clock/raf |
| `errors.js` | 15-code Law-4 taxonomy (`HPL1_BAD_MAGIC` … `HPL1_RING_UNDERRUN`) |

## The contract

```js
import { HotPlaneProducer, HotPlaneView, FrameScheduler, makeLaneOut } from '@weft/heddle-core';

const producer = HotPlaneProducer.create({ laneCount: 16, samplesPerLane: 256, tickHz: 240 });
// hand producer.planeBuffer (a SharedArrayBuffer) to any consumer thread

const view = new HotPlaneView(producer.planeBuffer);
const out = makeLaneOut();                 // allocate ONCE, reuse forever
setInterval(() => {
  if (view.readLane(3, out) === 0) draw(out.current, out.min, out.max, out.avg);
}, 4);

const sched = new FrameScheduler({ hz: 240 });
sched.start((ctx) => renderFrame(ctx));    // ctx is reused — zero alloc per frame
```

## Laws (enforced by the CI shard)

1. **Zero allocation on hot paths** — probes/alloc-probe.mjs under `--expose-gc`:
   100k publishes, 100k consumer reads, 100k scheduler pumps — all < 32 KiB heap
   growth (measured: ≤ 0; negative control +207 KB proves the probe bites).
2. **Determinism & little-endian** — every multi-byte access is explicitly LE;
   u64 as lo/hi u32 with lo-last publish ordering; golden fixtures pin the bytes.
3. **Byte-frozen kernel** — managed code only; `core/c/` untouched.
4. **Honest boundaries** — tears, epoch changes, underruns, detachment are
   coded events, never silent.
