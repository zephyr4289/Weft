# core/c/Makefile — local C build target (also driven by the root Makefile)
#
# Kernel targets (weft.c/weft.h — frozen):
#   spike, spike-tsan            — the kernel litmus runner (L1-L8) + the
#                                  Issue #16 Tier 1 L9-L16 suite (L13 needs
#                                  the fan-out driver layer linked in)
# Fan-out driver-layer targets (fanout.{h,c}, frame_cursor.{h,c} — RFC 0004/0008):
#   fanout-test{,-asan,-tsan,-seq}   — F-series + FrameCursor conformance
#   fanout-runner{,-asan,-tsan,-seq} — torture + xlang dump/validate runner
# The -seq variants build with -DWEFT_FANOUT_SEQ_CST=1 (the TS-equivalent
# all-seq_cst stamp regime) so both ordering regimes are torture-gated.
# VerifiedWeft targets (RFC 0005): verified-test, verified-runner, -asan variants.
# Governor targets (RFC 0009): governor-test — G-series conformance + the
# xlang-dump trace mode that G5 cross-language parity fixture consumes.

CC       = gcc
CFLAGS   = -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE
CFLAGS_DBG = -O0 -g -fsanitize=thread -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE
CFLAGS_ASAN = -O1 -g -fsanitize=address -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE

TARGET   = spike
TARGET_TSAN = spike-tsan

# Issue #17-1: every target linking fanout.c links the SIMD claim-copy seam
# (fanout_simd.{h,c}) — the dispatcher resolves scalar when silicon refuses.
FSIMD_SRC = fanout_simd.c
FANOUT_SRC = fanout.c frame_cursor.c weft.c $(FSIMD_SRC)

all: $(TARGET)

$(TARGET): weft.c fanout.c frame_cursor.c fanout_simd.c litmus_runner.c weft.h fanout.h frame_cursor.h fanout_simd.h
	$(CC) $(CFLAGS) -o $(TARGET) weft.c fanout.c frame_cursor.c $(FSIMD_SRC) litmus_runner.c

$(TARGET_TSAN): weft.c fanout.c frame_cursor.c fanout_simd.c litmus_runner.c weft.h fanout.h frame_cursor.h fanout_simd.h
	$(CC) $(CFLAGS_DBG) -o $(TARGET_TSAN) weft.c fanout.c frame_cursor.c $(FSIMD_SRC) litmus_runner.c

# ---- fan-out driver layer (RFC 0004 + RFC 0008) --------------------------

fanout-test: fanout.c frame_cursor.c fanout_test.c fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS) -o fanout-test $(FANOUT_SRC) fanout_test.c

fanout-test-asan: fanout.c frame_cursor.c fanout_test.c fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS_ASAN) -o fanout-test-asan $(FANOUT_SRC) fanout_test.c

fanout-test-tsan: fanout.c frame_cursor.c fanout_test.c fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS_DBG) -o fanout-test-tsan $(FANOUT_SRC) fanout_test.c

fanout-test-seq: fanout.c frame_cursor.c fanout_test.c fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS) -DWEFT_FANOUT_SEQ_CST=1 -o fanout-test-seq $(FANOUT_SRC) fanout_test.c

fanout-runner: fanout.c frame_cursor.c fanout_runner.c fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS) -o fanout-runner $(FANOUT_SRC) fanout_runner.c

fanout-runner-asan: fanout.c frame_cursor.c fanout_runner.c fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS_ASAN) -o fanout-runner-asan $(FANOUT_SRC) fanout_runner.c

fanout-runner-tsan: fanout.c frame_cursor.c fanout_runner.c fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS_DBG) -o fanout-runner-tsan $(FANOUT_SRC) fanout_runner.c

fanout-runner-seq: fanout.c frame_cursor.c fanout_runner.c fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS) -DWEFT_FANOUT_SEQ_CST=1 -o fanout-runner-seq $(FANOUT_SRC) fanout_runner.c

# ---- Issue #17-1: SIMD claim copy (fanout_simd) ----------------------------
# FS-series conformance (byte-identity gate, end-to-end integrity, torture)
# + --copy-bench micro mode. -legacy builds the seam DISABLED (the literal
# inline scalar loop) and must pass the identical battery — the escape
# hatch is a compatibility contract, not a stub.

fanout-simd-test: fanout.c fanout_simd.c fanout_simd_test.c fanout.h fanout_simd.h weft.h
	$(CC) $(CFLAGS) -o fanout-simd-test $(FANOUT_SRC) fanout_simd_test.c

fanout-simd-test-asan: fanout.c fanout_simd.c fanout_simd_test.c fanout.h fanout_simd.h weft.h
	$(CC) $(CFLAGS_ASAN) -o fanout-simd-test-asan $(FANOUT_SRC) fanout_simd_test.c

fanout-simd-test-tsan: fanout.c fanout_simd.c fanout_simd_test.c fanout.h fanout_simd.h weft.h
	$(CC) $(CFLAGS_DBG) -o fanout-simd-test-tsan $(FANOUT_SRC) fanout_simd_test.c

fanout-simd-test-seq: fanout.c fanout_simd.c fanout_simd_test.c fanout.h fanout_simd.h weft.h
	$(CC) $(CFLAGS) -DWEFT_FANOUT_SEQ_CST=1 -o fanout-simd-test-seq $(FANOUT_SRC) fanout_simd_test.c

