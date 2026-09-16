#!/usr/bin/env bash
# run_ports_validate_shard.sh — Phase 4 port structural validator.
# Targets: kotlin + swift + dart + ts port rule packs, plus the Series-5
# fan-out rule pack (fanout-kotlin/swift/dart — RFC 0004 VM-port rings).
# Each emits one JSON line; all must pass.
set -euo pipefail

mkdir -p ci/run-artifacts

# Run the validator
python3 tools/port_validator.py 2>&1 | tee ci/run-artifacts/shard-ports-validate.log | \
  python3 -c "
import json, sys
results = []
for line in sys.stdin:
    line = line.strip()
    if not line.startswith('{'):
        continue
    try:
        d = json.loads(line)
        results.append(d)
    except json.JSONDecodeError:
        continue

n_pass = sum(1 for r in results if r.get('pass'))
n_total = len(results)
out = {
    'shard': 'ports-validate',
    'status': 'PASSED' if n_pass == n_total else 'FAILED',
    'targets_pass': n_pass,
    'targets_total': n_total,
    'targets': [
        {'target': r.get('target'), 'pass': r.get('pass'),
         'checks_total': len(r.get('checks', [])),
         'checks_pass': sum(1 for c in r.get('checks', []) if c.get('pass'))}
        for r in results
    ],
}
import json
json.dump(out, open('ci/run-artifacts/shard-ports-validate-results.json', 'w'), indent=2)
print(json.dumps(out, indent=2))
"

# Re-run validator; capture exit code
set +e
python3 tools/port_validator.py > /dev/null 2>&1
EXIT=$?
set -e

if [ $EXIT -eq 0 ]; then
  echo "✅ ports-validate: all targets PASS (ports + fan-out rule packs)"
else
  echo "❌ ports-validate: at least one target FAILED"
  exit 1
fi
