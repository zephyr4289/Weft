# RFC-0016: The Heterogeneous Zero-Copy Mesh — DMA-BUF, External-Memory GPU Wraps, AF_XDP Ingestion & the Tensor Stream Bridge

- **Status:** Proposed (driver layer; Series 10)
- **Layer:** `core/c/weft_dmabuf`, `core/c/gpu_ring` (wrap constructors), `core/c/xdp_rx`, `core/c/weft_tensor`, `core/c/weft_hw_{v4l2,ahb}`, `probes/compute/{tensor_reduce,fft_radix2}`, `Sources/WeftSwiftUI/WeftMetalZeroCopy.swift`
- **Depends on:** RFC-0004 (fan-out ring), RFC-0011 (WFSH sessions), RFC-0013 (GPU streaming + fd bridge), RFC-0012 (kernel-bypass ladder), the device-tier F16 codec (Series 9)
- **Laws kept:** 1 (bounded paths), 2 (zero hot-path allocs), 3 (kernel byte-frozen), 4 (honest fallbacks) — per-module notes inline

## 0. Why

Weft's ring is already cross-thread (RFC-0004), cross-process (RFC-0011 WFSH), and cross-GPU (RFC-0003/0013). The remaining seams are **cross-device**: a camera pipe, a NIC running kernel-bypass, a hardware codec, an NPU, another GPU — every producer and consumer of dense realtime data that is not a CPU thread. On Linux, and on every mobile/Apple platform in a different dialect, the currency at that seam is a **shared buffer handle**: dma-buf on Linux/Android, IOSurface-backed MTLBuffer on Apple. And on the producer side, the fastest-growing class of producers is **AI inference runtimes** (llama.cpp, ONNX, Whisper) whose outputs are dense typed tensors.

This RFC closes both gaps with one stance: **the buffer the producer made IS the buffer the mesh consumes.** One allocation, aliased by every device in the pipeline; zero intermediate copies by construction, because there is nothing to copy through.

```
                 ┌──────────────── the ONE allocation (dma-buf / session pages) ────────────────┐
                 │                                                                               │
  CPU publisher ─┤ fan-out ring (RFC-0004) over the mmap — begin/fill/publish, I1–I8 unchanged   │
                 │                                                                               │
  GPU compute ───┤ weft_gpu_wrap_dmabuf / wrap_host — session-span VkBuffer over the SAME pages   │
                 │   tensor_reduce.spv · fft_radix2.spv — live-word consumption, no staging      │
                 │                                                                               │
  NIC (AF_XDP) ──┤ weft_xdp_rx — UMEM = the session pages; NIC DMA lands IN the ring slot        │
                 │   (the pre-bracketed fill ring keeps the seqlock contract intact)             │
                 │                                                                               │
  Camera (V4L2) ─┤ weft_hw_v4l2 — VIDIOC_EXPBUF dma-buf fd; compositor/GPU consume the frame    │
  Android cam ───┤ weft_hw_ahb — AHardwareBuffer native handle -> dma-buf fd                     │
  Apple ─────────┤ WeftMetalZeroCopy — MTLBuffer(bytesNoCopy:) .storageModeShared over the ring  │
                 │                                                                               │
  AI runtimes ───┤ weft_tensor (WTS1) — embeddings/tokens/PCM as ORDINARY frames, any transport   │
                 └───────────────────────────────────────────────────────────────────────────────┘
```

## 1. Scope & non-goals

**In scope:** allocation/import of ring storage as device-shareable memory; GPU consumption of *existing* sessions; NIC-DMA ingestion under the ring's invariants; an in-band tensor frame format + publisher/parse paths; camera/compositor seams; the evidence gates that keep every claim honest.

**Non-goals (explicit):**
- No kernel changes — `weft.c`/`weft.h` byte-frozen (Law 3; the kernel-freeze gate runs in this series' shard).
- No renderer policy — the render bridge (doc-007) stays mechanism; so does every seam here.
- No camera daemon, no BPF program shipped in-tree — the LIVE-mode rig (`ci/scripts/xdp_loopback_proof.sh`) documents the privileged deployment.
- No performance claims beyond what the evidence logs carry, with honesty labels (MEASURED / CI-GATED / DECLARED / HARDWARE-DEFERRED).

## 2. GPU external-memory wraps (§2 of the mandate)

### 2.1 The two roads

`weft_gpu_wrap_host` (VK_EXT_external_memory_host): the application's own page-aligned session — a WFSH shm mapping, a memfd, any mmap'd shared region — is imported as `VkDeviceMemory` (`VkImportMemoryHostPointerInfoEXT`, `HOST_ALLOCATION_BIT`), a session-span `VkBuffer` is bound over it, and the ordinary gpu_stream kit consumes the ring through the **same physical pages**. `minImportedHostPointerAlignment` is honored; malloc'd fan-out rings are refused (shm/dmabuf/mmap backing is the documented road — the header's wire layout is unchanged, only the backing store differs).