fanout-simd-test-legacy: fanout.c fanout_simd.c fanout_simd_test.c fanout.h fanout_simd.h weft.h
	$(CC) $(CFLAGS) -DWEFT_FANOUT_SIMD_DISABLE=1 -o fanout-simd-test-legacy $(FANOUT_SRC) fanout_simd_test.c

# ---- Issue #17-2: cache-aware slot layout (fanout_layout gate) ------------
# FL-series: split-theory (validated vs brute force), allocation guarantee,
# latency A/B (base- vs slot-aligned), interop byte-layout equality, foreign
# ring tolerance. -smallline builds the 32B-line leg (ARMv7/embedded class
# CPUs; a 64B-aligned payload base is 32B-aligned too — the no-regression
# claim exercised at its own line size).

fanout-layout-test: fanout.c fanout_simd.c fanout_layout_test.c fanout.h fanout_simd.h weft.h
	$(CC) $(CFLAGS) -o fanout-layout-test $(FANOUT_SRC) fanout_layout_test.c

fanout-layout-test-asan: fanout.c fanout_simd.c fanout_layout_test.c fanout.h fanout_simd.h weft.h
	$(CC) $(CFLAGS_ASAN) -o fanout-layout-test-asan $(FANOUT_SRC) fanout_layout_test.c

fanout-layout-test-smallline: fanout.c fanout_simd.c fanout_layout_test.c fanout.h fanout_simd.h weft.h
	$(CC) $(CFLAGS) -DWEFT_FANOUT_SLOT_ALIGN_BYTES=32 -o fanout-layout-test-smallline $(FANOUT_SRC) fanout_layout_test.c

# ---- Issue #17-3: writer batching (fanout_batch) --------------------------
# FB-series: batch roundtrip, mixed-mode compat, dropped accounting under
# n > M bursts (windowed stamping torture), atomic refusal, commit-only
# API + --bench throughput A/B (100-frame batches vs 100 singles, idle
# and reader-polling legs). Pure atomics: TSAN-clean by construction.

fanout-batch-test: fanout.c fanout_simd.c fanout_batch.c fanout_batch_test.c fanout.h fanout_simd.h fanout_batch.h weft.h
	$(CC) $(CFLAGS) -o fanout-batch-test $(FANOUT_SRC) fanout_batch.c fanout_batch_test.c

fanout-batch-test-asan: fanout.c fanout_simd.c fanout_batch.c fanout_batch_test.c fanout.h fanout_simd.h fanout_batch.h weft.h
	$(CC) $(CFLAGS_ASAN) -o fanout-batch-test-asan $(FANOUT_SRC) fanout_batch.c fanout_batch_test.c

fanout-batch-test-tsan: fanout.c fanout_simd.c fanout_batch.c fanout_batch_test.c fanout.h fanout_simd.h fanout_batch.h weft.h
	$(CC) $(CFLAGS_DBG) -o fanout-batch-test-tsan $(FANOUT_SRC) fanout_batch.c fanout_batch_test.c

fanout-batch-test-seq: fanout.c fanout_simd.c fanout_batch.c fanout_batch_test.c fanout.h fanout_simd.h fanout_batch.h weft.h
	$(CC) $(CFLAGS) -DWEFT_FANOUT_SEQ_CST=1 -o fanout-batch-test-seq $(FANOUT_SRC) fanout_batch.c fanout_batch_test.c

# ---- RFC 0011 chaos engine ------------------------------------------------
# fanout-chaos: stepped (deterministic scheduler -- the parity oracle) +
#               free (real threads, seed-deterministic fault sequence).
#               -seq builds the all-seq_cst A/B regime, as with the torture
#               gates; -asan builds the sanitizer leg.

fanout-chaos: fanout.c frame_cursor.c fanout_chaos.c fanout_chaos_runner.c fanout_chaos.h fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS) -DWEFT_FANOUT_CHAOS_FREE_MODE=1 -o fanout-chaos $(FANOUT_SRC) fanout_chaos.c fanout_chaos_runner.c

fanout-chaos-asan: fanout.c frame_cursor.c fanout_chaos.c fanout_chaos_runner.c fanout_chaos.h fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS_ASAN) -DWEFT_FANOUT_CHAOS_FREE_MODE=1 -o fanout-chaos-asan $(FANOUT_SRC) fanout_chaos.c fanout_chaos_runner.c

fanout-chaos-seq: fanout.c frame_cursor.c fanout_chaos.c fanout_chaos_runner.c fanout_chaos.h fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS) -DWEFT_FANOUT_CHAOS_FREE_MODE=1 -DWEFT_FANOUT_SEQ_CST=1 -o fanout-chaos-seq $(FANOUT_SRC) fanout_chaos.c fanout_chaos_runner.c

wire-probe: wire_probe.c fanout.c frame_cursor.c weft.c fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS) -o wire-probe wire_probe.c fanout.c frame_cursor.c weft.c $(FSIMD_SRC)

# ---- VerifiedWeft driver layer (RFC 0005) --------------------------------
# sha256.{h,c} + sha256_hw.c (Series 6 HW dispatch: x86 SHA-NI via target
# attribute — no flag changes; aarch64 CE needs +crypto, see below) +
# hmac.{h,c} + verified.{h,c} — pure C, zero dependencies.
# V-series conformance + xlang sign/verify runner. weft.c/weft.h untouched.
# Series 7 (SIMD batch): sha256_mb.{h,c} (8-way AVX2 / 4-way NEON
# multi-buffer transform) + verified_mb.{h,c} (lane-batched HMAC verify +
# vectorized magic scan) + the VMB conformance series.

