# core/c/adapters/Makefile — Pillar 6 (weft-adapters native middleware).
#
# Targets:
#   rmw-weft-test{,-asan,-tsan}      R1-R6: multi-process battery (zero-copy
#                                    identity, saturation, fan-out, sweep)
#   vision-dma-test{,-asan,-tsan}    V1-V6: 4K@120 mock camera, zero-heap
#                                    streaming, DLPack + dmabuf seams
#   adapters-torture-test{,-asan,-tsan}
#                                    T1-T4: 2M-cycle zero-growth torture
#   adapters-bench{,-asan}           G1 (loaned RTT p99) + G2 (vision
#                                    handoff p99) gates + B2/B4 reports
#   test / test-asan / test-tsan     the full batteries
#
# Every compile is fail-closed: -Wall -Wextra -Werror -pedantic -std=c11.
# The vision plain leg interposes the allocator (--wrap) to prove Law 1.
#
# Zero frozen-file surface: compiles the additive adapter tree plus the
# BASELINE frozen tensor view source (core/c/tensor/weft_tensor_view.c,
# read-only) — no frozen file is edited (git-diff gate in the runner).

CC       = gcc
CFLAGS   = -O2 -std=c11 -Wall -Wextra -Werror -pedantic -pthread -D_GNU_SOURCE
CFLAGS_ASAN = -O1 -g -fsanitize=address,undefined -std=c11 -Wall -Wextra \
              -Werror -pedantic -pthread -D_GNU_SOURCE
CFLAGS_TSAN = -O1 -g -fsanitize=thread -std=c11 -Wall -Wextra -Werror \
              -pedantic -pthread -D_GNU_SOURCE

BUILD    = ../../../tests/adapters/build
NATIVE   = ../../../tests/adapters/native
MOCKDIR  = ../../../tests/adapters/mock
BENCHDIR = ../../../tests/adapters

RMW_SRC  = rmw_weft/src/rmw_ring.c rmw_weft/src/rmw_registry.c \
           rmw_weft/src/rmw_weft.c
VIS_SRC  = vision/src/vision_dma.c vision/src/v4l2_backend.c
TENSOR_SRC = ../tensor/weft_tensor_view.c
MOCK_SRC  = $(MOCKDIR)/v4l2_mock_device.c
UTIL_SRC  = $(NATIVE)/test_util.c

INCLUDES = -Irmw_weft/include -Irmw_weft/compat/include -I$(NATIVE) \
           -Ivision/include -I../tensor -I$(MOCKDIR)

HAVE_V4L2 := $(shell printf '#include <linux/videodev2.h>\n' \
              | $(CC) -E -x c - >/dev/null 2>&1 && echo 1 || echo 0)
CFLAGS   += -DWEFT_HAVE_V4L2=$(HAVE_V4L2)
CFLAGS_ASAN += -DWEFT_HAVE_V4L2=$(HAVE_V4L2)
CFLAGS_TSAN += -DWEFT_HAVE_V4L2=$(HAVE_V4L2)

LINK_GUARD = -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc
LDLIBS   = -pthread -lrt

all: rmw-weft-test vision-dma-test adapters-torture-test adapters-bench

$(BUILD):
	mkdir -p $(BUILD)

# --- rmw battery (multi-process fork; three legs) --------------------------

rmw-weft-test: $(RMW_SRC) $(UTIL_SRC) $(NATIVE)/test_rmw_weft.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BUILD)/rmw-weft-test $(RMW_SRC) \
	    $(UTIL_SRC) $(NATIVE)/test_rmw_weft.c $(LDLIBS)

rmw-weft-test-asan: $(RMW_SRC) $(UTIL_SRC) $(NATIVE)/test_rmw_weft.c | $(BUILD)
	$(CC) $(CFLAGS_ASAN) $(INCLUDES) -o $(BUILD)/rmw-weft-test-asan \
	    $(RMW_SRC) $(UTIL_SRC) $(NATIVE)/test_rmw_weft.c $(LDLIBS)

rmw-weft-test-tsan: $(RMW_SRC) $(UTIL_SRC) $(NATIVE)/test_rmw_weft.c | $(BUILD)
	$(CC) $(CFLAGS_TSAN) $(INCLUDES) -o $(BUILD)/rmw-weft-test-tsan \
	    $(RMW_SRC) $(UTIL_SRC) $(NATIVE)/test_rmw_weft.c $(LDLIBS)

# --- vision battery (plain leg interposes the allocator: Law 1 proof) ------

vision-dma-test: $(VIS_SRC) $(TENSOR_SRC) $(MOCK_SRC) $(UTIL_SRC) \
                 $(NATIVE)/test_vision_dma.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -DTU_ALLOC_GUARD=1 \
	    -o $(BUILD)/vision-dma-test $(VIS_SRC) $(TENSOR_SRC) $(MOCK_SRC) \
	    $(UTIL_SRC) $(NATIVE)/test_vision_dma.c $(LINK_GUARD) $(LDLIBS)

