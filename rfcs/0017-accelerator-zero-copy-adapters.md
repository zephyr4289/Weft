# RFC-0017: Accelerator Zero-Copy Adapters (weft-tensor, Pillar 2)

- **Status**: Accepted (driver-layer pattern, tools scope)
- **Scope**: `tools/weft-tensor/**` — core/c untouched
- **Depends on**: RFC-0004 (fan-out ring), RFC-0016 §2/§5 (wrap
  constructors, WTS1), Engineer 1's DMA tensor ring (the view ABI seam)
- **Evidence**: `litmus/evidence/accelerators/` (AC-series, 97 gates,
  two roads), D-29 report

## 1. Why exists

RFC-0016 removed the copy seams between a Weft ring and the GPU, the
NIC, and cameras — one allocation, many consumers. What still copies is
everything AFTER the ring: AI runtimes (ONNX Runtime, TensorRT,
llama.cpp) re-frame producer bytes into their own tensor objects (3-10 ms
per frame on realistic pipelines), and every accelerator path re-packs
before dispatch. The lead's Pillar-2 mandate: the buffer the producer
made IS the buffer the accelerator consumes — Metal/ANE wraps,
Vulkan dma-buf compute, zero-copy ONNX values, ggml buffer types — with
< 500 us accelerator dispatch overhead and < 1.0 ms end-to-end.

## 2. The view ABI seam (§2)

`weft_tensor_view_t` (128 B, cache-line aligned, ABI v1) is the ONE
contract every adapter codes against: dtype (the frozen WTS1 numeric
dialect), rank/dims, strides (in ELEMENTS), the byte span, the
`schema_id` (weftc Pillar-1 layout hash), and the producer epoch. The
ladder (`weft_tensor_view_validate`) is refusal-first — every anomaly is
a named negative code; `weft_tensor_view_gpu_ready` is Law 2's gate
(LE + 16-byte float-vector alignment) that every GPU/NPU adapter calls
before binding. Engineer 1's ring engine owns the struct once it lands;
the ABI version + static layout asserts make any drift a compile error,
never a silent mismatch.

## 3. The Vulkan bridge (§3)

Two roads over one pooled compute kit:

- **Ring road** — wrap a WFSH session through the Series-10 constructors
  (`weft_gpu_wrap_host/_dmabuf`, alias-verified by the substrate) or
  adopt a native `weft_gpu_ring` session; the session span's OWN buffer
  is the SSBO at slot-granularity byte ranges.
- **Foreign road** — raw dma-buf fds (V4L2 EXPBUF / AHB / peer GPUs)
  through a standalone dlopen'd bootstrap (extension-gated device,
  `VkImportMemoryFdInfoKHR`), followed by the ALIAS CANARY: a canary
  frame written through the CPU map, preprocessed through the GPU
  binding, and compared BIT-EXACT against the scalar oracle. A device
  that accepts the import but backs it with fresh memory (llvmpipe's
  host-road behavior — the Series-10 discovery) is REFUSED.

The compute kit is Law-1-pooled: descriptors updated (not created), one
command buffer re-recorded, one fence reset — zero creates on the hot
path. The frozen preprocess kernel (`weft_preprocess.{comp,spv}`) is the
one-multiply normalize contract: `out[i] = (f32)src[i] * scale` — one
exact conversion + one rounded multiply, no add to fuse into an FMA, so
scalar == SIMD == GPU is an ==-gate (AC-K7, AC-X3).

## 4. The Metal adapter (§4)

Apple unified memory makes zero-copy a WRAP dance: `bytesNoCopy`
MTLBuffer (Metal compute), `CVPixelBufferCreateWithBytes` (CoreML /
Vision / ANE model input, release-callback keyed), and the REVERSE
OWNERSHIP road — the ring allocates its slots inside an `IOSurface`'s
bytes, so ANE consumes ring memory through the surface handle. The
geometry core (rows/bytesPerRow/64-alignment law, the two v1 wrap
roads) is portable and gated everywhere; the device roads are the
apple CI leg (the WeftMetalZeroCopy precedent); on other platforms the
weak stubs return honest UNSUPPORTED refusals. Discrete-GPU Macs refuse
rather than silently stage (Law 4).