VW_SRC = sha256.c sha256_hw.c hmac.c verified.c verified_mb.c sha256_mb.c weft.c

# aarch64: compile the CE transform with the crypto extension enabled
# (mbedTLS-style); execution stays gated on the runtime HWCAP probe, so the
# binary remains safe on non-CE ARM hardware. x86-64 needs no extra flags.
ifeq ($(shell $(CC) -dumpmachine 2>/dev/null | grep -c 'aarch64\|arm64'),1)
  VW_CFLAGS := -march=armv8-a+crypto
else
  VW_CFLAGS :=
endif

verified-test: sha256.c sha256_hw.c hmac.c verified.c verified_test.c weft.c weft.h sha256.h hmac.h verified.h
	$(CC) $(CFLAGS) $(VW_CFLAGS) -o verified-test $(VW_SRC) verified_test.c

verified-test-asan: sha256.c sha256_hw.c hmac.c verified.c verified_test.c weft.c weft.h sha256.h hmac.h verified.h
	$(CC) $(CFLAGS_ASAN) $(VW_CFLAGS) -o verified-test-asan $(VW_SRC) verified_test.c

verified-mb-test: sha256_mb.c sha256_mb.h verified_mb.c verified_mb.h verified_mb_test.c $(VW_SRC) weft.h
	$(CC) $(CFLAGS) $(VW_CFLAGS) -o verified-mb-test $(VW_SRC) verified_mb_test.c

verified-mb-test-asan: sha256_mb.c sha256_mb.h verified_mb.c verified_mb.h verified_mb_test.c $(VW_SRC) weft.h
	$(CC) $(CFLAGS_ASAN) $(VW_CFLAGS) -o verified-mb-test-asan $(VW_SRC) verified_mb_test.c

# Series 8 (RFC-0012): AVX-512 16-lane multi-buffer + per-impl pinning bench.
mb-bench: sha256_mb.c sha256_mb.h sha256_mb_bench.c verified_mb.c verified_mb.h $(VW_SRC) weft.h
	$(CC) $(CFLAGS) $(VW_CFLAGS) -o mb-bench $(VW_SRC) sha256_mb_bench.c

verified-runner: sha256.c sha256_hw.c hmac.c verified.c verified_runner.c weft.c weft.h sha256.h hmac.h verified.h
	$(CC) $(CFLAGS) $(VW_CFLAGS) -o verified-runner $(VW_SRC) verified_runner.c

verified-runner-asan: sha256.c sha256_hw.c hmac.c verified.c verified_runner.c weft.c weft.h sha256.h hmac.h verified.h
	$(CC) $(CFLAGS_ASAN) $(VW_CFLAGS) -o verified-runner-asan $(VW_SRC) verified_runner.c

# ---- Inter-process shared-memory rings (RFC-0011 draft) -------------------
# shm_ring.{h,c}: POSIX named/anonymous + Windows-gated session protocol over
# the RFC-0004 fan-out ring. S-series conformance (fork torture included)
# + the IPC torture/xlang runner. Linux link note: shm_open lives in librt
# on glibc < 2.17 and in libc since; -lrt is accepted and harmless either way.
SHM_SRC = shm_ring.c fanout.c weft.c $(FSIMD_SRC)

shm-test: shm_ring.c shm_ring.h shm_test.c $(SHM_SRC) fanout.h weft.h
	$(CC) $(CFLAGS) -o shm-test $(SHM_SRC) shm_test.c -lrt -lpthread

shm-test-asan: shm_ring.c shm_ring.h shm_test.c $(SHM_SRC) fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o shm-test-asan $(SHM_SRC) shm_test.c -lrt -lpthread

shm-runner: shm_ring.c shm_ring.h shm_runner.c $(SHM_SRC) fanout.h weft.h
	$(CC) $(CFLAGS) -o shm-runner $(SHM_SRC) shm_runner.c -lrt -lpthread

shm-runner-asan: shm_ring.c shm_ring.h shm_runner.c $(SHM_SRC) fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o shm-runner-asan $(SHM_SRC) shm_runner.c -lrt -lpthread

# ---- GPU-resident rings (RFC-0003 Triad-2 spike, Series 7) ------------------
# gpu_ring.{h,c}: Vulkan HOST_VISIBLE zero-copy session (dlopen'd loader —
# no link-time dep), Metal compile-gated, CPU fallback; vk_min.h is the
# shared INTERNAL minimal Vulkan ABI. gpu-probe: the zero-copy consumer
# proof (compute shader validates live ring words). The .spv is committed;
# glslangValidator rebuilds it when present (CI installs glslang-tools).
GPU_SRC = gpu_ring.c fanout.c weft.c $(FSIMD_SRC)
SPV = ../../probes/compute/validate_frame.spv
SPV_STREAM = ../../probes/compute/stream_frames.spv
SPV_RASTER = ../../probes/compute/rasterize_frame.spv

$(SPV): ../../probes/compute/validate_frame.comp
	if command -v glslangValidator >/dev/null 2>&1; then \
	glslangValidator -V $< -o $@; \
	else echo "glslangValidator not found — using committed $@"; touch $@; fi

$(SPV_STREAM): ../../probes/compute/stream_frames.comp
	if command -v glslangValidator >/dev/null 2>&1; then \
	glslangValidator -V $< -o $@; \
	else echo "glslangValidator not found — using committed $@"; touch $@; fi

