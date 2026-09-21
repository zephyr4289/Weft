#!/usr/bin/env bash
# run_spectrum_managed_shard.sh — Pillar 5 (weft-spectrum) CI shard wrapper.
#
# Delegates to tools/spectrum/tests/run_spectrum_managed_suite.sh (11
# fail-closed stages) and persists the full log + a compact results JSON
# under ci/run-artifacts/ for the ci-report branch.
#
# Output:
#   ci/run-artifacts/shard-spectrum-managed.log
#   ci/run-artifacts/shard-spectrum-managed-results.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG="$ROOT/ci/run-artifacts/shard-spectrum-managed.log"
RESULTS="$ROOT/ci/run-artifacts/shard-spectrum-managed-results.json"
: > "$LOG"

STARTED="$(date +%s)"
EXIT_CODE=0
if bash tools/spectrum/tests/run_spectrum_managed_suite.sh 2>&1 | tee -a "$LOG"; then
  EXIT_CODE=0
else
  EXIT_CODE=$?
fi
ENDED="$(date +%s)"

# stage scoreboard from the log (FAIL <n> / ALL <n> STAGES GREEN)
FAILED_LINE="$(grep -E 'SPECTRUM MANAGED SUITE: (ALL|.*FAILED)' "$LOG" | tail -1 || true)"
python3 - "$RESULTS" "$STARTED" "$ENDED" "$EXIT_CODE" "$FAILED_LINE" <<'PYEOF'
import json, sys
out, started, ended, code, line = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
json.dump({
    "shard": "spectrum-managed",
    "pillar": 5,
    "wallSeconds": ended - started,
    "exitCode": code,
    "scoreboard": line,
    "green": code == 0,
}, open(out, "w"), indent=1)
PYEOF

exit "$EXIT_CODE"
