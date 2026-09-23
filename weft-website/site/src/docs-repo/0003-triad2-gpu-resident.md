---
RFC: 0003
Title: Triad-2 GPU-Resident Mode
Status: Draft
Authors: Weft Core Team
Created: 2026-09-15
Supersedes / Superseded-by: None
---

# RFC 0003 — Triad-2 GPU-Resident Mode

## Summary
Proposes Triad-2, a GPU-resident zero-copy frame exchange architecture for high-throughput compute-to-render pipelines (e.g., WebGPU compute shaders, Metal compute, Vulkan compute). Instead of CPU-mediated staging buffer copies, three device-local `GPUBuffer` allocations rotate via index/descriptor exchange, keeping all frame data entirely in VRAM.

## Motivation & Analytical Model (Simulation-Only)
In graphics-intensive workloads (spectrogram waterfall, particle systems), CPU staging memory uploads introduce ~84 µs latency per frame in software staging copies. In an analytical Python simulation model (`spikes/gpu-resident/gpu_pingpong_bench.py`), maintaining 3 device-local buffers and exchanging indices via atomic swap reduces metadata handoff overhead to ~0.47 µs (~180x theoretical speedup).

> [!IMPORTANT]
> **Epistemic Disclosure (SIMULATION-ONLY)**:
> The 0.466 µs / 180.7x speedup figure is derived from an analytical Python simulation model of index pointer swaps versus memory copy staging. **No physical GPU or WebGPU backend was exercised in the sandbox**. Actual hardware execution on Vulkan/Metal/Dawn is explicitly **HARDWARE-DEFERRED** per the Owner Binding Pivot.

## Guide-level explanation
Developers using WebGPU, Metal, or Vulkan can bind `WeftGpuWriter` and `WeftGpuReader`. The compute pipeline writes to `GPUBuffer` slot `w_work`, while the render pipeline draws from `r_work`. Exchanging buffers takes zero memory bandwidth.

## Reference-level specification
- **Buffer Triad**: 3 pre-allocated `GPUBuffer` objects (`STORAGE | UNIFORM | VERTEX`).
- **Exchange Protocol**: Identical to RFC 0001 (Atomic exchange of 2-bit buffer slot index in uniform buffer or host metadata block).
- **Shader Binding Contract**: WGSL, MSL, and AGSL bindings receive dynamic offsets or descriptor tables indexing slot 0, 1, or 2.

## Boundary of the claim (Law 4)
Triad-2 eliminates host-to-device memory copy overhead. It does not eliminate GPU execution pipeline stalls if the compute shader exceeds frame budget.

## Alternatives considered
- Single buffer with GPU fences: Causes pipeline bubbles and sync stalls.
- CPU-to-GPU staging ring: High bandwidth overhead and PCIe bottleneck.

## Drawbacks
Requires GPU memory allocation for 3 full frame buffers up front; requires compute/render pipeline separation.

## Open questions
- Automatic integration with Android `AHardwareBuffer` and Apple `IOSurface` (deferred to hardware evaluation).

## Hardware Deferral List
- Physical device thermal persistence on Mali/Adreno/Apple Silicon GPU is deferred.
- WebGPU / Metal hardware driver backend measurements are deferred (simulation-only sandbox).


## Hardware-backed spike (Series 7 — this tree)

The Staff Decision bar was "pending hardware-backed spike / design
exploration." The spike now exists, driver-layer shaped:

- **`core/c/gpu_ring.{h,c}`** — the RFC-0004 fan-out ring allocated in
  memory a GPU dereferences directly: Vulkan `HOST_VISIBLE|HOST_COHERENT`
  device memory via a **dlopen'd loader** (no link-time dependency),
  first compute-capable queue family, persistent `vkMapMemory`. The CPU
  publishes through the ordinary fan-out API on the mapped pointer; the
  GPU binds the SAME `VkBuffer` as a storage buffer — **no staging
  buffer, no `vkCmdCopyBuffer`**, one allocation. Apple unified memory is
  the compile-gated METAL backend (compiled by the apple CI leg; the MSL
  binding story remains the open question below — declared, not claimed).
  A CPU fallback keeps the module testable on GPU-less hosts, and the
  backend tag travels with every claim (Law 4).
- **`probes/compute/validate_frame.comp`** — the GPU-side consumer: a
  compute shader that reads the LIVE ring words (latestSeq, slotSeq,
  payload) and validates the frame against the 04-LITMUS mixer family
  entirely on the GPU, reporting through a result buffer. This is the
  "compute shaders directly consume live Weft memory" claim, executable.
- **`core/c/gpu_probe.c`** — the proof harness: publish CPU-side, two
  compute dispatches (after frame 1 and after frame N — the observed seq
  must ADVANCE, proving the GPU reads live memory with no re-upload).

Evidence (`litmus/evidence/gpu-ring/gpu-series.log`): the allocation leg —
real Vulkan instance/device/buffer/memory, persistent map, live fan-out
publishes — is executable-proven in the x86_64 sandbox against Mesa
lavapipe 25.0.7 (a real Vulkan loader + driver stack executing in
software). The dispatch leg is **CI-gated, not sandbox-claimed**: this
sandbox caps a single mapping at ~126 GiB and llvmpipe's LLVM JIT reserves
~94 TB for shader codegen, so `vkCreateShaderModule` returns
`OUT_OF_HOST_MEMORY` (exit code 4 distinguishes exactly this). Standard
kernels (the `gpu-native` CI shard: ubuntu-latest + mesa-vulkan-drivers +
glslang-tools) allow the reservation and run the FULL dispatch proof,
including a geometry sweep and a SPIR-V rebuild byte-identity gate. Per
the round-7 F-2 precedent: no GPU performance numbers are claimed from
software execution, and none are quoted — the spike removes the STRUCTURAL
staging copy, which is what this RFC proposes.

Relationship to the original design: Triad-2's "3 device-local GPUBuffer
allocations rotated via index exchange" maps onto the existing RFC-0004
ring slots (M=3 is the triad) with the slot stamps already providing the
index protocol; the session header (`WFSH`, shared with RFC-0011's shm
sessions) is the descriptor-side contract. The WebGPU staging-ring road
(`GPUBufferUsage.MAP_READ`) remains browser-gated and is NOT claimed here.

## Staff Decision
[NOT ACCEPTED pending hardware-backed spike / design exploration accepted]