$(SPV_RASTER): ../../probes/compute/rasterize_frame.comp
	if command -v glslangValidator >/dev/null 2>&1; then \
	glslangValidator -V $< -o $@; \
	else echo "glslangValidator not found — using committed $@"; touch $@; fi

gpu-probe: gpu_ring.c gpu_ring.h vk_min.h gpu_probe.c $(GPU_SRC) fanout.h weft.h $(SPV)
	$(CC) $(CFLAGS) -o gpu-probe $(GPU_SRC) gpu_probe.c -lrt -lpthread -ldl

gpu-stream-probe: gpu_ring.c gpu_ring.h gpu_stream.c gpu_stream.h gpu_stream_probe.c vk_min.h $(GPU_SRC) fanout.h weft.h $(SPV) $(SPV_STREAM) $(SPV_RASTER)
	$(CC) $(CFLAGS) -o gpu-stream-probe $(GPU_SRC) gpu_stream.c gpu_stream_probe.c -lrt -lpthread -ldl

# RFC-0013 ABI audit gate: when the REAL Khronos headers are installed
# (libvulkan-dev), static-assert every vk_min constant/size against them.
# Without the headers the gate self-skips (declared) — vk_min.h documents
# the hand-verification against r362.
vk-abi-check: vk_abi_check.c vk_min.h
	@if echo '#include <vulkan/vulkan_core.h>' | gcc -fsyntax-only -x c - 2>/dev/null; then \
	$(CC) $(CFLAGS) -DHAVE_VULKAN_HEADERS=1 -o vk-abi-check vk_abi_check.c; \
else \
	echo "vk-abi-check: Khronos headers absent — SKIPPED (declared; vk_min.h hand-verified against r362)"; \
fi

gpu-ring-test: gpu_ring.c gpu_ring.h gpu_ring_test.c $(GPU_SRC) fanout.h weft.h
	$(CC) $(CFLAGS) -o gpu-ring-test $(GPU_SRC) gpu_ring_test.c -lrt -lpthread -ldl

gpu-ring-test-asan: gpu_ring.c gpu_ring.h gpu_ring_test.c $(GPU_SRC) fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o gpu-ring-test-asan $(GPU_SRC) gpu_ring_test.c -lrt -lpthread -ldl

# ---- RFC 0012 Tail-Latency Eradication Layer (turbo) -----------------------
# turbo.{h,c}: capability-detected placement/prefetch/pinning services riding
# the frozen fanout attach seam. T-series conformance (byte-equivalence,
# prefault gate, pinned torture). -seq builds the all-seq_cst A/B regime —
# prefetch and placement must be ordering-neutral there too.

TURBO_SRC = turbo.c fanout.c frame_cursor.c weft.c $(FSIMD_SRC)

turbo-test: turbo.c turbo.h turbo_test.c $(TURBO_SRC) fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS) -o turbo-test $(TURBO_SRC) turbo_test.c

turbo-test-asan: turbo.c turbo.h turbo_test.c $(TURBO_SRC) fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS_ASAN) -o turbo-test-asan $(TURBO_SRC) turbo_test.c

turbo-test-tsan: turbo.c turbo.h turbo_test.c $(TURBO_SRC) fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS_DBG) -o turbo-test-tsan $(TURBO_SRC) turbo_test.c

turbo-test-seq: turbo.c turbo.h turbo_test.c $(TURBO_SRC) fanout.h frame_cursor.h weft.h
	$(CC) $(CFLAGS) -DWEFT_FANOUT_SEQ_CST=1 -o turbo-test-seq $(TURBO_SRC) turbo_test.c

# ---- RFC 0012 kernel-bypass ingestion (uring_rx) ----------------------------
# uring_rx.{h,c}: datagram->frame ingestion under the FI1 bracket; capability
# ladder NONE/SYSCALL/RING/FIXED probed at runtime (no liburing dependency).
# U-series conformance + concurrent torture.

URING_SRC = uring_rx.c fanout.c weft.c $(FSIMD_SRC)

uring-test: uring_rx.c uring_rx.h uring_test.c $(URING_SRC) fanout.h weft.h
	$(CC) $(CFLAGS) -o uring-test $(URING_SRC) uring_test.c

uring-test-asan: uring_rx.c uring_rx.h uring_test.c $(URING_SRC) fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o uring-test-asan $(URING_SRC) uring_test.c

uring-test-tsan: uring_rx.c uring_rx.h uring_test.c $(URING_SRC) fanout.h weft.h
	$(CC) $(CFLAGS_DBG) -o uring-test-tsan $(URING_SRC) uring_test.c

# ---- RFC-0016 s3 DMA-BUF direct binding (weft_dmabuf) ------------------------
# weft_dmabuf.{h,c}: ring storage allocated from /dev/dma_heap/system +
# foreign dma-buf fd import (the cross-device currency). DB-series gates;
# no heap device on the build host -> the refusal leg runs (honest, Law 4).

DMABUF_SRC = weft_dmabuf.c shm_ring.c fanout.c frame_cursor.c weft.c $(FSIMD_SRC)

dmabuf-test: weft_dmabuf.c weft_dmabuf.h weft_dmabuf_test.c $(DMABUF_SRC) shm_ring.h fanout.h weft.h
	$(CC) $(CFLAGS) -o dmabuf-test $(DMABUF_SRC) weft_dmabuf_test.c -lrt

