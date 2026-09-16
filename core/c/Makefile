# core/c/Makefile — local C build target (also driven by the root Makefile)
#
# Kernel targets (weft.c/weft.h — frozen):
#   spike, spike-tsan            — the kernel litmus runner (unchanged)
# Fan-out driver-layer targets (fanout.{h,c}, frame_cursor.{h,c} — RFC 0004/0008):
#   fanout-test{,-asan,-tsan,-seq}   — F-series + FrameCursor conformance
#   fanout-runner{,-asan,-tsan,-seq} — torture + xlang dump/validate runner
# The -seq variants build with -DWEFT_FANOUT_SEQ_CST=1 (the TS-equivalent
# all-seq_cst stamp regime) so both ordering regimes are torture-gated.

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

clean:
	rm -f $(TARGET) $(TARGET_TSAN) fanout-test fanout-test-asan fanout-test-tsan \
	      fanout-test-seq fanout-runner fanout-runner-asan fanout-runner-tsan fanout-runner-seq

.PHONY: all clean
