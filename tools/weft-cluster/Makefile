# weft-cluster — RFC-0019: the kernel-bypass cluster pillar (Pillar 3).
#
# Builds against the frozen driver layer (core/c, read-only: shm_ring +
# fanout for the WFSH bridge) with ZERO link-time dependencies on
# optional runtimes — libibverbs is dlopen'd at runtime, io_uring/BPF/XDP
# go through raw syscalls (the uring_rx house rule, RFC-0012).
#
# Targets (house naming, the CL-series gates):
#   test-rdma / test-xdp / test-uring / test-fabric
#        the gate batteries (mock vtable + honest refusals + live legs
#        where the runner allows them)
#   weft-cluster-bench
#        the multi-road latency scoreboard (RFC-0019 §7)
#   asan — the ASAN legs (NO malloc-audit interposer: ASAN owns malloc;
#        compiling both in is a conflict)
#   evidence — run every gate + bench, tee to litmus/evidence/cluster
#   test — all gates, in order, fail-fast

CC      ?= cc
ROOT     = ../..
CORE     = $(ROOT)/core/c
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE
CFLAGS_ASAN = -O1 -g -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE \
              -fsanitize=address,undefined
INCLUDE  = -Iinclude -Isrc -I. -Ibackends/rdma -Ibackends/xdp \
           -Ibackends/uring -Ibackends/loopback -Ifabric -Itests -I$(CORE)
LIBS     = -ldl

# the seam sources (WCR1 contract + WCF1 frame + shared plumbing)
COMMON   = src/weft_wcr1.c src/weft_cluster_frame.c src/weft_cluster_core.c
# core/c substrate the WFSH bridge needs (read-only; kernel freeze holds)
CORESRC  = $(CORE)/shm_ring.c $(CORE)/fanout.c $(CORE)/fanout_simd.c
# the engines
RDMA     = backends/rdma/weft_rdma_driver.c
XDP      = backends/xdp/weft_xdp_driver.c
URING    = backends/uring/weft_uring_driver.c
LOOP     = backends/loopback/weft_loopback.c
FABRIC   = fabric/weft_cluster_fabric.c $(LOOP) $(RDMA) $(XDP) $(URING)
# the Law-1 malloc-audit interposer (NOT in ASAN legs)
AUDIT    = tests/weft_audit.c

.PHONY: all test asan evidence clean

all: test-rdma test-xdp test-uring test-fabric weft-cluster-bench

test: all
	./test-rdma && ./test-xdp && ./test-uring && ./test-fabric

# ---- the CL-series gates (O2 + audit interposer) ---------------------------

test-rdma: tests/test_rdma.c $(RDMA) $(COMMON) $(CORESRC) $(AUDIT)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ tests/test_rdma.c $(RDMA) $(COMMON) $(CORESRC) $(AUDIT) $(LIBS)

test-xdp: tests/test_xdp.c $(XDP) $(COMMON) $(CORESRC) $(AUDIT)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ tests/test_xdp.c $(XDP) $(COMMON) $(CORESRC) $(AUDIT) $(LIBS)

test-uring: tests/test_uring.c $(URING) $(COMMON) $(CORESRC) $(AUDIT)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ tests/test_uring.c $(URING) $(COMMON) $(CORESRC) $(AUDIT) $(LIBS)

test-fabric: tests/test_fabric.c $(FABRIC) $(COMMON) $(CORESRC) $(AUDIT)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ tests/test_fabric.c $(FABRIC) $(COMMON) $(CORESRC) $(AUDIT) $(LIBS)

# ---- the scoreboard ---------------------------------------------------------

weft-cluster-bench: bench/weft_cluster_bench.c $(FABRIC) $(COMMON) $(CORESRC) $(AUDIT)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ bench/weft_cluster_bench.c $(FABRIC) $(COMMON) $(CORESRC) $(AUDIT) $(LIBS)

# ---- ASAN legs (no audit interposer — ASAN owns malloc) --------------------

test-rdma-asan: tests/test_rdma.c $(RDMA) $(COMMON) $(CORESRC) tests/weft_audit_stub.c
	$(CC) $(CFLAGS_ASAN) $(INCLUDE) -o $@ tests/test_rdma.c $(RDMA) $(COMMON) $(CORESRC) tests/weft_audit_stub.c $(LIBS)

test-uring-asan: tests/test_uring.c $(URING) $(COMMON) $(CORESRC) tests/weft_audit_stub.c
	$(CC) $(CFLAGS_ASAN) $(INCLUDE) -o $@ tests/test_uring.c $(URING) $(COMMON) $(CORESRC) tests/weft_audit_stub.c $(LIBS)

test-fabric-asan: tests/test_fabric.c $(FABRIC) $(COMMON) $(CORESRC) tests/weft_audit_stub.c
	$(CC) $(CFLAGS_ASAN) $(INCLUDE) -o $@ tests/test_fabric.c $(FABRIC) $(COMMON) $(CORESRC) tests/weft_audit_stub.c $(LIBS)

asan: test-rdma-asan test-uring-asan test-fabric-asan
	./test-rdma-asan && ./test-uring-asan && ./test-fabric-asan

# ---- evidence pack -----------------------------------------------------------

evidence: all
	mkdir -p $(ROOT)/litmus/evidence/cluster
	./test-rdma  2>&1 | tee $(ROOT)/litmus/evidence/cluster/cl-rdma.log
	./test-xdp   2>&1 | tee $(ROOT)/litmus/evidence/cluster/cl-xdp.log
	./test-uring 2>&1 | tee $(ROOT)/litmus/evidence/cluster/cl-uring.log
	./test-fabric 2>&1 | tee $(ROOT)/litmus/evidence/cluster/cl-fabric.log
	./weft-cluster-bench --json $(ROOT)/litmus/evidence/cluster/cl-bench.json 2>&1 | tee $(ROOT)/litmus/evidence/cluster/cl-bench.log

clean:
	rm -f test-rdma test-xdp test-uring test-fabric weft-cluster-bench test-rdma-asan test-uring-asan test-fabric-asan
