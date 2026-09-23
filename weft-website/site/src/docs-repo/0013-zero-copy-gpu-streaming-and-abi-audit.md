---
RFC: 0013
Title: Zero-Copy GPU Streaming — Bind Kit, Storage/Texel/Image Roads, the fd Bridge, and the Vulkan ABI Audit
Status: Draft (implementation + first-ever executable dispatch evidence attached; ratification pending)
Authors: Weft Core Team (Engineer 2 — Hardware Acceleration & IPC)
Created: 2026-09-18
Supersedes / Superseded-by: Extends RFC 0003 (GPU-resident rings), RFC-0011 (WFSH shm sessions)
---

# RFC 0013 — Zero-Copy GPU Streaming & the ABI Audit

## Summary

Three things, one theme — taking RFC-0003's zero-copy claim from "one
validated frame behind a hand-rolled probe" to a production-shaped
streaming layer, and in the process finding (and fixing) that the
incumbent Vulkan ABI shim had been wrong in four distinct defect classes
the whole time:

1. **The ABI audit**: `vk_min.h`/`gpu_probe.c` carried 12 wrong constants,
   2 struct-layout defects, and 1 call-site bug (pipeline count 0) — none
   of it ever exercised, because the dispatch leg had never actually
   executed anywhere (see Motivation). All fixed; every constant and
   struct size is now compile-time-asserted against the real Khronos
   headers when installed (`vk_abi_check`), and the whole proof ladder
   now runs against a software ICD whose JIT actually executes
   (SwiftShader) instead of silently stalling at llvmpipe's reservation.
2. **The streaming layer**: a zero-copy bind kit (`gpu_stream.{h,c}`) with
   a frozen binding convention (ring SSBO / result / storage image /
   texel view), plus two new consumers — `stream_frames.comp` (the
   whole-ring window, advancing across dispatches) and
   `rasterize_frame.comp` (frames → self-verified rgba8ui texture, no
   host readback) — and MSL/WGSL mirrors of the consumer contract for the
   Apple and WebGPU roads.
3. **The fd bridge**: `weft_gpu_create_ex(EXPORTABLE_FD)` /
   `weft_gpu_export_fd` / `weft_gpu_import_fd` — cross-process zero-copy
   sharing of the device allocation via `VK_KHR_external_memory_fd`
   (opaque-fd; dma-buf when the ICD offers the EXT), with the WFSH shm
   session (RFC-0011) as the documented fallback when the extension is
   absent.

**Executable result** (evidence log, verbatim): `ALL APPLICABLE PROOFS
PASSED` — the RFC-0003 zero-copy consumer proof executed END-TO-END for
the first time in the repo's history ("GPU validated 1000 live frames
with no staging copy"), the whole-ring window advanced 256→512 with
fingerprints matching CPU arithmetic exactly, 64/64 texture pixels were
written and read back in-shader, and a pristine exec'd process imported
the producer's fd and validated its frames GPU-side.

## Motivation

RFC-0003's spike proved the ring CAN live in GPU-dereferenceable memory,
and `gpu_probe.c` was written to prove a shader consumes it. But every
evidence run — sandbox AND the `gpu-native` CI shard, 100+ logged runs —
exited at the allocation leg (exit 4): llvmpipe's LLVM JIT reserves ~94 TiB
of address space for shader codegen, and every evidence kernel refuses
the reservation. The dispatch leg, and therefore the ENTIRE
descriptor/pipeline side of `vk_min.h`, had never executed once. The
lead's Series-8 directive asked for production-grade zero-copy pipelines;
the honest first step was to make the existing proof actually run — which
is where the audit fell out: against a SwiftShader ICD (Subzero JIT, no
huge reservations), the pre-audit probe aborted with

```
libVulkan.cpp:2313 WARNING: UNSUPPORTED: pCreateInfo->flags 0x00001544
```

`0x1544 = 5444` — the exact byte size of `validate_frame.spv`. The ICD
was reading our `codeSize` as `flags`, because `VkShaderModuleCreateInfo_`
lacked the `flags` field. Full audit (verified line-by-line against
Khronos Vulkan-Headers r362; `vk_abi_check.c` compiles against the real
headers in CI):

| class | defect | effect if ever executed |
|---|---|---|
| constants | 12 wrong values (SUBMIT_INFO 23→4, DESCRIPTOR_TYPE_STORAGE_BUFFER 3→7, BUFFER_USAGE_STORAGE_BUFFER 0x8→0x20, ...) | rejected calls / wrong descriptor semantics |
| struct layout | `VkShaderModuleCreateInfo_` missing `flags` (codeSize read as flags) | shader module creation garbage — **experimentally confirmed** |
| struct layout | `VkPipelineShaderStageCreateInfo_` missing `flags` (stage read as 0) | wrong stage / silent garbage |
| struct size | `VkPhysicalDeviceProperties_` a 296-byte view of an 824-byte struct | 528-byte stack smash (−O0: segfault; −O2: silent) |
| call site | `vkCreateComputePipelines(..., createInfoCount=0, ...)` | pipeline NEVER created; NULL bound at dispatch |

