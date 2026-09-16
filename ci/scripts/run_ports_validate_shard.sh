#!/usr/bin/env bash
# run_ports_validate_shard.sh — Phase 4 port structural validator + proofs.
# Targets: kotlin + swift + dart + ts port rule packs, plus the Series-5
# fan-out rule pack (fanout-kotlin/swift/dart — RFC 0004 VM-port rings) and
# the Series-7 API<33 compat pack. Each emits one JSON line; all must pass.
#
# Series-7 upgrade ("validator -> proof"): the shard runs the validator
# with --prove (compile-and-load the C kernel into a real .so and drive a
# publish/claim roundtrip through it; Kotlin/Dart/Swift compile-or-delegate
# proofs) and --torture (the C F10 100k 3-reader torture). Delegations are
# recorded in the artifact, never silent.
set -euo pipefail

mkdir -p ci/run-artifacts

# Run the validator (structure + ordering-site proofs + compile/load + torture)
python3 tools/port_validator.py --prove --torture 2>&1 | tee ci/run-artifacts/shard-ports-validate.log | \
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
proofs = [r for r in results if str(r.get('target', '')).startswith('proof-')]
sites = sum(1 for r in results for c in r.get('checks', []) if str(c.get('check', '')).startswith('site_'))
out = {
    'shard': 'ports-validate',
    'status': 'PASSED' if n_pass == n_total else 'FAILED',
    'targets_pass': n_pass,
    'targets_total': n_total,
    'ordering_site_proofs': sites,
    'proofs': [
        {'proof': r.get('target'), 'pass': r.get('pass'), 'detail': r.get('detail')}
        for r in proofs
    ],
    'targets': [
        {'target': r.get('target'), 'pass': r.get('pass'),
         'checks_total': len(r.get('checks', [])),
         'checks_pass': sum(1 for c in r.get('checks', []) if c.get('pass'))}
        for r in results if not str(r.get('target', '')).startswith('proof-')
    ],
}
import json
json.dump(out, open('ci/run-artifacts/shard-ports-validate-results.json', 'w'), indent=2)
print(json.dumps(out, indent=2))
"

# Re-run validator; capture exit code (single source of truth for the gate)
set +e
python3 tools/port_validator.py --prove --torture > /dev/null 2>&1
EXIT=$?
set -e

if [ $EXIT -eq 0 ]; then
  echo "✅ ports-validate: all targets + proofs + torture PASS"
else
  echo "❌ ports-validate: at least one target/proof FAILED"
  exit 1
fi
