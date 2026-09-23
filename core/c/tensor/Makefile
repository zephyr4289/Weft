# core/c/tensor/Makefile — RFC-0021 weft-tensor fabric (Pillar 2).
#
# Targets:
#   tensor-view-test{,-asan}       WT1-16: strided geometry conformance +
#                                  the Python-oracle golden fixture
#   tensor-arena-test{,-asan}      WT17-24: paged arena conformance +
#                                  measured ns/alloc
#   tensor-ring-test{,-asan,-tsan} WT25-40: ring conformance + MPSC/SPMC/
#                                  MPMC tearing stress + fork torture +
#                                  throughput fixture (--bench heavy leg)
#   test / test-asan / test-tsan   the full batteries (the CI shard drives
#                                  these; TSAN leg skips fork — declared)
#
# Heavy evidence legs: run the -O2 binaries with --bench (1M arena
# allocs; 50K x 16KiB verified ring messages).
#
# Zero kernel surface: everything lives under core/c/tensor/; no frozen
# file is compiled or touched here.

CC       = gcc
CFLAGS   = -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE
CFLAGS_ASAN = -O1 -g -fsanitize=address -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE
CFLAGS_TSAN = -O1 -g -fsanitize=thread -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE

TENSOR_SRC = weft_tensor_view.c weft_tensor_arena.c weft_tensor_ring.c

all: tensor-view-test tensor-arena-test tensor-ring-test

tensor-view-test: $(TENSOR_SRC) weft_tensor.h tests/test_tensor_view.c
	$(CC) $(CFLAGS) -I. -o tensor-view-test $(TENSOR_SRC) tests/test_tensor_view.c

tensor-view-test-asan: $(TENSOR_SRC) weft_tensor.h tests/test_tensor_view.c
	$(CC) $(CFLAGS_ASAN) -I. -o tensor-view-test-asan $(TENSOR_SRC) tests/test_tensor_view.c

tensor-arena-test: $(TENSOR_SRC) weft_tensor.h tests/test_tensor_arena.c
	$(CC) $(CFLAGS) -I. -o tensor-arena-test $(TENSOR_SRC) tests/test_tensor_arena.c

tensor-arena-test-asan: $(TENSOR_SRC) weft_tensor.h tests/test_tensor_arena.c
	$(CC) $(CFLAGS_ASAN) -I. -o tensor-arena-test-asan $(TENSOR_SRC) tests/test_tensor_arena.c

tensor-ring-test: $(TENSOR_SRC) weft_tensor.h tests/test_tensor_ring.c
	$(CC) $(CFLAGS) -I. -o tensor-ring-test $(TENSOR_SRC) tests/test_tensor_ring.c

tensor-ring-test-asan: $(TENSOR_SRC) weft_tensor.h tests/test_tensor_ring.c
	$(CC) $(CFLAGS_ASAN) -I. -o tensor-ring-test-asan $(TENSOR_SRC) tests/test_tensor_ring.c

tensor-ring-test-tsan: $(TENSOR_SRC) weft_tensor.h tests/test_tensor_ring.c
	$(CC) $(CFLAGS_TSAN) -I. -o tensor-ring-test-tsan $(TENSOR_SRC) tests/test_tensor_ring.c

# Heavy evidence legs: the same -O2 binaries invoked with --bench
# (1M arena allocs; 50K x 16KiB verified ring messages).
test: tensor-view-test tensor-arena-test tensor-ring-test
	./tensor-view-test
	./tensor-arena-test
	./tensor-ring-test

test-asan: tensor-view-test-asan tensor-arena-test-asan tensor-ring-test-asan
	./tensor-view-test-asan
	./tensor-arena-test-asan
	./tensor-ring-test-asan

test-tsan: tensor-ring-test-tsan
	WT_SKIP_FORK=1 ./tensor-ring-test-tsan

clean:
	rm -f tensor-view-test tensor-view-test-asan tensor-arena-test \
              tensor-arena-test-asan tensor-ring-test tensor-ring-test-asan \
              tensor-ring-test-tsan

.PHONY: all test test-asan test-tsan clean
