# core/c/spectrum/Makefile — WEFT SPECTRUM native silicon acceleration (P5, D-52).
#
# Targets:
#   spectrum-oracle-test{,-asan,-tsan}     Law 5: all-paths-equal bit-exactness
#                                          (scalar/avx2/avx512 engines, goldens)
#   spectrum-mock-test{,-asan,-tsan}       mandate C: synthetic transport
#                                          conformance (latency/saturation/faults)
#   spectrum-dispatch-test{,-asan,-tsan}   Laws 2/3/4: end-to-end dispatch,
#                                          fallback hops, error ledger
#   spectrum-registry-test{,-asan,-tsan}   admission gates + registry rules
#   spectrum-torture-test{,-asan}          Law 1: 5,000,000-cycle zero-growth
#                                          (3 witnesses; TSan leg skipped —
#                                          the module is single-writer by
#                                          contract, no threads to race)
#   spectrum-bench                        the scoreboard + G1/G2/G3 gates
#   test / test-asan / test-tsan          full batteries (CI drives these)
#
# Fail-closed bar: -Wall -Wextra -Werror -pedantic on EVERY compile.
# Evidence binaries land in tests/spectrum/build/ (gitignored).

CC          = gcc
CFLAGS      = -O2 -std=c11 -Wall -Wextra -Werror -pedantic -pthread -D_GNU_SOURCE
CFLAGS_ASAN = -O1 -g -std=c11 -Wall -Wextra -Werror -pedantic -pthread -D_GNU_SOURCE \
              -fsanitize=address,undefined -fno-sanitize-recover=all
CFLAGS_TSAN = -O1 -g -std=c11 -Wall -Wextra -Werror -pedantic -pthread -D_GNU_SOURCE \
              -fsanitize=thread

BUILD_DIR = ../../../tests/spectrum/build

MODULE_SRC = \
    drivers/backend_registry.c \
    drivers/weft_driver_core.c \
    drivers/driver_qualcomm.c \
    drivers/driver_mediatek.c \
    drivers/driver_apple.c \
    drivers/driver_nvidia_pc.c \
    drivers/driver_riscv_arm.c \
    gpu/weft_gpu_loader.c \
    simd/weft_simd.c \
    simd/weft_simd_x86.c \
    simd/weft_simd_arm.c \
    simd/weft_simd_rvv.c

MOCK_SRC = ../../../tests/spectrum/mock/weft_mock_dma.c
NATIVE_DIR = ../../../tests/spectrum/native

TESTS = spectrum-oracle-test spectrum-mock-test spectrum-dispatch-test \
        spectrum-registry-test spectrum-torture-test

all: $(TESTS) spectrum-bench

# ---------------------------------------------------------------------------
# Test binaries (plain / ASan+UBSan / TSan legs)
# ---------------------------------------------------------------------------

spectrum-oracle-test: $(MODULE_SRC) simd/weft_simd.h
	$(CC) $(CFLAGS) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) \
	    $(NATIVE_DIR)/test_simd_oracle.c -lm

spectrum-oracle-test-asan: $(MODULE_SRC) simd/weft_simd.h
	$(CC) $(CFLAGS_ASAN) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) \
	    $(NATIVE_DIR)/test_simd_oracle.c -lm

spectrum-oracle-test-tsan: $(MODULE_SRC) simd/weft_simd.h
	$(CC) $(CFLAGS_TSAN) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) \
	    $(NATIVE_DIR)/test_simd_oracle.c -lm

spectrum-mock-test: $(MODULE_SRC) $(MOCK_SRC)
	$(CC) $(CFLAGS) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) $(MOCK_SRC) \
	    $(NATIVE_DIR)/test_mock_dma.c -lm

spectrum-mock-test-asan: $(MODULE_SRC) $(MOCK_SRC)
	$(CC) $(CFLAGS_ASAN) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) $(MOCK_SRC) \
	    $(NATIVE_DIR)/test_mock_dma.c -lm

