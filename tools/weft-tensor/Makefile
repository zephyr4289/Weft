# weft-tensor — RFC-0017: the NPU/GPU accelerator pillar (Pillar 2).
#
# Builds against the canonical driver layer (core/c — fanout, gpu_ring,
# shm_ring, weft_tensor WTS1) with NO link-time Vulkan/ONNX/ggml
# dependency (every runtime is dlopen'd — the gpu_ring rule). Targets
# mirror the house naming:
#   test-view / test-vk / test-metal / test-ort / test-ggml / test-cross
#        — the AC-series gates (V/K/M/O/G/X)
#   bench — the end-to-end latency harness (RFC-0017 §7)
#   asan  — the ASAN legs (NO alloc-audit interposer: ASAN already
#           intercepts malloc; compiling both in is a conflict)
#   evidence — run every gate + bench, tee to litmus/evidence/accelerators
#   spv-check — rebuild the committed .spv and diff (kernel freeze)
#   apple — the Metal device roads (clang ObjC++; the apple CI leg)
#
# The -O2 gates compile with -DWEFT_ACCEL_ALLOC_AUDIT=1: the malloc
# interposer that powers the Law-1 zero-allocation windows (G4, AC-O13).

CC      ?= cc
ROOT     = ../..
CORE     = $(ROOT)/core/c
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
           -DWEFT_ACCEL_ALLOC_AUDIT=1
CFLAGS_ASAN = -O1 -g -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
           -fsanitize=address
INCLUDE  = -I. -Iinclude -I$(CORE) -Ibackends/onnx -Ibackends/onnx/tests
LIBS     = -ldl -lpthread -lm

# driver-layer sources consumed (the substrate)
# ring-session sources (the substrate the bench + vk/cross gates use)
CORESRC  = $(CORE)/gpu_ring.c $(CORE)/shm_ring.c
# pillar sources
# COMMON: pillar sources + the light core modules the view ABI
# composes with (WTS1 dialect + fanout + f16 codec — the __has_include
# seam activates WTS1 on every post-Series-10 tree, so every binary
# links them)
COMMON   = src/weft_tensor_view.c src/weft_accel_common.c src/weft_simd.c \
           $(CORE)/fanout.c $(CORE)/fanout_simd.c \
           $(CORE)/weft_f16_codec.c $(CORE)/weft_tensor.c
VK       = backends/vulkan/weft_vk_bridge.c backends/vulkan/weft_vk_compute.c
ORT      = backends/onnx/weft_ort_bridge.c
MOCKORT  = backends/onnx/tests/mock_ort.c
GGML     = backends/ggml/weft_ggml_bridge.c backends/ggml/weft_ggml_plan.c
METAL    = backends/metal/weft_metal_core.c
ALLBACKENDS = $(VK) $(ORT) $(GGML) $(METAL) $(MOCKORT)

.PHONY: all test evidence spv-check apple clean

all: test-view test-vk test-metal test-ort test-ggml test-cross weft-accel-bench

test: all
	./test-view && ./test-vk && ./test-metal && ./test-ort && \
	./test-ggml && ./test-cross

# ---- the AC-series gates -------------------------------------------------

test-view: tests/test_view.c $(COMMON) $(CORESRC)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ tests/test_view.c $(COMMON) $(CORESRC) $(LIBS)

test-vk: tests/test_vk.c $(VK) $(COMMON) $(CORESRC)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ tests/test_vk.c $(VK) $(COMMON) \
	$(CORESRC) $(LIBS)

test-metal: tests/test_metal.c $(METAL) $(COMMON)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ tests/test_metal.c $(METAL) $(COMMON) $(LIBS)

test-ort: tests/test_ort.c $(ORT) $(MOCKORT) $(COMMON)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ tests/test_ort.c $(ORT) $(MOCKORT) \
	$(COMMON) $(LIBS)

test-ggml: tests/test_ggml.c $(GGML) $(COMMON)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ tests/test_ggml.c $(GGML) $(COMMON) $(LIBS)