dmabuf-test-asan: weft_dmabuf.c weft_dmabuf.h weft_dmabuf_test.c $(DMABUF_SRC) shm_ring.h fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o dmabuf-test-asan $(DMABUF_SRC) weft_dmabuf_test.c -lrt

# ---- RFC-0016 s2 GPU external-memory wrap (gpu_extmem_test) ------------------
# XE/XD-series: VK_EXT_external_memory_host + VK_KHR_external_memory_fd wrap
# roads over EXISTING WFSH sessions. Needs the .spv (rebuilt by the shard
# when glslangValidator is present; the committed .spv is canonical).

EXTMEM_SRC = gpu_ring.c gpu_stream.c weft_dmabuf.c shm_ring.c fanout.c frame_cursor.c weft.c $(FSIMD_SRC)

gpu-extmem-test: gpu_extmem_test.c $(EXTMEM_SRC) gpu_ring.h gpu_stream.h weft_dmabuf.h shm_ring.h fanout.h weft.h
	$(CC) $(CFLAGS) -o gpu-extmem-test $(EXTMEM_SRC) gpu_extmem_test.c -lrt -ldl

gpu-extmem-test-asan: gpu_extmem_test.c $(EXTMEM_SRC) gpu_ring.h gpu_stream.h weft_dmabuf.h shm_ring.h fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o gpu-extmem-test-asan $(EXTMEM_SRC) gpu_extmem_test.c -lrt -ldl

# ---- RFC-0016 s4 AF_XDP kernel-bypass ingestion (xdp_rx) ---------------------
# xdp_rx.{h,c}: NIC DMA straight into ring slot payloads (UMEM-as-session,
# unaligned chunks, pre-bracketed fill ring). X-series gates: the state
# machine (synthetic descriptors) + the uring delegation run everywhere;
# the xsk legs run where CONFIG_XDP_SOCKETS exists.

XDP_SRC = xdp_rx.c uring_rx.c weft_dmabuf.c fanout.c frame_cursor.c weft.c $(FSIMD_SRC)

xdp-test: xdp_rx.c xdp_rx.h xdp_test.c $(XDP_SRC) uring_rx.h weft_dmabuf.h fanout.h weft.h
	$(CC) $(CFLAGS) -o xdp-test $(XDP_SRC) xdp_test.c

xdp-test-asan: xdp_rx.c xdp_rx.h xdp_test.c $(XDP_SRC) uring_rx.h weft_dmabuf.h fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o xdp-test-asan $(XDP_SRC) xdp_test.c

# ---- RFC-0016 s4 LIVE-mode proof rig (privileged runners only) ---------------
# xdp_live_gate.c: compiles everywhere Linux does; refuses honestly (exit 3)
# on kernels without CONFIG_XDP_SOCKETS. Driven by ci/scripts/xdp_loopback_proof.sh.

xdp-live-gate: xdp_live_gate.c $(XDP_SRC) xdp_rx.h weft_dmabuf.h fanout.h weft.h
	$(CC) $(CFLAGS) -o xdp-live-gate $(XDP_SRC) xdp_live_gate.c

# ---- RFC-0016 s5 tensor stream bridge (weft_tensor) --------------------------
# WTS1 in-band tensor frames over the ordinary fan-out API. T-series gates.

TENSOR_SRC = weft_tensor.c weft_f16_codec.c weft_dmabuf.c fanout.c frame_cursor.c weft.c $(FSIMD_SRC)

tensor-test: weft_tensor.c weft_tensor.h weft_tensor_test.c $(TENSOR_SRC) weft_f16_codec.h weft_dmabuf.h fanout.h weft.h
	$(CC) $(CFLAGS) -o tensor-test $(TENSOR_SRC) weft_tensor_test.c -lm

tensor-test-asan: weft_tensor.c weft_tensor.h weft_tensor_test.c $(TENSOR_SRC) weft_f16_codec.h weft_dmabuf.h fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o tensor-test-asan $(TENSOR_SRC) weft_tensor_test.c -lm

# ---- RFC-0016 s5.2 fused compute passes (tensor_reduce + fft_radix2) ---------
# The .comp shaders consume live WTS1 frames through the imported ring;
# .spv rebuilt by glslangValidator when present (committed .spv canonical).
# hetero_probe.c is the capstone: memfd session -> wrap_dmabuf -> CPU
# publishes -> GPU reduces -> CPU references cross-check.

SPV_TENSOR = ../../probes/compute/tensor_reduce.spv
SPV_FFT = ../../probes/compute/fft_radix2.spv

$(SPV_TENSOR): ../../probes/compute/tensor_reduce.comp
	if command -v glslangValidator >/dev/null 2>&1; then \
	glslangValidator -V $< -o $@; \
	else echo "glslangValidator not found — using committed $@"; touch $@; fi

$(SPV_FFT): ../../probes/compute/fft_radix2.comp
	if command -v glslangValidator >/dev/null 2>&1; then \
	glslangValidator -V $< -o $@; \
	else echo "glslangValidator not found — using committed $@"; touch $@; fi

HETERO_SRC = gpu_ring.c gpu_stream.c weft_dmabuf.c weft_tensor.c weft_f16_codec.c shm_ring.c fanout.c frame_cursor.c weft.c $(FSIMD_SRC)


# ---- RFC-0016 s6 camera/compositor seams (v4l2 + ahb) -------------------------
# V-series gates: refusal legs run everywhere (the CI state); capture/AHB
# legs run where the hardware exists.