The one exercised defect (buffer usage bits) was harmless only because
nothing downstream ever consumed the buffer as the intended type.

## Guide-level explanation

A consumer builds a pipeline against a live ring in five lines:

```c
weft_gpu_ring_t* g; weft_gpu_create(&g, payload, slots);
void* spv = ...;  // probes/compute/*.spv — the consumer's shader
weft_gpu_stream_t* s;
weft_gpu_stream_init(&s, g, spv, spv_len, img_w, img_h, want_texel);
/* publish frames CPU-side through the ordinary fan-out API */
weft_gpu_stream_dispatch(s, push, sizeof(push), gx, gy, gz);
const uint32_t* result = weft_gpu_stream_result(s);
```

The binding convention is frozen: 0 = ring SSBO (the session span's own
VkBuffer — zero copy), 1 = result (8 words, HOST_COHERENT, mapped),
2 = rgba8ui storage image (optional), 3 = R32_UINT texel buffer view over
the span (optional — the "direct texture" road; the ring buffer is created
with texel usage bits, with a STORAGE-only retry fallback for ICDs that
refuse them, declared).

Cross-process: the producer creates with `EXPORTABLE_FD`, publishes,
exports; the consumer (a separate process — see the honesty note on fork)
imports and dispatches against the SAME pages. When the ICD lacks the
extension, `export_fd` returns -1 and the WFSH shm session is the
documented transport — the CPU-side protocol is identical either way.

## Reference-level specification

- `gpu_stream.{h,c}` — the kit (per-call resolved device procs through the
  ring's loader discipline; pipeline count=1 with NULL-handle rejection —
  the audit's defect class #5 defended structurally; image tiling fallback
  OPTIMAL→LINEAR, declared; first-dispatch UNDEFINED→GENERAL barrier;
  env-gated dispatch trace `WEFT_GPU_STREAM_TRACE`).
- `gpu_ring.{h,c}` — `weft_gpu_create_ex`, `weft_gpu_export_fd`,
  `weft_gpu_import_fd` (import validates the WFSH header against the
  caller's geometry before returning the ring — a foreign session is a
  hard error, not a fallback); instance/physical-device accessors
  (`weft_gpu_vk_instance`, `weft_gpu_vk_physical_device`,
  `weft_gpu_vk_instance_proc`) so consumers never re-create instances;
  `vkEnumerateDeviceExtensionProperties`-probed extension enablement;
  the device is now created from the physical device the family search
  actually selected (pre-Series-8 code used `phys[0]` regardless — fixed).
- `vk_min.h` — audited, corrected, and pinned: every struct
  `_Static_assert`ed to its r362 size; the audit table lives in the header
  comment block with the SwiftShader confirmation story.
- `vk_abi_check.c` — the compile-time gate against real Khronos headers
  (`-DHAVE_VULKAN_HEADERS`); self-skips declared when absent.
- `probes/compute/stream_frames.comp` — whole-ring consumer; result:
  (mismatches, latestSeq, window XOR-fingerprint, window width, magic).
  Two dispatches prove the window ADVANCES (the fingerprint changes and
  matches CPU arithmetic) — sustained streaming, not a single read.
- `probes/compute/rasterize_frame.comp` — render road; pixel (x, y) ←
  payload word y*w+x of the latest frame as rgba8ui; each pixel is
  written and READ BACK in-shader (`imageStore` + `memoryBarrier` +
  `imageLoad`, coherent within the invocation) and compared — the texture
  is verified without a single host round-trip.
- `probes/metal/stream_frames.metal` + `probes/webgpu/stream_frames.wgsl`
  (+ READMEs) — the consumer contract mirrored for Apple unified memory
  (`newBufferWithBytesNoCopy` wrap, no copy) and WebGPU (the honest
  one-copy boundary: WASM cannot alias GPU memory; `queue.writeBuffer`
  from the SAB ring view is the structural minimum).
