# weft-tensor — the NPU/GPU accelerator pillar (RFC-0017, Pillar 2)

Four accelerator adapters + one ABI seam that connect Weft's DMA tensor
ring to hardware execution units with **zero staging copies**:

```
weft_tensor_view_t (128 B, ABI v1 — Engineer 1's ring seam)
     ├── backends/vulkan   dma-buf / host imports (alias canary) +
     │                     pooled compute; frozen preprocess kernel
     ├── backends/metal    MTLBuffer / CVPixelBuffer / IOSurface wraps
     │                     (apple CI leg; honest stubs elsewhere)
     ├── backends/onnx     dlopen'd OrtApi (verified table mirror),
     │                     zero-copy wrap + pooled no-alloc OrtRun
     └── backends/ggml     custom buffer type + placement planner +
                           audio windowing (data-pointer road for all eras)
```

## Build & gates

```
make all            # the AC-series binaries + bench
make test           # run every gate (the no-ICD road where no ICD exists)
make evidence       # tee everything to litmus/evidence/accelerators/
make spv-check GLSLANG=glslangValidator   # kernel freeze (rebuild + diff)
make test-view-asan test-cross-asan       # ASAN legs
make apple          # the Metal device roads (macOS/iOS only)
```

With a Vulkan ICD present (e.g. lavapipe), export `VK_ICD_FILENAMES`
(+ `LD_LIBRARY_PATH` if the ICD lives outside the default path) and the
AC-K/AC-X/bench ICD legs activate: fd-road dma-buf import, GPU
preprocess bit-exact vs the scalar oracle, and the alias canary.

The end-to-end bench: `./weft-accel-bench` (or `make weft-accel-bench`).
Every stage carries its road label — `[ZERO-COPY]` / `[FALLBACK-COPY]` /
`[SIM-EP]` / `[WTS1]` — and the gates print a JSON evidence line.

## The contracts worth knowing

- **The view ladder** (`include/weft/weft_tensor_view.h`): every
  adapter validates the same way; refusals are named negative codes;
  `gpu_ready` is the 16-byte/LE alignment law (Law 2).
- **The normalize contract** (`include/weft/weft_accel_common.h`):
  `out[i] = (f32)src[i] * scale` — one exact conversion + ONE rounded
  multiply. No bias exists in v1 precisely so scalar == SIMD == GPU is
  an ==-gate (nothing for a shader compiler to fuse into an FMA).
- **The alias canary** (`weft_vk_alias_verify`): an import that does
  not carry the producer's own bytes is REFUSED, never silently
  consumed (the Series-10 llvmpipe discovery, now a reusable gate).
- **Honesty labels**: every measured line names its road; software-ICD
  execution and driver-internal allocations are reported as such, never
  laundered into hardware claims.

## Layout

```
include/weft/     the ABI seam + shared plumbing (stats, frozen-id, SIMD)
src/              view + common + SIMD implementations
backends/         one directory per accelerator; no cross-backend includes
bench/            the E2E latency harness + the deterministic SIM-EP
tests/            the AC-series gates (V/K/M/O/G/X)
shaders/          frozen kernels: .comp+.spv (Vulkan), .metal (Apple)
```

Reports: `reports/D-29-WEFT-TENSOR-NATIVE.md` · RFC:
`rfcs/0017-accelerator-zero-copy-adapters.md` · Evidence:
`litmus/evidence/accelerators/`.
