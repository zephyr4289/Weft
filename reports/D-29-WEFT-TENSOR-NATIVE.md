# D-29 REPORT: weft-tensor — the NPU/GPU Accelerator Pillar (Pillar 2 / RFC-0017)

- **Directive**: Pillar 2 — "NPU/GPU Acceleration & Zero-Copy AI Inference
  Engines, Project weft-tensor" (the lead's mission brief: Metal/ANE,
  Vulkan dma-buf, ONNX Runtime, llama.cpp/ggml zero-copy hooks; < 500 µs
  dispatch; < 1.0 ms end-to-end)
- **Status**: COMPLETED / PASS (4 accelerator backends + view ABI seam +
  E2E bench; 97 gates green on the ICD leg, 97 on the no-ICD refusal leg,
  ASAN-clean, kernel freeze byte-verified)
- **Date**: 2026-09-20
- **Environment**: `x86_64-sandbox` (gcc 14.2.0, AVX2) · Vulkan loader
  1.4.309 + **Mesa lavapipe 25.0.7 / llvmpipe (LLVM 19.1.7)** (software
  ICD — same stance as the gpu-native shard) · glslang 15.1 +
  spirv-tools 2025.1 (vendored debs) · no libonnxruntime / libggml
  (mock + probe batteries carry the logic; real-lib legs DECLARED)
- **Reference**: RFC-0017; builds on the Series-10 substrate (RFC-0016
  §2 wrap constructors, §5 WTS1) and Pillar 1's `schema_id` dialect

---

## 1. Executive Summary

Pillar 2 closes the last copy seam: **the buffer the producer made is
now the buffer every AI runtime consumes.** One 128-byte, ABI-versioned
tensor view (`weft_tensor_view_t`) describes a ring slot, a dma-buf, or
any span — and four adapters bind it without staging: Metal wraps the
bytes (MTLBuffer / CVPixelBuffer / IOSurface reverse-ownership), Vulkan
imports them (fd/host roads with a load-bearing alias canary), ONNX
Runtime wraps them (`CreateTensorWithDataAsOrtValue`, pointer-identity
gated, outputs written DIRECTLY into ring memory), and ggml places
tensors inside them (custom buffer type + a version-robust
data-pointer road). The frozen one-multiply preprocess kernel is
bit-exact across scalar, AVX2 and GPU — an ==-gate, not a tolerance —
and the end-to-end bench gates the mandate's budgets with every stage
carrying its road label.

The load-bearing discipline carried forward from Series 10: **alias
verification as a refusal, not a hope** — llvmpipe still accepts
host-pointer imports and backs them with fresh memory, and the canary
still catches it (AC-K9b: REFUSED, named, never silently consumed). The
fd road aliases and carries the full MEASURED zero-copy proof.

## 2. Mandate Scoreboard

