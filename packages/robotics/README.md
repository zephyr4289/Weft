# @weft/robotics

Weft robotics/vision managed connectors — RNG1 record-ring reader,
`rmw_weft` managed shim, zero-copy point-cloud/IMU decoders, and the
zero-re-render **`<WeftPointCloudViewer />`** (WebGL2) and
**`<WeftAttitudeIndicator />`** (Canvas2D). Pillar 6 deliverable B
(TS side; Python twin in `python/weft_robotics`).

## Architecture

```
Engineer 2's native rmw_weft / V4L2-DMA-BUF grabber
    │  shared memory (RNG1 ring, arena slots)
    ▼
attachRing(buffer)                      ← the ONLY native touch point
    │  acquire() — drop-not-block seqlock flyweight
    │  pointsView() / boxesView() → Float32Array windows OVER RING BYTES
    ▼
<WeftPointCloudViewer />                <WeftAttitudeIndicator />
  renders ONCE, rAF loop,                  renders ONCE, rAF loop,
  bufferSubData(straight from window),     quaternion → horizon,
  stats node mutated in place              zero per-frame allocation
```

- **Zero-copy**: `pointsView()` returns a `Float32Array` whose buffer IS
  the ring — 100,000+ LiDAR points per frame, zero JS object allocation,
  no copies. The WebGL upload (`bufferSubData`) consumes the window
  directly.
- **Zero-re-render** (Law 4 / W4-01, W4-06): the components run exactly
  once; feed updates never trigger React reconciliation — canvas pixels
  and DOM text nodes are mutated in place. Proven by a recording shim:
  1,000 live frames → 1,000 engine draws, 4 `createElement` calls total.
- **Drop-not-block** (W4-05): torn/overwritten windows are counted and
  skipped — never thrown, never awaited.
- **Parity**: TS and Python readers produce identical decisions on the
  frozen `rng1-ring.bin` fixture (10 records: 7 continuous + 1 torn +
  2 overwritten windows).

## RNG1 wire (docs/adapters/MANAGED-SEAMS-V1.md §5)

128 B header + `slot_count` × `slot_size` slots, little-endian. Slot:
`seq u64 · len u32 · topic_id u32 · ts_ns u64 · fmt u32 · flags u32 ·
32 reserved` then payload. fmt codes: `1` IMU6DOF (8×f64), `2`
POINTS_F32 (N×3×f32), `3` FRAME_DESC (FRM1), `4` BOXES_F32 (M×6×f32).

## Usage

```js
import { attachRing, FMT_POINTS_F32 } from '@weft/robotics';
import { createWeftPointCloudViewer } from '@weft/robotics';

const ring = attachRing(sharedMemoryBuffer);
const source = { acquire: () => ring.acquire() };
const Viewer = createWeftPointCloudViewer(React);
// <Viewer source={source} createEngine={({ gl }) => new PointCloudEngine(gl)} />
```

## Verification

| Gate | Command | Result |
|------|---------|--------|
| TS suite (12 tests) | `node --test "test/*.test.mjs"` | all green |
| 1M acquisition heap probe | `node --expose-gc probes/alloc-probe.mjs` | −7.6 KiB (gate 64 KiB) |
| Biting negative control | `node --expose-gc probes/alloc-probe.mjs control` | +746 KiB bites |
| Python twin (22 tests) | `pytest python/weft_robotics` | all green |
| Stage 6 pointer identity | `python/weft_robotics/test/test_camera.py` | DLPack addr == slot addr |
