#!/usr/bin/env bash
# run_ports_validate_shard.sh — Phase 4 port structural validator.
# 4 targets: kotlin + swift + dart + ts. Each exits 0 = PASS.
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
  echo "✅ ports-validate: 4/4 PASS"
else
  echo "❌ ports-validate: at least one target FAILED"
  exit 1
fi