vision-dma-test-asan: $(VIS_SRC) $(TENSOR_SRC) $(MOCK_SRC) $(UTIL_SRC) \
                      $(NATIVE)/test_vision_dma.c | $(BUILD)
	$(CC) $(CFLAGS_ASAN) $(INCLUDES) -DTU_NO_ALLOC_GUARD=1 \
	    -o $(BUILD)/vision-dma-test-asan $(VIS_SRC) $(TENSOR_SRC) \
	    $(MOCK_SRC) $(UTIL_SRC) $(NATIVE)/test_vision_dma.c $(LDLIBS)

vision-dma-test-tsan: $(VIS_SRC) $(TENSOR_SRC) $(MOCK_SRC) $(UTIL_SRC) \
                      $(NATIVE)/test_vision_dma.c | $(BUILD)
	$(CC) $(CFLAGS_TSAN) $(INCLUDES) -DTU_NO_ALLOC_GUARD=1 \
	    -o $(BUILD)/vision-dma-test-tsan $(VIS_SRC) $(TENSOR_SRC) \
	    $(MOCK_SRC) $(UTIL_SRC) $(NATIVE)/test_vision_dma.c $(LDLIBS)

# --- torture battery --------------------------------------------------------

adapters-torture-test: $(RMW_SRC) $(UTIL_SRC) \
                       $(NATIVE)/test_adapters_torture.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BUILD)/adapters-torture-test \
	    $(RMW_SRC) $(UTIL_SRC) $(NATIVE)/test_adapters_torture.c $(LDLIBS)

adapters-torture-test-asan: $(RMW_SRC) $(UTIL_SRC) \
                            $(NATIVE)/test_adapters_torture.c | $(BUILD)
	$(CC) $(CFLAGS_ASAN) $(INCLUDES) -o $(BUILD)/adapters-torture-test-asan \
	    $(RMW_SRC) $(UTIL_SRC) $(NATIVE)/test_adapters_torture.c $(LDLIBS)

adapters-torture-test-tsan: $(RMW_SRC) $(UTIL_SRC) \
                            $(NATIVE)/test_adapters_torture.c | $(BUILD)
	$(CC) $(CFLAGS_TSAN) $(INCLUDES) -o $(BUILD)/adapters-torture-test-tsan \
	    $(RMW_SRC) $(UTIL_SRC) $(NATIVE)/test_adapters_torture.c $(LDLIBS)

# --- benchmark (plain leg gates; sanitizer legs run with BENCH_NO_GATES=1) --

adapters-bench: $(RMW_SRC) $(VIS_SRC) $(TENSOR_SRC) $(MOCK_SRC) $(UTIL_SRC) \
                $(BENCHDIR)/bench_adapters_native.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BUILD)/adapters-bench $(RMW_SRC) \
	    $(VIS_SRC) $(TENSOR_SRC) $(MOCK_SRC) $(UTIL_SRC) \
	    $(BENCHDIR)/bench_adapters_native.c $(LDLIBS)

adapters-bench-asan: $(RMW_SRC) $(VIS_SRC) $(TENSOR_SRC) $(MOCK_SRC) \
                     $(UTIL_SRC) $(BENCHDIR)/bench_adapters_native.c | $(BUILD)
	$(CC) $(CFLAGS_ASAN) $(INCLUDES) -o $(BUILD)/adapters-bench-asan \
	    $(RMW_SRC) $(VIS_SRC) $(TENSOR_SRC) $(MOCK_SRC) $(UTIL_SRC) \
	    $(BENCHDIR)/bench_adapters_native.c $(LDLIBS)

# --- batteries --------------------------------------------------------------

test: rmw-weft-test vision-dma-test adapters-torture-test
	$(BUILD)/rmw-weft-test
	$(BUILD)/vision-dma-test
	$(BUILD)/adapters-torture-test

test-asan: rmw-weft-test-asan vision-dma-test-asan adapters-torture-test-asan
	$(BUILD)/rmw-weft-test-asan
	$(BUILD)/vision-dma-test-asan
	WEFT_QUICK=1 TORTURE_RSS_LIMIT_KIB=8192 $(BUILD)/adapters-torture-test-asan

test-tsan: rmw-weft-test-tsan vision-dma-test-tsan adapters-torture-test-tsan
	$(BUILD)/rmw-weft-test-tsan
	$(BUILD)/vision-dma-test-tsan
	WEFT_QUICK=1 TORTURE_RSS_LIMIT_KIB=16384 $(BUILD)/adapters-torture-test-tsan

clean:
	rm -f $(BUILD)/rmw-weft-test* $(BUILD)/vision-dma-test* \
	      $(BUILD)/adapters-torture-test* $(BUILD)/adapters-bench*

.PHONY: all test test-asan test-tsan clean
