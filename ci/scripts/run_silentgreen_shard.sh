#!/usr/bin/env bash
# run_silentgreen_shard.sh — the no-silent-green gate (Series 7).
#
# Enforces the lead's "no silent-green, set -o pipefail everywhere" contract
# mechanically: every ci/scripts/*.sh carries pipefail, and every pipeline
# inside every workflow run block is covered. The audit found and fixed SIX
# live silent-green holes when it landed (2x curl|sh, grep|tail diagnostic,
# 2x ls-remote|grep bookkeeping, plus the same curl in nightly) — this shard
# keeps the class dead permanently.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts

python3 ci/scripts/run_pipefail_audit.py 2>&1 | tee ci/run-artifacts/shard-silent-green-audit.log
echo '{"shard":"silent-green-audit","status":"PASSED"}' > ci/run-artifacts/shard-silent-green-audit-results.json
echo "✅ silent-green-audit shard PASSED"
