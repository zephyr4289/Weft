#!/usr/bin/env bash
# run_turbo_native_shard.sh — RFC-0012 TLEL shard (C only, no extra deps).
#
# Gates (any failure exits non-zero):
#   1. T-series conformance: plain / ASAN / TSAN / all-seq_cst regimes
#      (byte-equivalence, prefault gate, pinned torture, honesty ladder)
#   2. U-series conformance: plain / ASAN / TSAN
#      (kernel-as-filler bracket, capability ladder, size accounting)
#   3. Capability reports captured (turbo + uring ladders — informational;
#      refusals are facts about the host, not failures)
#   4. T-bench smoke: one small rep per TL variant + ING A/B (the committed
#      full-rep evidence lives in litmus/evidence/turbo/)
#   5. Zero-regression guardrail: B-suite C leg (B1 + B3) + F-series
#
# Output: ci/run-artifacts/shard-turbo-native.log
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-turbo-native.log
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

# --- 1: T-series, all regimes ---
step "build turbo + uring targets"
make -C core/c turbo-test turbo-test-asan turbo-test-tsan turbo-test-seq \
     uring-test uring-test-asan uring-test-tsan \
     turbo-runner turbo-runner-sys bench \
     fanout-test fanout-test-seq 2>&1 | tee -a "$LOG"


step "T-series conformance (plain)"
./core/c/turbo-test 2>&1 | tee -a "$LOG" || fail=1
step "T-series conformance (ASAN)"
./core/c/turbo-test-asan 2>&1 | tee -a "$LOG" || fail=1
step "T-series conformance (TSAN)"
./core/c/turbo-test-tsan 2>&1 | tee -a "$LOG" || fail=1
step "T-series conformance (all-seq_cst regime)"
./core/c/turbo-test-seq 2>&1 | tee -a "$LOG" || fail=1

# --- 2: U-series ---
step "U-series conformance (plain)"
./core/c/uring-test 2>&1 | tee -a "$LOG" || fail=1
step "U-series conformance (ASAN)"
./core/c/uring-test-asan 2>&1 | tee -a "$LOG" || fail=1
step "U-series conformance (TSAN)"
./core/c/uring-test-tsan 2>&1 | tee -a "$LOG" || fail=1

# --- 3: capability ladders (informational; refusals are host facts) ---
step "capability reports"
./core/c/turbo-runner caps 2>&1 | tee -a "$LOG"

# --- 4: T-bench smoke (one small rep per cell; full evidence is committed) ---
step "T-bench smoke: TL-writer (256B, one rep per variant)"
for v in plain turbo-prefetch turbo-ring turbo-nt-fill turbo-full; do
  ./core/c/turbo-runner TL-writer variant=$v payload=256 frames=50000 2>/dev/null | tee -a "$LOG" || fail=1
done
step "T-bench smoke: TL-reader (one rep: plain vs early+pin)"
for v in plain turbo-early-hint turbo-early-hint+pin; do
  ./core/c/turbo-runner TL-reader variant=$v payload=256 measure_s=1.0 \
    cadence_us=500 thrash_kib=1024 2>/dev/null | tee -a "$LOG" || fail=1
done
step "T-bench smoke: ING-uring A/B"
./core/c/turbo-runner ING-uring frames=20000 2>/dev/null | tee -a "$LOG" || fail=1
./core/c/turbo-runner-sys ING-uring frames=20000 2>/dev/null | tee -a "$LOG" || fail=1

# --- 5: zero-regression guardrail ---
step "zero-regression: B1 + B3 (C leg, gates must PASS)"
./core/c/bench B1-pub-throughput payload_max=256 measure_s=1.0 warmup_s=0.5 \
  warmup_ops=50000 sample_stride=256 2>/dev/null | tee -a "$LOG" || fail=1
./core/c/bench B3-scaling-fingerprint samples_per_size=2000 writer_hz=1000 \
  sample_stride=256 2>/dev/null | tee -a "$LOG" || fail=1
step "zero-regression: F-series (both regimes)"
./core/c/fanout-test 2>&1 | tail -1 | tee -a "$LOG" || fail=1
./core/c/fanout-test-seq 2>&1 | tail -1 | tee -a "$LOG" || fail=1

echo "" | tee -a "$LOG"
if [ "$fail" -ne 0 ]; then
  echo "TURBO-NATIVE SHARD: FAIL" | tee -a "$LOG"
  exit 1
fi
echo "TURBO-NATIVE SHARD: PASS" | tee -a "$LOG"