| Deliverable | What landed | Measured (this sandbox) | Status |
| :--- | :--- | :--- | :---: |
| **A. Apple Silicon & ANE adapter** | `backends/metal/`: geometry core + MSL wrap trio (MTLBuffer bytesNoCopy, CVPixelBuffer for CoreML/Vision/ANE, IOSurface reverse-ownership span) + pooled compute with frozen `weft_preprocess.metal` | Portable core MEASURED (13 AC-M gates); device roads = apple CI leg, weak-stub refusals gated off-Apple; discrete-Mac no-staging stance documented | **PASS** (device legs declared) |
| **B. Linux/Android dma-buf Vulkan bridge** | `backends/vulkan/`: standalone dlopen bootstrap (ext-gated), fd + host imports, **alias canary**, pooled compute kit (N SSBO bindings at byte sub-ranges — a ring SLOT becomes an SSBO), frozen `weft_preprocess.{comp,spv}` (spirv-val VALID, rebuild byte-identical) | **fd road MEASURED on lavapipe**: import → GPU preprocess **bit-exact vs scalar oracle** (AC-K7), canary proves aliasing (AC-K8); host road refuses fresh-memory backing by NAME (AC-K9b — the Series-10 discovery reproduced); dispatch p50 **38 µs** (G2, budget 500 µs) | **PASS+** |
| **C. Zero-copy ONNX bridge** | `backends/onnx/`: dlopen'd runtime, **verified v1.16.3 API-table mirror** (generated from the vendored header, offsetof-asserted indices), zero-copy wrap (pointer-identity gate), pooled no-alloc `OrtRun` (mem-pattern + arena off, one IoBinding, output bound INTO ring memory), in-tree fixture model (onnx.checker-VALIDATED Identity graph) | 14 AC-O gates green via the mock table (option discipline, bind/run shapes, **zero-malloc run window**); real-lib leg DECLARED (no libonnxruntime in sandbox) | **PASS** (real-lib declared) |
| **D. llama.cpp/ggml feeder** | `backends/ggml/`: plan layer (dtype map with NAMED unsigned refusals, ggml geometry order, zero-alloc 64-byte placement planner, Whisper-style windowing) + the FORMAL buffer-type road (b4312 ABI, runtime `ggml_nbytes` probe) + the data-pointer road (every era) | 21 AC-G gates green (plan layer fully gated; placement/windowing exact); runtime leg DECLARED (no libggml; probe verdicts reported where present) | **PASS** (runtime declared) |
| **E. E2E latency benchmark** | `bench/`: probe ladder → 4-stage pipeline, every stage labeled, p50/p95/p99 + dispatch isolation + bit-exactness + Law-1 audit window; deterministic SIM-EP (labeled, never hardware) | **Both roads PASS**: GPU road dispatch p50 **38 µs**, pipeline floor **32.7 µs** (SIMD preprocess 22 µs + stages); full GPU-road total 1350 µs carries the SOFTWARE-ICD label (llvmpipe JIT execution — structural proof, performance hardware-deferred); SIMD road total p50 **37.6 µs**, strict **0 allocs** across 512 iterations | **PASS** |

## 3. The Gates (AC-series, 97 per road + ASAN + freeze)

| Suite | Checks | Legs | Highlights |
| :--- | :--- | :--- | :--- |
| AC-V (view ABI) | 29 | -O2 + ASAN | 13-rung refusal ladder; strided-reach byte_len; gpu_ready alignment law; WTS1 compose; SIMD bit-exact oracle |
| AC-K (vulkan) | 11 | no-ICD + ICD | fd-road import; **GPU preprocess bit-exact**; **alias canary**; definite host-road verdict; non-session refusal; below-alignment refusal |
| AC-M (metal) | 13 | portable | geometry law (64-byte rows, pixel multiples); MSL frozen-identity; weak-stub honesty off-Apple |
| AC-O (onnx) | 14 (+3 declared) | mock + real-lib-declared | pointer identity through the table; pooling profile enforced; **zero-malloc pooled run** (AC-O13) |
| AC-G (ggml) | 21 (+1 declared) | plan + runtime-declared | unsigned-dtype named refusals; ne[] reversal exactness; planner epoch/reset; windowing partial-tail policy |
| AC-X (capstone) | 6 | no-ICD + ICD | one view → SIMD + GPU + ONNX-wrap + ggml-shape + Metal-geometry, all consistent; GPU == scalar bit-exact on the ICD leg |
| bench | G1-G4 | both roads | both PASS (see scoreboard); JSON evidence line |
| meta | — | ASAN + spv | leak-clean; `.spv` rebuild **byte-identical** (kernel freeze holds) |

## 4. Honesty Ledger

- **MEASURED here**: every refusal leg (both roads); the fd-road zero-copy
  preprocess (bit-exact, structure — NOT discrete-GPU performance; the
  logs say so); the pooled-dispatch overhead (38 µs on the software ICD
  includes lvp's submit internals — the minimal-dispatch methodology
  isolates the API shape); the full mock-battery logic for ORT; the plan
  layer for ggml; SIMD (AVX2) bit-exactness; the Law-1 zero-malloc
  window on CPU roads.
- **CI-GATED**: .spv byte-identity; ASAN; the no-ICD refusal leg.
- **DECLARED**: Metal device roads (apple CI leg — the
  WeftMetalZeroCopy precedent); real-lib ORT/ggml legs (mock + probe
  batteries carry the logic; fixtures ship in-tree); hardware dispatch
  numbers (lavapipe's 1350 µs full-frame preprocess is CPU/JIT
  execution, labeled `gpu_is_software_icd` in the JSON — the gpu_ring
  precedent: structural claim proven, performance hardware-deferred).