- `gpu_stream_probe.c` — the gates: STREAM, RASTER, FD (--fd), and the
  guardrail (kit cleanly refuses non-Vulkan backends — exit code contract
  mirrors gpu-probe's).

## Boundary of the claim (Law 4)

- **Execution environment**: all dispatch evidence is SwiftShader
  (Subzero JIT) — a real Vulkan loader + ICD stack executing shaders in
  software on the CPU. It is NOT discrete-GPU performance evidence and no
  performance numbers are quoted from it. Discrete-GPU numbers stay
  hardware-deferred exactly as RFC-0003 defers them. What this series
  removes is the STRUCTURAL gap: the proof ladder now executes, on
  hardware any CI runner can provide, where before it silently stalled.
- **llvmpipe note**: the sandbox and the CI gpu-native shard keep the
  lavapipe environment variable path; with the audit fixes the probe
  would also complete there on standard kernels — but every logged runner
  to date refuses the JIT reservation, which is WHY the SwiftShader leg
  exists. CI should install/probe both.
- **fork+Vulkan**: the fd bridge's consumer must be a pristine process
  (fork+exec). A fork-cloned child inheriting live Vulkan state is
  outside every ICD's contract (the probe's first FD implementation
  crashed exactly there — instructive, documented). The exec'd worker
  also required clearing `FD_CLOEXEC` on the exported fd (library-created
  fds default to it) — a real-world footgun now written down.
- **Metal/WGSL**: reference contracts only; compile-gated on Apple CI
  (proposed, optional), NOT executable-tested on the x86_64 host. The
  WebGPU road is honestly one-copy (platform primitive absent) — stated
  in the WGSL header and README, not worked around.
- **Texel road**: `vkCreateBufferView` over the span is implemented and
  the binding convention reserves slot 3; the executable in-tree consumers
  currently use SSBO + image bindings. The texel kernel is future work —
  the VIEW creation path is exercised (init accepts `want_texel`), the
  consumer isn't.

## Falsifiable claims (evidence attached)

`litmus/evidence/gpu-ring/series8-abi-audit-dispatch.log` and
`series8-zero-copy-streaming.log` (SwiftShader ICD, verbatim runs):

- ABI audit: the four defect classes, the SwiftShader abort signature
  (`flags 0x1544` = the spv byte size), and the first-ever end-to-end
  `zero-copy consumer PROVEN — GPU validated 1000 live frames with no
  staging copy` + geometry sweep (64/256/1024 B × 3–8 slots) + `-O0`
  build (the pre-audit stack smash reproduced and eliminated).
- STREAM: `mismatches=0 seq=256 xor=0x76e059cb` → `mismatches=0 seq=512
  xor=0x6d96082f` — window advanced, fingerprints EXACTLY equal to
  CPU-side arithmetic (independently recomputed).
- RASTER: `mismatches=0 pixels=64/64` with in-shader readback diagnostics
  (`load r/g/a = 83/236/255`, matching the CPU expectation).
- FD: producer exports `opaque-fd`; the exec'd worker imports and reports
  `mismatches=0 seq=512 word0=0x53ec96ee magic=0x54464557` — validated
  through the producer's allocation.
- Guardrail: `kit on cpu backend -> rc=-1 (clean refusal)`; no-ICD run
  exits 3 with the guardrail gate green.

## Alternatives considered

- **Fix constants only, keep llvmpipe**: the proof would still never run
  (reservation refused on every evidence host). The SwiftShader execution
  leg is the difference between "compiles" and "proves".
- **Real Vulkan SDK dependency**: rejected — the kernel's no-new-library
  bar holds at the DSO boundary (dlopen'd loader, hand-audited minimal
  ABI, now r362-pinned by static asserts + CI compile gate).
- **vkd3u/Anvil-style full header vendoring**: rejected — 27k lines of
  vendored headers vs a 430-line audited shim with a compile-time gate.
- **CUDA/ROCM interop for the bridge**: the opaque-fd contract composes
  with `cudaExternalMemory` / level-zero imports directly (both consume
  OPAQUE_FD/dma-buf); a native CUDA path would add a link-time dependency
  for zero protocol gain. Documented as the integration path, not built.

## Drawbacks

- The SwiftShader evidence leg needs the ICD present (CI: download or
  package; the evidence log names the exact dist). lavapipe remains the
  fallback environment.
- The fd bridge's producer-side is Linux-POSIX shaped (fds); Windows
  handles (VK_KHR_external_memory_win32) are the same protocol with
  different syscalls — declared open, not built.
- `gpu_stream` grows the driver-layer surface by one module; the binding
  convention is frozen so consumers stay source-stable.

## Open questions

- Apple CI: gate `xcrun metal -c` on `probes/metal/stream_frames.metal`
  (optional step in apple-packages)?
- The texel-buffer consumer kernel (binding 3 is reserved and
  view-creation is exercised; a `texel_scan.comp` would close it).
- Peer-to-peer (GPU↔GPU) via the same opaque-fd contract on multi-GPU
  hosts — hardware-deferred with the rest of the discrete-GPU numbers.
- WebGPU: a browser-harness probe (tools/browser-harness exists for SAB)
  driving `stream_frames.wgsl` with the one-copy writeBuffer path, as a
  declared-boundary demo rather than a zero-copy claim.

## Staff Decision

[EMPTY — implementation + executable evidence attached
(`litmus/evidence/gpu-ring/series8-*.log`); ratification pending]
