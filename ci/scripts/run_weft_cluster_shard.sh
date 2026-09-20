#!/usr/bin/env bash
# run_weft_cluster_shard.sh — RFC 0018 weft-cluster shard (Pillar 3).
#
# Gates (any failure exits non-zero):
#   1. CL-ring conformance: plain / ASAN / TSAN
#      (two-store seqlock, tear detection, backpressure, validation ladder,
#       fork cross-process IPC, 100k zero-alloc steady state)
#   2. CL-consensus conformance: plain / ASAN
#      (ring-embedded election, lease stability, quorum loss, fencing,
#       partition healing, vote uniqueness, clock-skew gate, eviction,
#       consensus seqlock pairing, 100k cluster sync ops zero-alloc)
#   3. Split-brain torture: plain / ASAN
#      (dueling candidates, byzantine block injection, 7-node random
#       partition chaos w/ single-leader invariant + refusal whitelist,
#       leader crash recovery)
#   4. Benchmark scoreboard (informational; committed full evidence lives
#      in litmus/evidence/cluster/)
#
# Output: ci/run-artifacts/shard-weft-cluster.log
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-weft-cluster.log
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

# --- 1: ring engine, all regimes ---
step "build cluster targets"
make -C core/c cluster-ring-test cluster-ring-test-asan cluster-ring-test-tsan \
     cluster-consensus-test cluster-consensus-test-asan \
     split-brain-torture split-brain-torture-asan cluster-bench 2>&1 | tee -a "$LOG"

step "CL-ring conformance (plain)"
./core/c/cluster-ring-test 2>&1 | tee -a "$LOG" || fail=1
step "CL-ring conformance (ASAN)"
./core/c/cluster-ring-test-asan 2>&1 | tee -a "$LOG" || fail=1
step "CL-ring conformance (TSAN)"
./core/c/cluster-ring-test-tsan 2>&1 | tee -a "$LOG" || fail=1

# --- 2: consensus engine ---
step "CL-consensus conformance (plain)"
./core/c/cluster-consensus-test 2>&1 | tee -a "$LOG" || fail=1
step "CL-consensus conformance (ASAN)"
./core/c/cluster-consensus-test-asan 2>&1 | tee -a "$LOG" || fail=1

# --- 3: split-brain torture ---
step "split-brain torture (plain)"
./core/c/split-brain-torture 2>&1 | tee -a "$LOG" || fail=1
step "split-brain torture (ASAN)"
./core/c/split-brain-torture-asan 2>&1 | tee -a "$LOG" || fail=1

# --- 4: benchmark scoreboard (informational) ---
step "cluster-bench scoreboard (protocol-engine overhead, local transport)"
timeout 300 ./core/c/cluster-bench 2>&1 | tee -a "$LOG" || fail=1

echo "" | tee -a "$LOG"
if [ "$fail" -ne 0 ]; then
  echo "WEFT-CLUSTER SHARD: FAIL" | tee -a "$LOG"
  exit 1
fi
echo "WEFT-CLUSTER SHARD: PASS" | tee -a "$LOG"