spectrum-mock-test-tsan: $(MODULE_SRC) $(MOCK_SRC)
	$(CC) $(CFLAGS_TSAN) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) $(MOCK_SRC) \
	    $(NATIVE_DIR)/test_mock_dma.c -lm

spectrum-dispatch-test: $(MODULE_SRC) $(MOCK_SRC)
	$(CC) $(CFLAGS) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) $(MOCK_SRC) \
	    $(NATIVE_DIR)/test_spectrum_dispatch.c -lm

spectrum-dispatch-test-asan: $(MODULE_SRC) $(MOCK_SRC)
	$(CC) $(CFLAGS_ASAN) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) $(MOCK_SRC) \
	    $(NATIVE_DIR)/test_spectrum_dispatch.c -lm

spectrum-dispatch-test-tsan: $(MODULE_SRC) $(MOCK_SRC)
	$(CC) $(CFLAGS_TSAN) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) $(MOCK_SRC) \
	    $(NATIVE_DIR)/test_spectrum_dispatch.c -lm

spectrum-registry-test: $(MODULE_SRC)
	$(CC) $(CFLAGS) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) \
	    $(NATIVE_DIR)/test_spectrum_registry.c -lm

spectrum-registry-test-asan: $(MODULE_SRC)
	$(CC) $(CFLAGS_ASAN) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) \
	    $(NATIVE_DIR)/test_spectrum_registry.c -lm

spectrum-registry-test-tsan: $(MODULE_SRC)
	$(CC) $(CFLAGS_TSAN) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) \
	    $(NATIVE_DIR)/test_spectrum_registry.c -lm

spectrum-torture-test: $(MODULE_SRC) $(MOCK_SRC)
	$(CC) $(CFLAGS) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) $(MOCK_SRC) \
	    $(NATIVE_DIR)/test_spectrum_torture.c -lm

spectrum-torture-test-asan: $(MODULE_SRC) $(MOCK_SRC)
	$(CC) $(CFLAGS_ASAN) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) $(MOCK_SRC) \
	    $(NATIVE_DIR)/test_spectrum_torture.c -lm

spectrum-bench: $(MODULE_SRC) $(MOCK_SRC)
	$(CC) $(CFLAGS) -I. -Idrivers -o $(BUILD_DIR)/$@ $(MODULE_SRC) $(MOCK_SRC) \
	    ../../../tests/spectrum/bench_spectrum_native.c -lm

# ---------------------------------------------------------------------------
# Batteries (the CI shard + the canonical runner drive these)
# ---------------------------------------------------------------------------

test: $(TESTS)
	$(BUILD_DIR)/spectrum-oracle-test
	$(BUILD_DIR)/spectrum-mock-test
	$(BUILD_DIR)/spectrum-dispatch-test
	$(BUILD_DIR)/spectrum-registry-test
	$(BUILD_DIR)/spectrum-torture-test

test-asan: spectrum-oracle-test-asan spectrum-mock-test-asan \
           spectrum-dispatch-test-asan spectrum-registry-test-asan \
           spectrum-torture-test-asan
	$(BUILD_DIR)/spectrum-oracle-test-asan
	$(BUILD_DIR)/spectrum-mock-test-asan
	$(BUILD_DIR)/spectrum-dispatch-test-asan
	$(BUILD_DIR)/spectrum-registry-test-asan
	$(BUILD_DIR)/spectrum-torture-test-asan

test-tsan: spectrum-oracle-test-tsan spectrum-mock-test-tsan \
           spectrum-dispatch-test-tsan spectrum-registry-test-tsan
	$(BUILD_DIR)/spectrum-oracle-test-tsan
	$(BUILD_DIR)/spectrum-mock-test-tsan
	$(BUILD_DIR)/spectrum-dispatch-test-tsan
	$(BUILD_DIR)/spectrum-registry-test-tsan

clean:
	rm -f $(BUILD_DIR)/spectrum-*

.PHONY: all test test-asan test-tsan clean