## 5. The ONNX bridge (§5)

dlopen'd `libonnxruntime` (no link-time dependency), API table = the
VERIFIED v1.16.3 mirror (`weft_ort_abi.h` — generated from the vendored
header, every used field offsetof-asserted at its table index; older
runtimes refuse at load). The zero-copy wrap is
`CreateTensorWithDataAsOrtValue` over the view's OWN pointer — gated by
pointer identity through `GetTensorMutableData` (mock and real legs).
The pooled session pins Law 1: `DisableMemPattern` +
`DisableCpuMemArena` + fixed intra-op threads; ONE IoBinding; input and
output values pre-wrapped over ring slots (the EP writes logits
DIRECTLY into Weft memory); `RunWithBinding` is the only table call on
the hot path — the AC-O13 gate asserts the malloc window does not move.

## 6. The ggml bridge (§6)

Two roads over one plan layer (pure, gated everywhere): the dtype map
(unsigned WTS1 dtypes have no ggml type — the NAMED refusal), the ggml
geometry order (ne[0] innermost — the reverse of the view's dims), the
zero-alloc first-fit placement planner (64-byte law, epoch-rewindable),
and Whisper-style audio windowing (partial tails dropped or labeled,
never secretly padded). The FORMAL road constructs a
`WEFT_DMA` ggml_backend_buffer_type over registered ring spans (pinned
to the b4312 public-struct ABI — the 2025 split made the structs
opaque, and the bridge detects and reports that era honestly; a
`ggml_nbytes` runtime probe refuses any layout disagreement). The
DATA-POINTER road (tensor->data inside the span) works across every
ggml generation.

## 7. The bench (§7)

End-to-end, every stage labeled: `[SIM-DMA]` produce → `[ZERO-COPY]`/
`[FALLBACK-COPY]` preprocess → `[ZERO-COPY]`/`[SIM-EP]` execute →
`[WTS1]` write-back. Gates: G1 pipeline < 1.0 ms (the SIMD floor on any
host; the GPU road's number carries a SOFTWARE-ICD label on lavapipe —
structural proof here, performance hardware-deferred), G2 dispatch
< 500 us (measured on a minimal dispatch: our API overhead, not device
throughput), G3 bit-exactness (prep + execute), G4 the Law-1 window
(strict on CPU roads; on software ICDs the driver's internal
allocations are REPORTED, never laundered to zero).

## 8. Laws

1. **Zero dynamic allocations on the inference hot path** — everything
   pooled at setup; the malloc-audit window gates it (AC-O13, G4).
2. **Strict endianness & alignment** — the view ladder + gpu_ready gate
   (16-byte float vectors, 64-byte rows); misaligned/BE views route to
   the labeled SIMD copy, never a silent bind.
3. **Kernel freeze & modularity** — frozen kernels are byte-committed
   (CI rebuild-diff) + frozen-ID checked at pipeline build; adapters
   are modular under `tools/weft-tensor/backends/`; core/c untouched.
4. **Honest fallbacks** — every refusal named; alias canaries are
   load-bearing; software-ICD execution and driver-internal allocations
   are labeled, never presented as hardware evidence.

## 9. Evidence

- `ac-series-icd.log` — 97 PASS, 0 FAIL (lavapipe: fd-road import,
  bit-exact GPU preprocess, alias canary, definite host-road verdict)
- `ac-series-noicd.log` — the refusal ladder as a gate
- `ac-asan-freeze.log` — ASAN-clean + .spv byte-identity
- DECLARED (apple CI leg): Metal device roads; real-lib ORT/ggml legs
  (mock + probe batteries carry the logic here)
