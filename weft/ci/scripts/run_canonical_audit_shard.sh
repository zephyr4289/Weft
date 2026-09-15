#!/usr/bin/env bash
# run_canonical_audit_shard.sh — verify bench/results.json sha256 = 16b5c663...
# This is the R1 Path C invariant: the canonical bundle's hash must never drift.
set -euo pipefail

mkdir -p ci/run-artifacts

EXPECTED_PREFIX="16b5c663"
ACTUAL_SHA=$(sha256sum bench/results.json | cut -d' ' -f1)
ACTUAL_PREFIX=${ACTUAL_SHA:0:8}

echo "=== Canonical bundle audit ===" | tee ci/run-artifacts/shard-canonical-audit.log
echo "Expected prefix: $EXPECTED_PREFIX" | tee -a ci/run-artifacts/shard-canonical-audit.log
echo "Actual sha256:   $ACTUAL_SHA" | tee -a ci/run-artifacts/shard-canonical-audit.log
echo "Actual prefix:   $ACTUAL_PREFIX" | tee -a ci/run-artifacts/shard-canonical-audit.log

# Also verify the embedded stamp line is NOT present (R1 Path C removed it)
if grep -q '"sha256"' bench/results.json; then
  echo "❌ FAIL: results.json still contains the embedded 'sha256' stamp line (R1 Path C regression)" | \
    tee -a ci/run-artifacts/shard-canonical-audit.log
  STATUS="FAILED"
  REASON="stamp-line-present"
else
  echo "✅ No embedded stamp line (R1 Path C intact)" | tee -a ci/run-artifacts/shard-canonical-audit.log
  if [ "$ACTUAL_PREFIX" = "$EXPECTED_PREFIX" ]; then
    echo "✅ Canonical hash matches $EXPECTED_PREFIX" | tee -a ci/run-artifacts/shard-canonical-audit.log
    STATUS="PASSED"
    REASON="ok"
  else
    echo "❌ Canonical hash mismatch" | tee -a ci/run-artifacts/shard-canonical-audit.log
    STATUS="FAILED"
    REASON="hash-mismatch"
  fi
fi

python3 -c "
import json
out = {
    'shard': 'canonical-audit',
    'status': '$STATUS',
    'expected_prefix': '$EXPECTED_PREFIX',
    'actual_sha256': '$ACTUAL_SHA',
    'actual_prefix': '$ACTUAL_PREFIX',
    'reason': '$REASON',
}
json.dump(out, open('ci/run-artifacts/shard-canonical-audit-results.json', 'w'), indent=2)
print(json.dumps(out, indent=2))
"

[ "$STATUS" = "PASSED" ] || exit 1
