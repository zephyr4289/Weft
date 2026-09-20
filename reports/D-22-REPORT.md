# DIRECTIVE-22 REPORT: The Heterogeneous Zero-Copy Mesh (Series 10 / RFC-0016)

- **Directive**: D-22 (Series 10 — the lead's heterogeneous mandate: DMA-BUF / external-memory GPU / AF_XDP / tensor streams / camera seams)
- **Status**: COMPLETED / PASS (6 implementation patches + this docs/CI patch; hardware-deferred legs declared with asserted refusal gates)
- **Date**: 2026-09-19
- **Environment**: `x86_64-sandbox` (gcc 14.2.0, kernel 5.10.134 — no CONFIG_XDP_SOCKETS, no /dev/dma_heap, no /dev/video) · Vulkan loader 1.4.309 + **Mesa lavapipe 25.0.7** (software ICD — the gpu-native shard's precedent) · glslang 15.2
- **Reference**: RFC-0016; the Series-8 fd bridge (RFC-0013) and Series-9 SIMD/F16/device-tier layers it builds on

---

## 1. Executive Summary

Series 10 closes Weft's remaining copy seams under one stance: **the buffer
the producer made IS the buffer the mesh consumes.** One allocation —
page-aligned WFSH session storage — is aliased by the CPU (fan-out publish),
the GPU (Vulkan external-memory wraps), the NIC (AF_XDP UMEM), cameras
(V4L2 EXPBUF / AHardwareBuffer), Apple silicon (bytesNoCopy MTLBuffer), and
AI runtimes (WTS1 tensor frames carried as ordinary ring frames). Six
patches, 160+ new gate checks, every claim labeled MEASURED / CI-GATED /
DECLARED / HARDWARE-DEFERRED, kernel byte-frozen (gate-verified), zero
regressions on the existing GPU/uring/device-tier suites.

The load-bearing discovery of the series: **llvmpipe accepts host-pointer
imports but backs them with fresh memory** — a silent-degradation trap that
would have shipped a wrap consuming GPU-side zeros. The wrap constructors
therefore *verify aliasing* (canary round-trip) and refuse non-aliasing
devices; the refusal itself is a gated, asserted behavior, and the fd road
(llvmpipe mmaps the fd) carries the full MEASURED zero-copy proof instead.

## 2. Mandate Scoreboard

| Deliverable | What landed | Measured (this sandbox) | Status |
| :--- | :--- | :--- | :---: |
| **GPU external-memory import** (VK_KHR_external_memory_fd / VK_EXT_external_memory_host / Metal shared / WebGPU) | `weft_gpu_wrap_host` + `weft_gpu_wrap_dmabuf` + `weft_gpu_export_dmabuf_fd`; alias verification; `WeftMetalZeroCopy.swift`; WGSL mirrors | **fd road live on lavapipe**: CPU publishes 40→80 frames through its mmap, GPU consumes live words through the imported fd, window ADVANCES (mismatches=0); host road refused-with-verification on llvmpipe, hardware-deferred for aliasing drivers; Metal = apple-CI leg | **PASS+** (fd road MEASURED) |
| **Zero-copy compute shaders** (GLSL/MSL/WGSL over live ring slots) | `tensor_reduce.{comp,wgsl}` (deterministic 64-lane partition, adds-only, BIT-EXACT vs CPU mirror) + `fft_radix2.{comp,wgsl}` (peak-bin exact + Parseval self-check) | **Capstone MEASURED**: argmax/sum/max bit-identical both bursts (`0x1.70cp+4 == ref`); FFT peak bin 37 exact, magnitude rel 2.63e-9, Parseval 2.06e-7 | **PASS+** |
| **DMA-BUF direct binding** (Linux heaps + foreign fd import) | `weft_dmabuf` (hand-rolled uapi; system-heap-only stance — uncached heaps refused for ring backing; WFSH dialect byte-identity) | Substrate PROVEN everywhere: two-mapping aliasing, 2000-frame cross-view traffic, fork torture, cross-dialect attach; heap ioctl leg refused-and-declared here (no /dev/dma_heap), runs where heaps exist | **PASS** (heap leg declared) |
| **AF_XDP kernel-bypass ingestion** (NIC DMA into UMEM-as-ring-slots) | `weft_xdp_rx`: pre-bracketed fill ring (I1–I8 intact, depth-1 by construction), unaligned-chunk UMEM over the session span, honest ladder NONE→URING→SETUP→LIVE | State machine gated with synthetic descriptors (offset identity, misorder refusal, pad/truncate, telescoping EXACT); **500 real datagrams through the uring delegation**; LIVE rig shipped (privileged, exit-3 declared here — no CONFIG_XDP_SOCKETS) | **PASS** (LIVE declared) |
| **AI & tensor stream bridge** (llama.cpp / ONNX / Whisper / GGML) | `weft_tensor`: WTS1 in-band format (11 dtypes, F16 = the codec dialect), 5 adapter seams incl. the zero-copy raw-cursor road | 42 gates: all dtypes roundtrip, refusal ladder, F16 bit-exactness, 200-frame stream + rank-4, cross-surface through the substrate | **PASS+** |
| **Camera capture seams** (V4L2 / AVFoundation / Camera2) | `weft_hw_v4l2` (EXPBUF→dma-buf), `weft_hw_ahb` (native handle→fd, NDK-gated), Metal bridge above; Camera2/AVFoundation composition documented | Refusal legs gated everywhere (ENODEV/ENOTSUP, clean out-params); capture/AHB legs run where hardware exists | **PASS** (legs declared) |

## 3. The Gates (160+ checks, all green in the shard)

| Suite | Checks | Legs | Highlights |
| :--- | :--- | :--- | :--- |
| DB (dmabuf) | 42 | -O2 + ASAN | dialect byte-identity vs shm_ring; aliasing substrate; fork torture; fd ownership |
| XE/XD (extmem) | 24 | no-ICD + ICD + ASAN | fd-road zero-copy (window advances); alignment gate (+128 refused, page-base control); alias verification; session-intact-after-refusal |
| X (xdp) | 38 | -O2 + ASAN | state machine, telescoping `drops = lastSeq − freshClaims` exact; uring delegation with real traffic; refusal ladder |
| T (tensor) | 42 | -O2 + ASAN | 11 dtypes, 11-way refusal ladder, codec bit-exactness, no-burn fit refusal, rank-4 |
| V (camera) | 10 | -O2 + ASAN | seam honesty + composition identities |
| H (capstone) | 4 | ICD | the full pipeline, one allocation end-to-end |
| shard meta | — | — | .spv byte-identical rebuilds (sha-verified); **kernel freeze: zero diffs on weft.c/weft.h since db24254**; no-ICD leg = refusal honesty as a CI gate |

## 4. Honesty Ledger

- **MEASURED here**: every refusal leg; the fd-road wrap + zero-copy GPU consumption (structure — NOT discrete-GPU performance; llvmpipe is software, the logs say so); the tensor/DSP capstone; the uring delegation; the DB substrate proofs.
- **CI-GATED**: the ICD-leg proofs (shard installs lavapipe exactly like gpu-native); .spv determinism; kernel freeze.
- **DECLARED**: AF_XDP LIVE (no CONFIG_XDP_SOCKETS in CI kernels — rig + gate binary shipped, exit-3 asserted); heap ioctls; V4L2 capture; AHB (NDK leg); Metal (apple CI leg); WebGPU's one forced copy (platform limit, protocol identical).
- **HARDWARE-DEFERRED**: host-pointer wrap positive leg (aliasing drivers: AMD/Intel/NVIDIA/GBM — llvmpipe's non-aliasing behavior is the discovery that made verification load-bearing); real-GPU dma-buf export/import; discrete-GPU fence pacing.

## 5. Files

**New modules**: `core/c/{weft_dmabuf,xdp_rx,weft_tensor,weft_hw_v4l2,weft_hw_ahb}.{h,c}` · `core/c/{gpu_extmem_test,xdp_test,weft_tensor_test,weft_hw_camera_test,xdp_live_gate,hetero_probe}.c`
**Extended**: `core/c/gpu_ring.{h,c}` (wrap constructors + alias verification + dmabuf export + import_kind), `core/c/vk_min.h` (host-import/props2 ABI, size-asserted)
**Shaders**: `probes/compute/{tensor_reduce,fft_radix2}.{comp,wgsl}` + committed `.spv`
**Apple**: `Sources/WeftSwiftUI/WeftMetalZeroCopy.swift`
**CI/docs**: `rfcs/0016-heterogeneous-zerocopy-mesh.md` · `ci/scripts/run_heterogeneous_shard.sh` (+ extreme-matrix registration) · `ci/scripts/xdp_loopback_proof.sh` · `litmus/evidence/heterogeneous/*` (this run) · this report

## 6. Open (follow-up series candidates)

NPU/DSP runtime adapters (QNN/CoreML/Hexagon — WTS1 needs no changes, the seams are the runtimes' output contracts) · async fence-scoped dispatch evidence on real GPUs · the Windows/DEXC dialect · heap-backed sessions on Android (per-board heap inventory) · a browser WebGPU demo mode consuming WGSL mirrors over SAB rings.