- **REPRODUCED DISCOVERY**: llvmpipe's host-road fresh-memory backing
  (Series 10) — the canary catches it and the gate asserts the DEFINITE
  verdict (AC-K9b).

## 5. The Engineering Discoveries (worth the lead's attention)

1. **`vkGetInstanceProcAddr(NULL, ...)` resolves ONLY the four global
   commands** — `vkDestroyInstance` must be resolved post-instance;
   resolving it pre-instance silently returns NULL on conforming
   loaders and every subsequent teardown is broken. Caught by the
   no-ICD refusal leg's error naming.
2. **`vkEnumerateDeviceExtensionProperties` returns `VK_INCOMPLETE`
   (a SUCCESS-class code) on partial fills** — lavapipe advertises 153
   extensions; a 64-entry window + a `!= VK_SUCCESS` check silently
   drops the dma-buf extension. The bridge now takes the full count and
   accepts INCOMPLETE.
3. **`vkCreateComputePipelines` has SIX parameters** (no flags arg —
   flags live inside each create-info). A speculative 7-arg typedef
   shifts every argument and drivers dereference a near-NULL infos
   pointer (the 0x50 SEGV). The vk_min.h discipline exists precisely
   for this class; the kit now uses the substrate's constants
   exclusively.
4. **Descriptor offsets are absolute; push-constant indices are
   RELATIVE to the bound range** — double-counting the absolute offset
   writes outside the intended window without any driver complaint.
   Caught by the bench's bit-exactness gate (G3), not by Vulkan.
5. **Command pools need `RESET_COMMAND_BUFFER_BIT` for explicit
   resets** — undefined behavior otherwise (crashed lvp's worker). The
   kit now flags the pool and keeps its explicit reset discipline.
6. **The ggml 2025 split made buffer-type structs opaque** — the formal
   custom-buffer road is pinned to the b4312 public ABI with a runtime
   `ggml_nbytes` probe; the data-pointer road covers every era. Never a
   silent layout mismatch.

## 6. Files

**View seam**: `tools/weft-tensor/include/weft/{weft_tensor_view,weft_tensor_dialect,weft_accel_common}.h` · `src/{weft_tensor_view,weft_accel_common,weft_simd}.c`
**Vulkan**: `backends/vulkan/{weft_vk_bridge.h,weft_vk_bridge.c,weft_vk_compute.c}` · `shaders/weft_preprocess.{comp,spv}` + `shaders/weft_preprocess_spv.h`
**Metal**: `backends/metal/{weft_metal_bridge.h,weft_metal_core.c,weft_metal_apple.mm}` · `shaders/weft_preprocess.metal`
**ONNX**: `backends/onnx/{weft_ort_bridge.h,weft_ort_bridge.c,weft_ort_abi.h}` · `tests/{mock_ort.c,abi-verify/*}` · `tests/fixtures/{gen_model.py,weft_fixture_identity.onnx,fixture_model.h}`
**ggml**: `backends/ggml/{weft_ggml_bridge.h,weft_ggml_plan.c,weft_ggml_bridge.c}` · `tests/abi-verify/*`
**Bench**: `bench/{weft_accel_bench.c,weft_sim_ep.c}`
**Tests/build**: `tests/test_{view,vk,metal,ort,ggml,cross}.c` · `Makefile`
**Docs/CI**: `rfcs/0017-accelerator-zero-copy-adapters.md` · this report · `ci/scripts/run_weft_tensor_shard.sh` · `litmus/evidence/accelerators/*`

## 7. Open (follow-up series candidates)

Engineer-1 ring-core integration (the view ABI seam is frozen v1; swap
to the core's struct + re-run the static asserts) · CoreML/ANE model
fixture on the apple CI leg (the CVPixelBuffer road is wrapped; a real
MLModel compile test is the next rung) · TensorRT/QNN EP sessions
(same pooled-binding pattern, provider-specific memory info) ·
Whisper-class end-to-end on real libggml (windowing + placement are
gated; the graph build is llama.cpp's) · hardware dispatch evidence
(real GPUs — the 38 µs software-ICD dispatch includes lvp's submit
internals; kernel-submit drivers are expected lower).