`weft_gpu_wrap_dmabuf` (VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf): the same construction over a dma-buf fd — weft_dmabuf's heap allocation, a V4L2 EXPBUF, an AHardwareBuffer's native handle, or another process's export. A dup is imported (the caller's fd is never consumed); the session mmaps its own CPU view and unmaps it at destroy. `weft_gpu_export_dmabuf_fd` mints dma-buf fds for the reverse direction.

### 2.2 Alias verification — the load-bearing discovery

**MEASURED (llvmpipe 25.0.7, the CI software ICD):** the driver *accepts* host-pointer imports (`vkAllocateMemory` returns SUCCESS) but backs them with **fresh memory** — `vkMapMemory` returns a different pointer whose contents are zeros. A wrap that trusted the return code would silently consume an empty ring — exactly the silent degradation Law 4 forbids.

Every wrap therefore **verifies aliasing** before handing the session back: a canary written through the CPU view must round-trip through the imported allocation's mapped view (scratch word: header offset 28, `creator_pid` — advisory in both WFSH dialects, validated by neither, restored byte-identical). A device that does not alias is refused; the caller falls back to `weft_gpu_create`'s HOST_VISIBLE road. On llvmpipe this refusal is a **gated, asserted behavior** (XE-series); the host-pointer positive leg is HARDWARE-DEFERRED exactly as RFC-0003 defers discrete-GPU numbers. The fd road aliases on llvmpipe (it mmaps the fd) and is **MEASURED end-to-end** (§5.3).

## 3. DMA-BUF ring backing (§3 of the mandate)

`weft_dmabuf` allocates session storage from `/dev/dma_heap/system` (hand-rolled dma-heap uapi, the uring_rx no-dependency discipline): the WFSH header is written in shm_ring's byte-identical dialect (DB4 gate), the ring ctrl zero-initialized to the fresh-ring invariants, and the fd is handed to any consumer — Vulkan import, V4L2 peer, AF_XDP UMEM, another process.

**Why the system heap only:** the ring's correctness rests on coherent shared-memory atomics. `system-uncached` heaps give non-coherent CPU mappings (explicit `DMA_BUF_IOCTL_SYNC` cache maintenance around every touch — incompatible with lock-free seqlock stamps). The probe records every heap node found and **refuses** the uncached ones for ring backing — an honest capability boundary, not a silent performance cliff. `weft_dmabuf_sync` is still exported for *foreign* uncached buffers (camera frames the CPU must touch).

Foreign imports validate with `>=` page-rounded semantics (heap allocations exceed the exact session span) and map honestly as raw (non-session) buffers when no WFSH header is present — the camera-frame road. The DB-series gates prove the mmap-aliasing substrate (one allocation, two mappings, live fan-out traffic, fork torture) on every host; the heap-ioctl leg runs where heaps exist and its refusal is asserted where they do not.

## 4. AF_XDP ingestion (§4 of the mandate)

### 4.1 The pre-bracketed fill ring

