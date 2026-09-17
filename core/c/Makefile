# core/c/Makefile — local C build target (also driven by the root Makefile)
#
# Kernel targets (weft.c/weft.h — frozen):
#   spike, spike-tsan            — the kernel litmus runner (unchanged)
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

FANOUT_SRC = fanout.c frame_cursor.c weft.c

all: $(TARGET)

$(TARGET): weft.c litmus_runner.c weft.h
	$(CC) $(CFLAGS) -o $(TARGET) weft.c litmus_runner.c

$(TARGET_TSAN): weft.c litmus_runner.c weft.h
	$(CC) $(CFLAGS_DBG) -o $(TARGET_TSAN) weft.c litmus_runner.c

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
	$(CC) $(CFLAGS) -o wire-probe wire_probe.c fanout.c frame_cursor.c weft.c

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

verified-runner: sha256.c sha256_hw.c hmac.c verified.c verified_runner.c weft.c weft.h sha256.h hmac.h verified.h
	$(CC) $(CFLAGS) $(VW_CFLAGS) -o verified-runner $(VW_SRC) verified_runner.c

verified-runner-asan: sha256.c sha256_hw.c hmac.c verified.c verified_runner.c weft.c weft.h sha256.h hmac.h verified.h
	$(CC) $(CFLAGS_ASAN) $(VW_CFLAGS) -o verified-runner-asan $(VW_SRC) verified_runner.c

# ---- Inter-process shared-memory rings (RFC-0011 draft) -------------------
# shm_ring.{h,c}: POSIX named/anonymous + Windows-gated session protocol over
# the RFC-0004 fan-out ring. S-series conformance (fork torture included)
# + the IPC torture/xlang runner. Linux link note: shm_open lives in librt
# on glibc < 2.17 and in libc since; -lrt is accepted and harmless either way.
SHM_SRC = shm_ring.c fanout.c weft.c

shm-test: shm_ring.c shm_ring.h shm_test.c $(SHM_SRC) fanout.h weft.h
	$(CC) $(CFLAGS) -o shm-test $(SHM_SRC) shm_test.c -lrt -lpthread

shm-test-asan: shm_ring.c shm_ring.h shm_test.c $(SHM_SRC) fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o shm-test-asan $(SHM_SRC) shm_test.c -lrt -lpthread

shm-runner: shm_ring.c shm_ring.h shm_runner.c $(SHM_SRC) fanout.h weft.h
	$(CC) $(CFLAGS) -o shm-runner $(SHM_SRC) shm_runner.c -lrt -lpthread

shm-runner-asan: shm_ring.c shm_ring.h shm_runner.c $(SHM_SRC) fanout.h weft.h
	$(CC) $(CFLAGS_ASAN) -o shm-runner-asan $(SHM_SRC) shm_runner.c -lrt -lpthread

governor-test: governor.c governor_test.c governor.h
	$(CC) $(CFLAGS) -o governor-test governor.c governor_test.c

governor-test-asan: governor.c governor_test.c governor.h
	$(CC) $(CFLAGS_ASAN) -o governor-test-asan governor.c governor_test.c

clean:
	rm -f $(TARGET) $(TARGET_TSAN) governor-test governor-test-asan fanout-test fanout-test-asan fanout-test-tsan \
	      fanout-test-seq fanout-runner fanout-runner-asan fanout-runner-tsan fanout-runner-seq \
	      verified-test verified-test-asan verified-runner verified-runner-asan \
	      verified-mb-test verified-mb-test-asan \
	      shm-test shm-test-asan shm-runner shm-runner-asan

.PHONY: all clean