CAMERA_SRC = weft_hw_v4l2.c weft_hw_ahb.c weft_dmabuf.c fanout.c frame_cursor.c weft.c $(FSIMD_SRC)

camera-test: weft_hw_camera_test.c $(CAMERA_SRC) weft_hw_v4l2.h weft_hw_ahb.h weft_dmabuf.h fanout.h weft.h
	$(CC) $(CFLAGS) -o camera-test $(CAMERA_SRC) weft_hw_camera_test.c

camera-test-asan: weft_hw_camera_test.c $(CAMERA_SRC) weft_hw_v4l2.h weft_hw_ahb.h weft_dmabuf.h fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o camera-test-asan $(CAMERA_SRC) weft_hw_camera_test.c

hetero-probe: hetero_probe.c $(HETERO_SRC) $(SPV_TENSOR) $(SPV_FFT) gpu_ring.h gpu_stream.h weft_dmabuf.h weft_tensor.h weft_f16_codec.h fanout.h weft.h
	$(CC) $(CFLAGS) -o hetero-probe $(HETERO_SRC) hetero_probe.c -lrt -ldl -lm





# ---- RFC 0012 T-bench evidence runner ---------------------------------------
# turbo_runner.c: caps / TL-writer / TL-reader / TL-ring / ING-uring modes;
# one JSON line per variant (bench_runner.c methodology). -sys builds the
# forced-SYSCALL ingestion A/B leg (-DWEFT_URING_FORCE_SYSCALL, bench-only).

TRUNNER_SRC = turbo.c uring_rx.c fanout.c weft.c $(FSIMD_SRC)

turbo-runner: turbo_runner.c $(TRUNNER_SRC) turbo.h uring_rx.h fanout.h weft.h
	$(CC) $(CFLAGS) -o turbo-runner $(TRUNNER_SRC) turbo_runner.c

turbo-runner-sys: turbo_runner.c $(TRUNNER_SRC) turbo.h uring_rx.h fanout.h weft.h
	$(CC) $(CFLAGS) -DWEFT_URING_FORCE_SYSCALL=1 -o turbo-runner-sys $(TRUNNER_SRC) turbo_runner.c

# ---- Issue #16 Tier 1: revocation handshake stress (Task 5) --------------

revoke-stress: revoke_stress.c weft.c weft.h
	$(CC) $(CFLAGS) -o revoke-stress revoke_stress.c weft.c

revoke-stress-asan: revoke_stress.c weft.c weft.h
	$(CC) $(CFLAGS_ASAN) -o revoke-stress-asan revoke_stress.c weft.c

revoke-stress-tsan: revoke_stress.c weft.c weft.h
	$(CC) $(CFLAGS_DBG) -o revoke-stress-tsan revoke_stress.c weft.c

# ---- Issue #16 Tier 1: FFI-boundary structured fuzz (Task 4) -------------

ffi-fuzz: ffi_fuzz.c weft.c weft.h
	$(CC) $(CFLAGS) -o ffi-fuzz ffi_fuzz.c weft.c

ffi-fuzz-asan: ffi_fuzz.c weft.c weft.h
	$(CC) $(CFLAGS_ASAN) -o ffi-fuzz-asan ffi_fuzz.c weft.c

# ---- Issue #15: thin hardware-adaptive layer (weft_hw) --------------------

weft-hw-test: weft_hw.c weft_hw_test.c weft_hw.h weft.c weft.h
	$(CC) $(CFLAGS) -o weft-hw-test weft_hw.c weft_hw_test.c weft.c

weft-hw-test-asan: weft_hw.c weft_hw_test.c weft_hw.h weft.c weft.h
	$(CC) $(CFLAGS_ASAN) -o weft-hw-test-asan weft_hw.c weft_hw_test.c weft.c

bench: bench_runner.c weft.c weft.h
	$(CC) $(CFLAGS) -o bench bench_runner.c weft.c

governor-test: governor.c governor_test.c governor.h
	$(CC) $(CFLAGS) -o governor-test governor.c governor_test.c

governor-test-asan: governor.c governor_test.c governor.h
	$(CC) $(CFLAGS_ASAN) -o governor-test-asan governor.c governor_test.c

# ---- Series 8: SIMD Q12 raster blend (RFC-0009 §cadence raster op) --------
# blend-test: bit-exactness gate — every compiled path (sse4.1/avx2/neon)
#             must equal the scalar reference on the full sweep; also the
#             --digests golden-vector emitter for the xlang-blend fixture.
# blend-runner: ns/frame + GB/s at 1080p/4K/8K, scalar vs dispatched SIMD.

blend-test: blend_q12.c blend_test.c blend_q12.h
	$(CC) $(CFLAGS) -o blend-test blend_q12.c blend_test.c

blend-test-asan: blend_q12.c blend_test.c blend_q12.h
	$(CC) $(CFLAGS_ASAN) -o blend-test-asan blend_q12.c blend_test.c

blend-runner: blend_q12.c blend_runner.c blend_q12.h
	$(CC) $(CFLAGS) -o blend-runner blend_q12.c blend_runner.c

# ---- Series 8: thread QoS + the native render worklet ---------------------
# worklet-test: W1 no-lost-ticks, W2 single-consumer order, W3 QoS honesty,
#               W4 post->run latency distribution, W5 the governed blend
#               loop on the worklet thread.
# qos-test: thread_qos flags honesty (affinity applies / RT refused is
#           declared; the big-core mask parses or abstains).