The uring rung made the kernel the bracket-filler; the XDP rung makes the **NIC** the filler: session pages are registered as UMEM (`XDP_UMEM_REG` over the page-aligned session span, **unaligned chunk mode**, kernel ≥ 5.7 — slot payload *k* lives at session offset `80+8M+k·pb`, never chunk-aligned for useful geometry, and the low-48-bit address encoding is exactly `include/uapi/linux/if_xdp.h`'s).

The protocol that keeps I1–I8 intact: the buffer is handed to the driver **only while the bracket is open** —

1. `publish(frame s)` — slot `(s−1)%M` closes, live for readers;
2. `begin()` — slot `s%M` invalidated (SeqCst store + fence: property P1);
3. fill ring posts slot `s%M`'s payload offset — the NIC may DMA from now on;
4. NIC DMAs the packet into the slot (readers mid-DMA see `slotSeq==0`: the frozen skip path);
5. rx descriptor arrives; the state machine verifies the address **is** the in-flight slot;
6. `publish()` — stamp + latestSeq flip (Release).

Depth is ONE by construction (the frozen `publish()` stamps the most recent `begin()` pair only — the same soundness finding RFC-0004's depth analysis reached for uring_rx). Short packets are tail-zeroed (padded), oversize packets keep the head (truncated), misordered descriptors are **refused and counted** — every anomaly in the honesty record.

### 4.2 The ladder

`NONE` (no `CONFIG_XDP_SOCKETS` — `socket(AF_XDP)` → EAFNOSUPPORT; **the CI/sandbox state**) → attach **delegates to uring_rx's own ladder** (SYSCALL/RING/FIXED — a drop-in upgrade, never a regression); `SETUP` (xsk + UMEM + rings functional) → `LIVE` (descriptors under traffic; needs an XDP program — the privileged rig `ci/scripts/xdp_loopback_proof.sh` + `xdp-live-gate` drive it end-to-end, DECLARED on AF_XDP-less kernels with an honest exit-3). bind() is deliberately the caller's (ifindex/queue is deployment policy).

## 5. The tensor stream bridge (§5 of the mandate)

### 5.1 WTS1

A 32-byte little-endian header + payload, carried as an **ordinary fan-out frame**: magic `WTS1`, version/dtype/rank/flags, elem_count, payload_words, dims[4]. Invariants: `prod(dims[0..rank)) == elem_count`; `payload_words == ceil(elem_count·elem_size/4)`; dims ≥ rank zero; flags zero (unknown bits reject — the house version discipline). Eleven dtypes; **F16 uses the weft_f16_codec dialect** so quantization is tree-wide identical.

### 5.2 The adapter seams (at most ONE copy per frame)

| Runtime | Output | Seam |
|---|---|---|
| llama.cpp / GGML | token stream | `weft_tensor_publish_tokens` (U32 ids, zero transforms) |
| llama.cpp / ONNX | embeddings | `weft_tensor_publish_f16` (F32→F16 via the codec, one pass) |
| Whisper | PCM chunks | `weft_tensor_publish_audio_f32` |
| any engine | raw tensor | `frame_begin` + `fill_bytes` (one copy) |
| arena-allocating engines | tensor in-slot | the raw cursor road (**zero copies** — GGML custom allocators; the documented integration point) |

Oversize geometry is refused **before** `begin()` — no seq burned (unlike uring_rx's kernel path, the size is visible first; Law 4). Parse validates everything and guesses nothing.

### 5.3 Fused compute passes (the capstone)

`tensor_reduce.{comp,wgsl}` — parses the WTS1 F16 header GPU-side from the **latest live slot** and reduces the embedding (argmax, max, sum) in a deterministic 64-lane partition + fixed combine tree: **adds-only f32 (FMA-proof)** + `unpackHalf2x16` (IEEE-exact, bit-identical to `weft_f16_to_f32`) ⇒ the CPU reference mirroring the partition produces **BIT-IDENTICAL results** — the blend_q12 discipline applied to GPU reductions.

`fft_radix2.{comp,wgsl}` — a 512-point radix-2 DIT FFT over a WTS1 F32 waveform frame with **honest numerics**: twiddle ULPs differ across libm, so the gates are peak-bin-EXACT + magnitude within 1e-3 of a double-precision DFT + an in-shader Parseval self-check (declared tolerances — the rasterize_frame honesty split).

`hetero_probe` — the capstone gate: memfd session → `wrap_dmabuf` → CPU publishes 100 WTS1 frames → GPU reduces through the imported fd → burst-2 **liveness** (the window advances) → two-tone FFT. **MEASURED on lavapipe:** bit-exact reductions both bursts, peak bin 37 exact, magnitude rel 2.63e-9, Parseval 2.06e-7.

## 6. Camera/compositor seams (§6)

**V4L2** (`weft_hw_v4l2`): QUERYCAP/G_FMT/REQBUFS(MMAP)/QBUF/DQBUF/EXPBUF — the frame exports as a dma-buf fd and composes with the mesh (`weft_dmabuf_import_fd` raw road / `weft_gpu_wrap_dmabuf` GPU road). Hand-rolled LP64 videodev2 ABI, declared for 32-bit. **AHardwareBuffer** (`weft_hw_ahb`): `AHardwareBuffer_getNativeHandle` → dma-buf fd (the gralloc contract) for Camera2 ImageReader buffers or fresh RGBA8888 allocations; non-Android builds compile ENOTSUP stubs; the NDK leg (android/weft-core) links libandroid. **Apple** (`WeftMetalZeroCopy.swift`): `MTLBuffer(bytesNoCopy:)` `.storageModeShared` over the ring — the VK_EXT_external_memory_host equivalent; **refuses** discrete-only Macs rather than silently staging. **WebGPU:** the WGSL mirrors carry the protocol (same result words, same algorithms); the browser's one forced copy (SAB → storage buffer) is a platform limit, declared here — the shader-side contract is identical, so a demos/web integration changes zero protocol bytes.

**Camera2/AVFoundation composition (documented seams):** Android — ImageReader(USAGE_GPU_SAMPLED) → AHardwareBuffer → fd → mesh. Apple — CVPixelBufferPool + IOSurface → MTLBuffer zero-copy. Both are the same stance one layer up.

## 7. Evidence & gates

| Gate | Series | What it proves | Runs where |
|---|---|---|---|
| dmabuf-test | DB (42) | heap/refusal, dialect identity, aliasing substrate, fork torture | every host (± heap leg) |
| gpu-extmem-test | XE/XD (24) | wrap roads: refusal ladder + fd-road zero-copy proof (CPU publish → GPU live consume → window advances) + alias verification | every host; vulkan legs with an ICD (CI: lavapipe) |
| xdp-test | X (38) | the pre-bracketed state machine (synthetic descriptors, telescoping exact), 500 real datagrams through the uring delegation, refusal ladder | every host; xsk legs on CONFIG_XDP_SOCKETS hosts |
| tensor-test | T (42) | WTS1 roundtrips, refusal ladder, F16 bit-exactness, streaming + rank-4, cross-surface | every host |
| camera-test | V (10) | seam honesty (ENODEV/ENOTSUP), composition identities | every host; legs where hardware exists |
| hetero-probe | H (4) | THE pipeline: wrap → tensor publish → GPU reduce bit-exact → liveness → FFT | with an ICD; declared exit-0 otherwise |
| xdp-live-gate | LIVE rig | end-to-end NIC DMA → ring slot → descriptor → publish | privileged CONFIG_XDP_SOCKETS hosts |

Shard: `ci/scripts/run_heterogeneous_shard.sh` (registered in the extreme matrix) — builds every gate in `-O2` + ASAN, runs the vulkan legs with and without the ICD (both must pass: refusal honesty *and* the positive proof), rebuilds the `.spv` byte-identical, checks the kernel freeze (weft.c/weft.h sha256 vs the frozen pair), and regenerates the evidence logs.

**Honesty labels:** llvmpipe is a *software* Vulkan stack — the wrap proofs are structural (loader, instance, import, binding, dispatch, live-word consumption), NOT discrete-GPU performance evidence; the logs say so. The host-pointer positive leg, real-GPU dma-buf exports, heap ioctls, AF_XDP data path, V4L2 capture, AHB and Metal are HARDWARE-DEFERRED with refusal gates asserted in CI — the RFC-0003 stance, applied uniformly.

## 8. Open questions

1. **Fence-scoped async dispatch over wraps** — the Series-9 4-deep pipeline (issue #17-5) composes with wrapped sessions unchanged; its evidence on real GPUs (where the fence actually paces a queue) is hardware-deferred.
2. **Unaligned-mode offset bits** — the decode strips high bits per the system's `if_xdp.h`; a driver that populates offset bits within the flag encoding is a LIVE-rig discovery (the rig's misorder counter would catch it first).
3. **NPU/DSP runtimes** (QNN, CoreML, Hexagon): the WTS1 format needs no changes (dense typed tensors); the adapter seam is each runtime's output-buffer contract — expected as follow-up series with on-device evidence.
4. **Windows/DEXC** — `ID3D12Fence` + shared handles are the platform's dialect; the session-header and state-machine layers port unchanged, the import layer is new plumbing.

## 9. References

- The mandate: the lead's Series-10 briefing (heterogeneous compute, zero-copy GPU/NPU, kernel-bypass ingestion).
- RFC-0004 (ring + invariants I1–I8, the depth-1 analysis), RFC-0011 (WFSH), RFC-0003/0013 (GPU residency + streaming kit + fd bridge), RFC-0012 (the uring ladder this series extends), the F16 codec + device tiers (Series 9).
- Evidence: `litmus/evidence/heterogeneous/` (this series); `litmus/evidence/gpu-ring/` (the Series-7/8 base).
- Report: `reports/D-22-REPORT.md` (the scoreboard).
