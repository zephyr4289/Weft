# weft-tensor (Python)

Zero-copy Python bridge into the Weft managed tensor plane (WTR1 layout —
`docs/weft-tensor/LAYOUT-V1.md`). This is Pillar 2 Deliverable B: the AI
ecosystem door — NumPy and PyTorch tensors that read STRAIGHT out of Weft
shared memory with no serialization, no base64, no copies.

```python
import torch, weft_tensor as wt

ring = wt.WeftRing.attach("camera_ring.mm")     # mmap'd WTR1 memory
view = ring.acquire_latest()                    # wait-free, zero-alloc
tensor = torch.from_dlpack(view)                # zero-copy -> PyTorch
arr = view.as_numpy()                           # zero-copy -> NumPy (shared alias)
```

## What's here

| Module | Contents |
|---|---|
| `weft_tensor/layout.py` | WTR1 constants, CRC-32/IEEE (chained zlib — oracle-verified vs node:zlib), fail-closed validation with TS-identical error codes |
| `weft_tensor/ring.py` | `WeftRing.create()/attach()` — seqlock producer/consumer, per-slot precreated memoryviews + ndarrays at attach (hot loop allocates nothing) |
| `weft_tensor/view.py` | `WeftTensorView` flyweight — `__dlpack__`/`__dlpack_device__`, `__array_interface__`, `as_numpy()` — all aliasing THE SAME ring memory |
| `weft_tensor/dlpack_capi.py` | ctypes DLPack: `DLManagedTensor` capsules, malloc-only scratch (freed solely by the consumer-triggered deleter), owner lifetime via registry |
| `weft_tensor/ingest.py` | `AudioPcmFeeder` — PCM chunks across slot boundaries at the declared quantum, zero-alloc carry |

## DLPack notes (the hard parts, done properly)

- Capsules are the **unversioned `"dltensor"`** protocol — accepted by
  `np.from_dlpack` and `torch.from_dlpack` alike; consumers rename to
  `used_dltensor` on adoption.
- Struct/shape/strides scratch is **`malloc`'d and freed ONLY by the deleter**
  the consumer invokes — ctypes never owns that memory (a ctypes-owned buffer
  freed by libc is a double-free; we crash-tested this exact failure mode).
- The ring that produced a tensor is kept alive by a registry reference until
  the deleter fires — exactly DLPack's lifetime semantics.
- `strides=NULL` is served for C-contiguous rings (broadest consumer compat);
  real element strides otherwise (DLPack convention, matching the wire).

## Zero-allocation discipline (Law 1)

`acquire_latest()`, `as_numpy()` and the audio feed loop return/operate on
precreated objects (per-slot memoryviews, per-slot ndarrays, preallocated
carry state). `tests/test_alloc.py` proves **0 GC collections** over 10k
steady-state frames (ring) and 20k audio chunks, measured via `gc.callbacks`.

## Law 4 boundary

`WeftRing.attach()` runs the full fail-closed validation (magic, version,
sizes, dtype, strides sanity, LE flag, chained-zlib CRC, bounds). Corruption
classes map 1:1 to the TypeScript `LayoutError` codes, e.g. `WTR1_BAD_CRC`,
`WTR1_NOT_LITTLE_ENDIAN`.

## Torch without torch

The torch lane is skip-guarded with an explicit reason (`test_dlpack.py`);
`np.from_dlpack` exercises the identical protocol path. Where torch exists
(CI gpu lane), `torch.from_dlpack(view)` is asserted element-exact against
the ring payload.
