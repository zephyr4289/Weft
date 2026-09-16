# weft-fanout-rec — RFC-0004 flight-recorder capture/replay (.weftrec v2)
#
# Builds against the canonical C fan-out ring (core/c/fanout.{h,c}) — the
# byte-compatible interop target. Targets mirror core/c/Makefile's naming:
#   weft-fanout-rec            — O2 default (fenced acq/rel regime)
#   weft-fanout-rec-seq        — all-seq_cst A/B regime
#   weft-fanout-rec-asan       — ASAN
#   weft-fanout-rec-tsan       — TSAN (the ring is TSAN-clean; the tool's
#                                pthreads + shm path gets the same gate)
#   test / test-seq            — the end-to-end selftest gate

CC      ?= cc
ROOT     = ../..
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE
CFLAGS_DBG = -O1 -g -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE -fsanitize=thread
CFLAGS_ASAN = -O1 -g -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE -fsanitize=address
INCLUDE  = -I$(ROOT)/core/c
FANOUT   = $(ROOT)/core/c/fanout.c
SRC      = weft_fanout_rec.c

.PHONY: all test test-seq clean

all: weft-fanout-rec weft-fanout-rec-seq

weft-fanout-rec: $(SRC) $(FANOUT) $(ROOT)/core/c/fanout.h
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $(SRC) $(FANOUT)

weft-fanout-rec-seq: $(SRC) $(FANOUT) $(ROOT)/core/c/fanout.h
	$(CC) $(CFLAGS) -DWEFT_FANOUT_SEQ_CST=1 $(INCLUDE) -o $@ $(SRC) $(FANOUT)

weft-fanout-rec-asan: $(SRC) $(FANOUT) $(ROOT)/core/c/fanout.h
	$(CC) $(CFLAGS_ASAN) $(INCLUDE) -o $@ $(SRC) $(FANOUT)

weft-fanout-rec-tsan: $(SRC) $(FANOUT) $(ROOT)/core/c/fanout.h
	$(CC) $(CFLAGS_DBG) $(INCLUDE) -o $@ $(SRC) $(FANOUT)

test: weft-fanout-rec
	./weft-fanout-rec selftest --frames 20000 --words 64

test-seq: weft-fanout-rec-seq
	./weft-fanout-rec-seq selftest --frames 20000 --words 64

clean:
	rm -f weft-fanout-rec weft-fanout-rec-seq weft-fanout-rec-asan \
	      weft-fanout-rec-tsan selftest_a.weftrec selftest_b.weftrec
