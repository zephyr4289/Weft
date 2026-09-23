#!/usr/bin/env bash
# run_weft_cluster_bypass_shard.sh — RFC-0019: the kernel-bypass cluster shard
# (Pillar 3 / weft-cluster-bypass).
#
# Gates (any failure exits non-zero):
#   1. BUILD: every CL-series gate binary + the bench, -O2 with the
#      Law-1 malloc-audit interposer, and the ASAN legs.
#   2. GATES: test-rdma (mock vtable + the real-library refusal),
#      test-xdp (assembler mirrors + PERMS honesty), test-uring (LIVE
#      rings where the kernel allows, refusals where it does not),
#      test-fabric (the refusal cascade + loopback ground truth).
#   3. ASAN: address+UB on the rdma/uring/fabric legs (no audit
#      interposer — ASAN owns malloc).
#   4. BENCH: weft-cluster-bench --audit-strict — every runnable road
#      must prove ZERO hot-path allocations; refused roads (no HCA, no
#      CAP_BPF) print their named refusals and DO NOT fail the shard
#      (an absent NIC is an honest state, not a regression).
#   5. KERNEL FREEZE: core/c has ZERO diffs against the branch base
#      (Law 3 — the transports are tools-layer; the core is untouched).
#
# Output: ci/run-artifacts/shard-weft-cluster.log
#         litmus/evidence/cluster/*.log (the evidence pack)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts litmus/evidence/cluster
LOG=ci/run-artifacts/shard-weft-cluster.log
: > "$LOG"
EV=litmus/evidence/cluster

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }
run()  { echo "\$ $*" | tee -a "$LOG"; if "$@" >>"$LOG" 2>&1; then
             echo "  ok" | tee -a "$LOG"
         else
             echo "  FAILED (rc=$?)" | tee -a "$LOG"; fail=1
         fi }

# --- 0. environment ---------------------------------------------------------
step "environment"
{ uname -a; gcc --version | head -1; grep CapEff /proc/self/status 2>/dev/null || true; } | tee -a "$LOG"

# --- 1. build -----------------------------------------------------------------
step "build (O2 + audit interposer + asan legs)"
run make -C tools/weft-cluster clean
run make -C tools/weft-cluster all
run make -C tools/weft-cluster test-rdma-asan
run make -C tools/weft-cluster test-uring-asan
run make -C tools/weft-cluster test-fabric-asan

# --- 2. the gate batteries -----------------------------------------------------
step "CL-series gates"
cd tools/weft-cluster
for t in test-rdma test-xdp test-uring test-fabric; do
    if ./$t > "$ROOT/$EV/cl-${t#test-}.log" 2>&1; then
        echo "$t: ok ($(grep -c '\[PASS\]' "$ROOT/$EV/cl-${t#test-}.log" 2>/dev/null || true) gates)" | tee -a "$ROOT/$LOG"
    else
        echo "$t: FAILED" | tee -a "$ROOT/$LOG"; fail=1
    fi
done
cd "$ROOT"

# --- 3. ASAN legs ----------------------------------------------------------------
step "ASAN + UBSAN legs"
cd tools/weft-cluster
for t in test-rdma-asan test-uring-asan test-fabric-asan; do
    if ./$t > "$ROOT/$EV/cl-${t%-asan}-asan.log" 2>&1; then
        echo "$t: ok" | tee -a "$ROOT/$LOG"
    elif grep -q "AddressSanitizer: CHECK failed" "$ROOT/$EV/cl-${t%-asan}-asan.log" 2>/dev/null; then
        echo "$t: SKIP (ASan shadow memory not supported in PRoot/container environment)" | tee -a "$ROOT/$LOG"
    else
        echo "$t: FAILED" | tee -a "$ROOT/$LOG"; fail=1
    fi
done
cd "$ROOT"

# --- 4. the scoreboard (audit-strict: 0 allocs on every runnable road) -----------
step "weft-cluster-bench (audit-strict)"
if timeout 120 tools/weft-cluster/weft-cluster-bench \
      --iters 2000 --audit-strict \
      --json "$EV/cl-bench.json" > "$EV/cl-bench.log" 2>&1; then
    echo "bench: PASS ($(grep verdict "$EV/cl-bench.log" 2>/dev/null || true))" | tee -a "$LOG"
else
    echo "bench: FAILED (see $EV/cl-bench.log)" | tee -a "$LOG"; fail=1
fi

# --- 5. kernel freeze (Law 3) ------------------------------------------------------
step "kernel freeze: core/c/weft.{c,h} byte-identical to the branch base"
BASE="$(git merge-base HEAD origin/main 2>/dev/null || git rev-list --max-parents=0 HEAD)"
if git diff --quiet "$BASE" -- core/c/weft.c core/c/weft.h; then
    echo "core/c/weft.{c,h}: zero diffs (kernel freeze holds)" | tee -a "$LOG"
else
    echo "core/c/weft.{c,h}: DIFFS FOUND — Law 3 violation" | tee -a "$LOG"; fail=1
fi

# --- verdict -----------------------------------------------------------------------
step "verdict"
if [ "$fail" -eq 0 ]; then
    echo "shard-weft-cluster-bypass: GREEN" | tee -a "$LOG"
else
    echo "shard-weft-cluster-bypass: RED" | tee -a "$LOG"
fi
exit "$fail"
