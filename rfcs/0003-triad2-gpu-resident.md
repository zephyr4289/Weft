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

## Motivation
In graphics-intensive workloads (spectrogram waterfall, 10,000+ particle systems), CPU staging memory uploads introduce 80–130 µs latency per frame. By maintaining 3 device-local GPU buffers and exchanging indices/pointers via atomic swap, handoff latency is reduced to sub-microsecond levels (~0.47 µs, 180x speedup).

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

## Staff Decision
[EMPTY]