worklet-test: worklet.c worklet.h thread_qos.c thread_qos.h worklet_test.c blend_q12.c blend_q12.h
	$(CC) $(CFLAGS) -o worklet-test worklet.c thread_qos.c worklet_test.c blend_q12.c

qos-test: thread_qos.c thread_qos.h qos_test.c
	$(CC) $(CFLAGS) -o qos-test thread_qos.c qos_test.c

# ---- Issue #19 Tier 4: fuzz runner & bounded revocation ------------------

fuzz-runner: fuzz_runner.c weft.c fanout.c frame_cursor.c weft.h fanout.h
	$(CC) $(CFLAGS) -o fuzz-runner fuzz_runner.c weft.c fanout.c frame_cursor.c

fuzz-runner-asan: fuzz_runner.c weft.c fanout.c frame_cursor.c weft.h fanout.h
	$(CC) $(CFLAGS_ASAN) -o fuzz-runner-asan fuzz_runner.c weft.c fanout.c frame_cursor.c

reclaim-test: weft.c reclaim_test.c weft.h
	$(CC) $(CFLAGS) -o reclaim-test weft.c reclaim_test.c

reclaim-test-asan: weft.c reclaim_test.c weft.h
	$(CC) $(CFLAGS_ASAN) -o reclaim-test-asan weft.c reclaim_test.c

# ---- Tier 4: FFI boundary isolation (issue #19 §2) ------------------------

ffi_host-test: ffi_host.c ffi_host.h ffi_host_test.c
	$(CC) $(CFLAGS) -o ffi_host-test ffi_host.c ffi_host_test.c

ffi_host-test-asan: ffi_host.c ffi_host.h ffi_host_test.c
	$(CC) $(CFLAGS_ASAN) -o ffi_host-test-asan ffi_host.c ffi_host_test.c

# ---- Tier 5: .weftrec v4 trace standard (issue #20 §1, RFC 0014) ----------
# trace_rec-test: T-series — codec conformance (round trip, CRC catch,
#                  version gate, kind gate, exact size, JSON export, vector).
# trace-dump: the deterministic scenario emitter + reference tools
#             (hex = the cross-port byte-identity surface).

# ---- Tier 5: Prometheus exposition (issue #20 §2) -------------------------
# metrics-dump: kernel counters + live measured latency quantiles in the
# Prometheus TEXT format (scraped by tools/prometheus/weft_exporter.py).

# p99-bench: publish/claim latency quantiles (min-of-reps) for the
# perf-regression gate baseline (issue #20 §4).
p99-bench: p99_bench.c weft.c weft.h
	$(CC) $(CFLAGS) -o p99-bench p99_bench.c weft.c

metrics-dump: metrics_dump.c weft.c weft.h
	$(CC) $(CFLAGS) -o metrics-dump metrics_dump.c weft.c

trace_rec-test: trace_rec.c trace_rec.h trace_rec_test.c
	$(CC) $(CFLAGS) -o trace_rec-test trace_rec.c trace_rec_test.c

trace-dump: trace_dump.c trace_rec.c trace_rec.h weft.c weft.h
	$(CC) $(CFLAGS) -o trace-dump trace_dump.c trace_rec.c weft.c

# ---- Axis 3: Self-stabilizing ring recovery (doc-006) --------------------
recovery-test: recovery_test.c fanout.c frame_cursor.c fanout_simd.c weft.c fanout.h weft.h
	$(CC) $(CFLAGS) -o recovery-test recovery_test.c fanout.c frame_cursor.c fanout_simd.c weft.c

recovery-test-asan: recovery_test.c fanout.c frame_cursor.c fanout_simd.c weft.c fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o recovery-test-asan recovery_test.c fanout.c frame_cursor.c fanout_simd.c weft.c

# ---- RFC-0016: Zero-Copy IPC Mesh (weft_shm + weft_ipc) --------------------
# weft_shm.{h,c}: sealed memfd sessions + kernel-enforced RO views + SCM_RIGHTS
#                 exchange + the HELLO/GRANT handshake (mechanism).
# weft_ipc.{h,c}: lock-free registry (/dev/shm/weft_registry_v1) + heartbeats
#                 + crash healing + successor handoff + HMAC capability
#                 tokens over the in-tree sha256/hmac (policy).
# weft-shm-test / weft-ipc-test: M-series / R-series conformance.
# weft-ipc-torture: memfd | mesh | latency modes (100k+ cross-process
#                   publications, crashy producer restart, park-vs-eventfd).
#                   ASAN legs fork heavily — set ASAN_OPTIONS=abort_on_error=1.

IPC_MESH_SRC = weft_shm.c weft_ipc.c shm_ring.c fanout.c fanout_simd.c frame_cursor.c weft.c sha256.c sha256_hw.c hmac.c

weft-shm-test: weft_shm.c weft_shm.h weft_ipc.c weft_ipc.h weft_shm_test.c shm_ring.c shm_ring.h fanout.c fanout_simd.c frame_cursor.c weft.c sha256.c sha256_hw.c hmac.c
	$(CC) $(CFLAGS) -o weft-shm-test $(IPC_MESH_SRC) weft_shm_test.c -lrt -lpthread

