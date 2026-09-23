# core/c/studio/inspector/Makefile — Pillar 7 (Weft Studio native inspector).
#
# Targets:
#   studio-inspector-test{,-asan,-tsan}   I1-I5 / P1-P5 / F batteries
#   studio-torture-test{,-asan,-tsan}     T1-T4: 2M-cycle rapid scrape +
#                                         attach/detach + crash recovery
#   studio-bench{,-asan}                  G1 (inspector CPU < 0.5% on a
#                                         10M msg/s stream) + G2 (100k-slot
#                                         memory-map snapshot p99 < 5 us)
#                                         gates + B1-B4 reports
#   test / test-asan / test-tsan          the full batteries
#
# Every compile is fail-closed: -Wall -Wextra -Werror -pedantic -std=c11
# EXCEPT the frozen baseline weft.c (its owner's flags predate -Werror; it
# is compiled read-only with -Wno-error on the single known redefinition
# note — the git-diff gate in the runner proves the file is untouched).
#
# Link surfaces (read-only baseline consumption, house discipline):
#   * rmw_weft ring + registry (Pillar 6, merged baseline) — the batteries
#     spawn REAL publishers/subscribers over REAL WFRM rings.
#   * cluster IPC chain (weft_ipc + weft_shm + hmac + sha256 + fanout +
#     shm_ring) — the inspector's WFRE discovery calls the real API; the
#     battery drives a real WFSH ring through the real fanout writer.
#   * tests/adapters/native/test_util.c — the P6 test belt (PDEATHSIG
#     children, fd//dev/shm audits, allocator interposition).

CC       = gcc
CFLAGS   = -O2 -std=c11 -Wall -Wextra -Werror -pedantic -pthread -D_GNU_SOURCE
CFLAGS_ASAN = -O1 -g -fsanitize=address,undefined -std=c11 -Wall -Wextra \
              -Werror -pedantic -pthread -D_GNU_SOURCE
CFLAGS_TSAN = -O1 -g -fsanitize=thread -std=c11 -Wall -Wextra -Werror \
              -pedantic -pthread -D_GNU_SOURCE

BUILD    = ../../../../tests/studio/build
NATIVE   = ../../../../tests/studio/native
ADAPT_UT = ../../../../tests/adapters/native

INSPECT_SRC = src/weft_shm_inspector.c src/weft_contention_profiler.c \
              src/weft_memory_stream.c
RMW_SRC  = ../../adapters/rmw_weft/src/rmw_ring.c \
           ../../adapters/rmw_weft/src/rmw_registry.c
# the frozen cluster chain: weft.c alone relaxes -Werror (baseline flags)
IPC_SRC  = ../../weft_shm.c ../../weft_ipc.c ../../shm_ring.c \
           ../../fanout.c ../../fanout_simd.c ../../frame_cursor.c \
           ../../sha256.c ../../sha256_hw.c ../../hmac.c
WEFT_SRC = ../../weft.c
UTIL_SRC = $(ADAPT_UT)/test_util.c

INCLUDES = -Iinclude -I../../adapters/rmw_weft/include -I../.. \
           -I$(ADAPT_UT)

LDLIBS   = -pthread -lrt

# baseline weft.c: compiled with its owner's flag set (no -Werror), the
# runner's frozen-file gate proves zero edits
CFLAGS_WEFT = -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE

all: studio-inspector-test studio-torture-test studio-bench

$(BUILD):
	mkdir -p $(BUILD)

# --- module objects (also used by the runner's namespace gate) ----------

$(BUILD)/weft_shm_inspector.o: src/weft_shm_inspector.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/weft_contention_profiler.o: src/weft_contention_profiler.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/weft_memory_stream.o: src/weft_memory_stream.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

MODULE_OBJS = $(BUILD)/weft_shm_inspector.o \
              $(BUILD)/weft_contention_profiler.o \
              $(BUILD)/weft_memory_stream.o

# baseline weft.c: a SEPARATE object compiled with its owner's flags (no
# -Werror: its unguarded _GNU_SOURCE define predates this pillar). gcc
# applies command-line flags to every file in one invocation, so per-file
# immunity requires a per-file compile; the frozen-file gate proves the
# source is untouched.
$(BUILD)/weft_baseline.o: ../../weft.c | $(BUILD)
	$(CC) $(CFLAGS_WEFT) -I../.. -c $< -o $@
