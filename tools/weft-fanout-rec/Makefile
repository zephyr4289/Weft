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
LIBS     = -lm
FANOUT   = $(ROOT)/core/c/fanout.c
# Issue #17-1: fanout.c's claim seam links fanout_simd.c (SIMD copy
# dispatcher; scalar fallback when silicon refuses).
FSIMD    = $(ROOT)/core/c/fanout_simd.c
SHMRING  = $(ROOT)/core/c/shm_ring.c
SRC      = weft_fanout_rec.c

.PHONY: all test test-seq e2e clean

# e2e — RFC-0010 demo gate: capture -> validate -> replay -> recapture under
# v3 compression, wave family (the road the lead's roadmap named).
e2e: weft-fanout-rec
	bash e2e.sh

all: weft-fanout-rec weft-fanout-rec-seq

weft-fanout-rec: $(SRC) $(FANOUT) $(SHMRING) $(ROOT)/core/c/fanout.h $(ROOT)/core/c/shm_ring.h
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $(SRC) $(FANOUT) $(FSIMD) $(SHMRING) $(LIBS) -lrt

weft-fanout-rec-seq: $(SRC) $(FANOUT) $(SHMRING) $(ROOT)/core/c/fanout.h $(ROOT)/core/c/shm_ring.h
	$(CC) $(CFLAGS) -DWEFT_FANOUT_SEQ_CST=1 $(INCLUDE) -o $@ $(SRC) $(FANOUT) $(FSIMD) $(SHMRING) $(LIBS) -lrt

weft-fanout-rec-asan: $(SRC) $(FANOUT) $(SHMRING) $(ROOT)/core/c/fanout.h $(ROOT)/core/c/shm_ring.h
	$(CC) $(CFLAGS_ASAN) $(INCLUDE) -o $@ $(SRC) $(FANOUT) $(FSIMD) $(SHMRING) $(LIBS)

weft-fanout-rec-tsan: $(SRC) $(FANOUT) $(SHMRING) $(ROOT)/core/c/fanout.h $(ROOT)/core/c/shm_ring.h
	$(CC) $(CFLAGS_DBG) $(INCLUDE) -o $@ $(SRC) $(FANOUT) $(FSIMD) $(SHMRING) $(LIBS)

test: weft-fanout-rec
	./weft-fanout-rec selftest --frames 20000 --words 64

test-seq: weft-fanout-rec-seq
	./weft-fanout-rec-seq selftest --frames 20000 --words 64

clean:
	rm -f weft-fanout-rec weft-fanout-rec-seq weft-fanout-rec-asan \
	      weft-fanout-rec-tsan selftest_a.weftrec selftest_b.weftrec
