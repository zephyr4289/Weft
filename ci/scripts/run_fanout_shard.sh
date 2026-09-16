#!/usr/bin/env bash
# run_fanout_shard.sh — RFC 0004 concurrent fan-out shard.
#
# Runs the true-parallelism torture test (1 writer + 4 reader workers,
# integrity-gated) and the concurrent bench. The torture test IS the gate:
# any integrity violation, non-convergence, inexact drop accounting, or
# timeout exits non-zero.
#
# Output: ci/run-artifacts/shard-fanout-concurrent.log
#         ci/run-artifacts/shard-fanout-concurrent-results.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts

# Node 18+ has worker_threads + SharedArrayBuffer; Node 24 (CI env) strips TS
# types natively for the daisy-chain phase's kernel import.
node spikes/fanout-heddles/fanout_concurrent_test.cjs 2>&1 | tee ci/run-artifacts/shard-fanout-concurrent.log

TEST_RC=${PIPESTATUS[0]}
if [ "$TEST_RC" -ne 0 ]; then
  echo '{"shard":"fanout-concurrent","status":"FAILED","stage":"torture-test"}' > ci/run-artifacts/shard-fanout-concurrent-results.json
  exit "$TEST_RC"
fi

# Bench runs for evidence; a bench crash fails the shard, rate numbers do not
# (they are environment-relative — the invariants live in the torture gate).
node spikes/fanout-heddles/fanout_bench.cjs 2>&1 | tee -a ci/run-artifacts/shard-fanout-concurrent.log

echo '{"shard":"fanout-concurrent","status":"PASSED","gates":"G1-G6 + daisy integrity"}' > ci/run-artifacts/shard-fanout-concurrent-results.json
echo "✅ fanout-concurrent shard PASSED"