test-cross: tests/test_cross.c $(ALLBACKENDS) $(COMMON) $(CORESRC)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ tests/test_cross.c $(ALLBACKENDS) \
	$(COMMON) $(CORESRC) $(LIBS)

# ---- the bench ------------------------------------------------------------

weft-accel-bench: bench/weft_accel_bench.c bench/weft_sim_ep.c $(ALLBACKENDS) \
       $(COMMON) $(CORESRC)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ bench/weft_accel_bench.c \
	bench/weft_sim_ep.c $(ALLBACKENDS) $(COMMON) $(CORESRC) $(LIBS)

# ---- ASAN legs (no audit interposer — ASAN owns malloc) -------------------

test-view-asan: tests/test_view.c $(COMMON) $(CORESRC)
	$(CC) $(CFLAGS_ASAN) $(INCLUDE) -o $@ tests/test_view.c $(COMMON) $(CORESRC) $(LIBS)

test-cross-asan: tests/test_cross.c $(ALLBACKENDS) $(COMMON) $(CORESRC)
	$(CC) $(CFLAGS_ASAN) $(INCLUDE) -o $@ tests/test_cross.c $(ALLBACKENDS) \
	$(COMMON) $(CORESRC) $(LIBS)

weft-accel-bench-asan: bench/weft_accel_bench.c bench/weft_sim_ep.c $(ALLBACKENDS) \
       $(COMMON) $(CORESRC)
	$(CC) $(CFLAGS_ASAN) $(INCLUDE) -o $@ bench/weft_accel_bench.c \
	bench/weft_sim_ep.c $(ALLBACKENDS) $(COMMON) $(CORESRC) $(LIBS)

# ---- evidence pack ---------------------------------------------------------

evidence: all
	mkdir -p $(ROOT)/litmus/evidence/accelerators
	./test-view  2>&1 | tee $(ROOT)/litmus/evidence/accelerators/ac-view.log
	./test-vk    2>&1 | tee $(ROOT)/litmus/evidence/accelerators/ac-vk.log
	./test-metal 2>&1 | tee $(ROOT)/litmus/evidence/accelerators/ac-metal.log
	./test-ort   2>&1 | tee $(ROOT)/litmus/evidence/accelerators/ac-ort.log
	./test-ggml  2>&1 | tee $(ROOT)/litmus/evidence/accelerators/ac-ggml.log
	./test-cross 2>&1 | tee $(ROOT)/litmus/evidence/accelerators/ac-cross.log
	./weft-accel-bench 2>&1 | tee $(ROOT)/litmus/evidence/accelerators/ac-bench.log

# ---- kernel freeze: rebuild the committed .spv and diff --------------------

GLSLANG ?= glslangValidator
spv-check:
	$(GLSLANG) -V --target-env vulkan1.1 \
	  -o /tmp/weft_preprocess.rebuild.spv \
	  backends/vulkan/shaders/weft_preprocess.comp
	cmp /tmp/weft_preprocess.rebuild.spv \
	  backends/vulkan/shaders/weft_preprocess.spv
	@echo "spv: byte-identical rebuild (kernel freeze holds)"

# ---- the apple CI leg (Metal device roads; NOT for linux hosts) ------------

apple: weft_metal_apple_test

weft_metal_apple_test: tests/test_metal.c backends/metal/weft_metal_apple.mm \
                       $(METAL) $(COMMON)
	$(CC) -std=c11 -Wall -Wextra -D_GNU_SOURCE -Iinclude -I$(CORE) \
	  -x objective-c++ -fobjc-arc -o $@ tests/test_metal.c \
	  backends/metal/weft_metal_apple.mm $(METAL) $(COMMON) $(LIBS) \
	  -framework Metal -framework IOSurface -framework CoreVideo \
	  -framework Foundation

clean:
	rm -f test-view test-vk test-metal test-ort test-ggml test-cross \
	      weft-accel-bench test-view-asan test-cross-asan \
	      weft-accel-bench-asan weft_metal_apple_test
