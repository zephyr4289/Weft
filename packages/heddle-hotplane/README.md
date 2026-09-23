# @weft/heddle-hotplane

The heddle-2.0 **WHP2 unified hot-plane** engine for JavaScript: a
zero-copy mirror of the normative [HOTPLANE-LAYOUT-V2](../../docs/heddle/HOTPLANE-LAYOUT-V2.md)
memory layout, operating directly on a `SharedArrayBuffer` shared with
C producers (via the [WASM bridge](../../core/wasm/heddle_bridge.c) or a
native N-API/FFI producer).

```js
import { HotPlane } from '@weft/heddle-hotplane';

const plane = new HotPlane(sharedArrayBuffer);   // full validation ladder

if (plane.epoch !== lastEpoch) {                 // idle-frame killer
  const mask = plane.harvestMask();              // the frame barrier
  for (const lane of bitsSet(mask)) {
    const bbox = plane.bboxHarvest(lane);        // mutated index range
    const out = new Uint8Array(plane.sampleSize);
    if (plane.readCell(lane, cell, out) === 0) { /* consistent frame */ }
    else plane.remark(1n << BigInt(lane));       // defer torn lane
  }
  plane.frameCommit();
}
```

* **Zero allocation** on every hot path (Law 1) — all state lives in
  the plane; the class holds only views and decoded constants.
* **Lock-free two-store seqlock reads** with bounded retries and
  honest `HEDDLE_E_SEQ_TORN` refusal — never torn data (Law 4).
* **Atomics on a `BigUint64Array` register view** — every access is a
  data-race-free atomic; sequentially consistent, which is stronger
  than the engine's acquire/release ordering, so the C protocol
  remains correct by construction.
* Verified **byte-exact** against the C engine's golden plane image
  (`core/c/heddle/golden/`): 37 interop checks decode every field,
  every lane's stats and bounding boxes, and every cell's payload.

Run the interop suite: `npm test` (needs the repo's golden files).

Environment: Node ≥ 18 (or any COOP/COOP-enabled browser context with
`SharedArrayBuffer`). The plane must sit at a 64-byte-aligned offset.