weft-shm-test-asan: weft_shm.c weft_shm.h weft_ipc.c weft_ipc.h weft_shm_test.c shm_ring.c shm_ring.h fanout.c fanout_simd.c frame_cursor.c weft.c sha256.c sha256_hw.c hmac.c
	$(CC) $(CFLAGS_ASAN) -o weft-shm-test-asan $(IPC_MESH_SRC) weft_shm_test.c -lrt -lpthread

weft-ipc-test: weft_shm.c weft_shm.h weft_ipc.c weft_ipc.h weft_ipc_test.c shm_ring.c shm_ring.h fanout.c fanout_simd.c frame_cursor.c weft.c sha256.c sha256_hw.c hmac.c
	$(CC) $(CFLAGS) -o weft-ipc-test $(IPC_MESH_SRC) weft_ipc_test.c -lrt -lpthread

weft-ipc-test-asan: weft_shm.c weft_shm.h weft_ipc.c weft_ipc.h weft_ipc_test.c shm_ring.c shm_ring.h fanout.c fanout_simd.c frame_cursor.c weft.c sha256.c sha256_hw.c hmac.c
	$(CC) $(CFLAGS_ASAN) -o weft-ipc-test-asan $(IPC_MESH_SRC) weft_ipc_test.c -lrt -lpthread

weft-ipc-test-tsan: weft_shm.c weft_shm.h weft_ipc.c weft_ipc.h weft_ipc_test.c shm_ring.c shm_ring.h fanout.c fanout_simd.c frame_cursor.c weft.c sha256.c sha256_hw.c hmac.c
	$(CC) $(CFLAGS_DBG) -o weft-ipc-test-tsan $(IPC_MESH_SRC) weft_ipc_test.c -lrt -lpthread

weft-ipc-torture: weft_shm.c weft_shm.h weft_ipc.c weft_ipc.h weft_ipc_torture.c shm_ring.c shm_ring.h fanout.c fanout_simd.c frame_cursor.c weft.c sha256.c sha256_hw.c hmac.c
	$(CC) $(CFLAGS) -o weft-ipc-torture $(IPC_MESH_SRC) weft_ipc_torture.c -lrt -lpthread

weft-ipc-torture-asan: weft_shm.c weft_shm.h weft_ipc.c weft_ipc.h weft_ipc_torture.c shm_ring.c shm_ring.h fanout.c fanout_simd.c frame_cursor.c weft.c sha256.c sha256_hw.c hmac.c
	$(CC) $(CFLAGS_ASAN) -o weft-ipc-torture-asan $(IPC_MESH_SRC) weft_ipc_torture.c -lrt -lpthread

# ---- Device Spectrum Driver Optimizations (doc-007) ----------------------
DEV_SPECTRUM_SRC = weft_device_profile.c weft_f16_codec.c weft_render_bridge.c fanout.c frame_cursor.c fanout_simd.c weft.c

device-spectrum-test: $(DEV_SPECTRUM_SRC) device_spectrum_test.c weft_device_profile.h weft_f16_codec.h weft_render_bridge.h fanout.h weft.h
	$(CC) $(CFLAGS) -o device-spectrum-test $(DEV_SPECTRUM_SRC) device_spectrum_test.c -lm

device-spectrum-test-asan: $(DEV_SPECTRUM_SRC) device_spectrum_test.c weft_device_profile.h weft_f16_codec.h weft_render_bridge.h fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o device-spectrum-test-asan $(DEV_SPECTRUM_SRC) device_spectrum_test.c -lm

device-spectrum-test-tsan: $(DEV_SPECTRUM_SRC) device_spectrum_test.c weft_device_profile.h weft_f16_codec.h weft_render_bridge.h fanout.h weft.h
	$(CC) $(CFLAGS_DBG) -o device-spectrum-test-tsan $(DEV_SPECTRUM_SRC) device_spectrum_test.c -lm

clean:
	rm -f $(TARGET) $(TARGET_TSAN) governor-test governor-test-asan fanout-test fanout-test-asan fanout-test-tsan \
	      fanout-test-seq fanout-runner fanout-runner-asan fanout-runner-tsan fanout-runner-seq \
	      fanout-simd-test fanout-simd-test-asan fanout-simd-test-tsan fanout-simd-test-seq fanout-simd-test-legacy \
	      fanout-layout-test fanout-layout-test-asan fanout-layout-test-smallline \
	      fanout-batch-test fanout-batch-test-asan fanout-batch-test-tsan fanout-batch-test-seq \
	      verified-test verified-test-asan verified-runner verified-runner-asan \
	      verified-mb-test verified-mb-test-asan mb-bench \
	      shm-test shm-test-asan shm-runner shm-runner-asan \
	      gpu-probe gpu-stream-probe vk-abi-check gpu-ring-test gpu-ring-test-asan \
	      turbo-test turbo-test-asan turbo-test-tsan turbo-test-seq \
	      uring-test uring-test-asan uring-test-tsan \
	      turbo-runner turbo-runner-sys bench \
	      blend-test blend-test-asan blend-runner \
	      worklet-test qos-test reclaim-test reclaim-test-asan ffi_host-test ffi_host-test-asan trace_rec-test trace-dump metrics-dump p99-bench recovery-test recovery-test-asan \
	      weft-shm-test weft-shm-test-asan weft-ipc-test weft-ipc-test-asan weft-ipc-test-tsan weft-ipc-torture weft-ipc-torture-asan \
	      device-spectrum-test device-spectrum-test-asan device-spectrum-test-tsan

.PHONY: all clean