WEFT_OBJ = $(BUILD)/weft_baseline.o

# --- batteries ------------------------------------------------------------

studio-inspector-test: $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(WEFT_OBJ) \
                       $(UTIL_SRC) $(NATIVE)/test_shm_inspector.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BUILD)/studio-inspector-test \
	    $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(UTIL_SRC) \
	    $(NATIVE)/test_shm_inspector.c $(WEFT_OBJ) $(LDLIBS)

studio-inspector-test-asan: $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(WEFT_OBJ) \
                            $(UTIL_SRC) $(NATIVE)/test_shm_inspector.c | $(BUILD)
	$(CC) $(CFLAGS_ASAN) $(INCLUDES) -o $(BUILD)/studio-inspector-test-asan \
	    $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(UTIL_SRC) \
	    $(NATIVE)/test_shm_inspector.c $(WEFT_OBJ) $(LDLIBS)

studio-inspector-test-tsan: $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(WEFT_OBJ) \
                            $(UTIL_SRC) $(NATIVE)/test_shm_inspector.c | $(BUILD)
	$(CC) $(CFLAGS_TSAN) $(INCLUDES) -o $(BUILD)/studio-inspector-test-tsan \
	    $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(UTIL_SRC) \
	    $(NATIVE)/test_shm_inspector.c $(WEFT_OBJ) $(LDLIBS)

studio-torture-test: $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(WEFT_OBJ) \
                     $(UTIL_SRC) $(NATIVE)/test_inspector_torture.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BUILD)/studio-torture-test \
	    $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(UTIL_SRC) \
	    $(NATIVE)/test_inspector_torture.c $(WEFT_OBJ) $(LDLIBS)

studio-torture-test-asan: $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(WEFT_OBJ) \
                          $(UTIL_SRC) $(NATIVE)/test_inspector_torture.c | $(BUILD)
	$(CC) $(CFLAGS_ASAN) $(INCLUDES) -o $(BUILD)/studio-torture-test-asan \
	    $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(UTIL_SRC) \
	    $(NATIVE)/test_inspector_torture.c $(WEFT_OBJ) $(LDLIBS)

studio-torture-test-tsan: $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(WEFT_OBJ) \
                          $(UTIL_SRC) $(NATIVE)/test_inspector_torture.c | $(BUILD)
	$(CC) $(CFLAGS_TSAN) $(INCLUDES) -o $(BUILD)/studio-torture-test-tsan \
	    $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(UTIL_SRC) \
	    $(NATIVE)/test_inspector_torture.c $(WEFT_OBJ) $(LDLIBS)

# --- benchmark ------------------------------------------------------------

studio-bench: $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(WEFT_OBJ) $(UTIL_SRC) \
              $(NATIVE)/bench_inspector.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BUILD)/studio-bench \
	    $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(UTIL_SRC) \
	    $(NATIVE)/bench_inspector.c $(WEFT_OBJ) $(LDLIBS)

studio-bench-asan: $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(WEFT_OBJ) \
                   $(UTIL_SRC) $(NATIVE)/bench_inspector.c | $(BUILD)
	$(CC) $(CFLAGS_ASAN) $(INCLUDES) -o $(BUILD)/studio-bench-asan \
	    $(INSPECT_SRC) $(RMW_SRC) $(IPC_SRC) $(UTIL_SRC) \
	    $(NATIVE)/bench_inspector.c $(WEFT_OBJ) $(LDLIBS)

# --- legs -------------------------------------------------------------------

test: studio-inspector-test studio-torture-test
	$(BUILD)/studio-inspector-test
	$(BUILD)/studio-torture-test

test-asan: studio-inspector-test-asan studio-torture-test-asan
	$(BUILD)/studio-inspector-test-asan
	WEFT_QUICK=1 $(BUILD)/studio-torture-test-asan

test-tsan: studio-inspector-test-tsan studio-torture-test-tsan
	$(BUILD)/studio-inspector-test-tsan
	WEFT_QUICK=1 $(BUILD)/studio-torture-test-tsan

clean:
	rm -f $(BUILD)/studio-* $(BUILD)/*.o

.PHONY: all test test-asan test-tsan clean
