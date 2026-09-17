#!/usr/bin/env bash
# run_guardian_shard.sh — RFC 0011 Telemetry Guardian shard.
#
# The watchdog itself, in CI:
#   1. SELFTEST  — the guardian must BITE on every bad fixture (a 3.41%
#                  throughput drop, a ONE-BYTE wire drift, a failed shard
#                  artifact) and PASS the healthy ones. A guardian that
#                  cannot bite is not a guardian — this leg enforces it.
#   2. WIRE      — the C wire probe observes the REAL kernel's layout at
#                  runtime (raw byte reads at documented offsets) and the
#                  guardian diffs it against the canonical manifest. Any
#                  single-byte drift = RED.
#   3. THROUGHPUT— the bench matrix vs the pinned baseline, >= 3% median
#                  drop = RED (creates the baseline on first run, loudly).
#   4. CRASH     — every shard result artifact + litmus results audited;
#                  any FAILED/pass=false is a finding.
#
# Output: ci/run-artifacts/shard-guardian.log
#         ci/run-artifacts/shard-guardian-results.json
#         tools/guardian/guardian-verdict.json (last watch)
# Exit:   0 only if all legs are green.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts ci/baselines
LOG=ci/run-artifacts/shard-guardian.log
RESULTS=ci/run-artifacts/shard-guardian-results.json
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

GUARDIAN="python3 tools/guardian/guardian.py"

step "Build the C wire probe"
make -C core/c wire-probe 2>&1 | tee -a "$LOG"

step "1. Guardian SELFTEST — the watchdog must bite"
if $GUARDIAN selftest 2>&1 | tee -a "$LOG"; then :; else fail=1; echo "selftest RED" | tee -a "$LOG"; fi

step "2. WIRE watch — live C kernel probe vs canonical manifest"
if ./core/c/wire-probe > ci/run-artifacts/wire-probe-c.json 2>>"$LOG" && \
   $GUARDIAN wire --probe-json ci/run-artifacts/wire-probe-c.json 2>&1 | tee -a "$LOG"; then
  :
else
  fail=1; echo "wire watch RED" | tee -a "$LOG"
fi

step "3. THROUGHPUT watch — bench matrix vs pinned baseline (>=3% drop = RED)"
if [ ! -f ci/baselines/guardian-throughput-baseline.json ]; then
  echo "no guardian throughput baseline yet — seeding from the current bench results (LOUD first-run)" | tee -a "$LOG"
  python3 - <<'PY' 2>&1 | tee -a "$LOG"
import json
from pathlib import Path
results = json.loads(Path("bench/results.json").read_text())
entries = {}
for key, val in results.get("matrix", {}).items():
    metric = "ops_per_s" if "B1" in key else ("publishes_per_s" if "B2" in key else None)
    if metric and metric in val.get("metrics", {}):
        entries[key] = {"metric": metric, "value": val["metrics"][metric]}
Path("ci/baselines/guardian-throughput-baseline.json").write_text(json.dumps({
    "v": 1,
    "note": "Guardian throughput baseline. Update ONLY via a PR that explains the change; the guardian flags >=3% median drops.",
    "entries": entries,
}, indent=2))
print(f"seeded {len(entries)} baseline entries")
PY
fi
if $GUARDIAN throughput 2>&1 | tee -a "$LOG"; then :; else fail=1; echo "throughput watch RED" | tee -a "$LOG"; fi

step "4. CRASH watch — shard artifacts + litmus results audited"
if $GUARDIAN crash 2>&1 | tee -a "$LOG"; then :; else fail=1; echo "crash watch RED" | tee -a "$LOG"; fi

if [ "$fail" -eq 0 ]; then
  echo '{"shard":"guardian","status":"PASSED","gates":"selftest-bites + wire-probe 52 fields + throughput vs baseline + crash audit"}' > "$RESULTS"
  echo "guardian shard: PASS" | tee -a "$LOG"
else
  echo '{"shard":"guardian","status":"FAILED"}' > "$RESULTS"
  echo "guardian shard: RED" | tee -a "$LOG"
  exit 1
fi
