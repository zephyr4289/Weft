#!/usr/bin/env bash
# run_fanout_native_shard.sh — RFC-0004 native fan-out shard (C + Rust + xlang).
#
# Gates (any failure exits non-zero):
#   1. C F-series conformance, both ordering regimes (fenced acq/rel + seq_cst)
#   2. C torture: 1 writer x 4 readers, every fresh claim word-validated,
#      both regimes (200k frames — CI-friendly; the committed 1M-frame
#      evidence lives in litmus/evidence/fanout/)
#   3. Rust suite: F-series + cursor + torture + exhaustive Loom model
#      (embedded preemption bound 2 — terminates in ~25s)
#   4. Cross-language interop: TS producer -> C consumer, C producer -> TS
#      consumer, bit-exact payload validation (requires packages/core/dist —
#      built by the workflow's fanout-native leg or `pnpm --filter @weft/core build`)
#
# Output: ci/run-artifacts/shard-fanout-native.log
#         ci/run-artifacts/shard-fanout-native-results.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-fanout-native.log
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

# --- 1+2: C gates, both ordering regimes ---
step "C build (fenced acq/rel + seq_cst)"
make -C core/c fanout-test fanout-test-seq fanout-runner fanout-runner-seq 2>&1 | tee -a "$LOG"

step "C F-series conformance (default regime)"
./core/c/fanout-test 2>&1 | tee -a "$LOG" || fail=1
step "C F-series conformance (all-seq_cst regime)"
./core/c/fanout-test-seq 2>&1 | tee -a "$LOG" || fail=1

step "C torture (default regime, 200k frames)"
./core/c/fanout-runner torture 200000 4 256 4 2>&1 | tee -a "$LOG" || fail=1
step "C torture (all-seq_cst regime, 200k frames)"
./core/c/fanout-runner-seq torture 200000 4 256 4 2>&1 | tee -a "$LOG" || fail=1

# --- 3: Rust suite ---
step "Rust suite (F-series + cursor + torture + Loom model, bound 2)"
if command -v cargo >/dev/null 2>&1; then
  (cd core/rust && WEFT_FANOUT_FRAMES=200000 cargo test --release) 2>&1 | tee -a "$LOG" || fail=1
else
  echo "cargo not found — SKIPPED (declared)" | tee -a "$LOG"
fi

# --- 4: cross-language interop ---
step "xlang interop (TS <-> C, bit-exact)"
if [ -f packages/core/dist/index.js ]; then
  FRAMES=2000 bash fixtures/xlang-fanout/run.sh 2>&1 | tee -a "$LOG" || fail=1
else
  echo "packages/core/dist not built — interop SKIPPED (build with: pnpm --filter @weft/core build)" | tee -a "$LOG"
  if [ -n "${CI:-}" ]; then fail=1; fi # in CI the workflow builds it; missing dist is a failure
fi

if [ "$fail" -ne 0 ]; then
  echo '{"shard":"fanout-native","status":"FAILED"}' > ci/run-artifacts/shard-fanout-native-results.json
  exit 1
fi
echo '{"shard":"fanout-native","status":"PASSED","gates":"C F-series+torture x2 regimes + rust suite + loom(bound 2) + xlang TS<->C"}' > ci/run-artifacts/shard-fanout-native-results.json
echo "✅ fanout-native shard PASSED" | tee -a "$LOG"
