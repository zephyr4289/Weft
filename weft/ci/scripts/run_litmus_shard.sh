#!/usr/bin/env bash
# run_litmus_shard.sh — run the litmus suite for a single language.
# Args: $1 = lang (c | rust | ts)
# Outputs: ci/run-artifacts/shard-litmus-<lang>.log + litmus/results.json
set -euo pipefail
LANG_ARG="${1:?usage: run_litmus_shard.sh <c|rust|ts>}"

mkdir -p ci/run-artifacts

# Build the needed kernel first
case "$LANG_ARG" in
  c)    make build-c ;;
  rust) make build-rust ;;
  ts)   make build-ts ;;
  *)    echo "unknown lang: $LANG_ARG" >&2; exit 2 ;;
esac

# Run litmus for the requested language only
python3 tools/litmus_driver.py --langs "$LANG_ARG" --timeout 180 2>&1 | tee ci/run-artifacts/shard-litmus-${LANG_ARG}.log

# Write structured results JSON for the aggregator
python3 -c "
import json, sys
results_path = 'litmus/results.json'
try:
    r = json.load(open(results_path))
    matrix = r.get('matrix', {})
    cells = {k: v.get('pass') for k, v in matrix.items()}
    n_pass = sum(1 for v in cells.values() if v)
    n_total = len(cells)
    out = {
        'shard': 'litmus-${LANG_ARG}',
        'status': 'PASSED' if n_pass == n_total else 'FAILED',
        'cells_pass': n_pass,
        'cells_total': n_total,
        'cells': cells,
    }
except Exception as e:
    out = {'shard': 'litmus-${LANG_ARG}', 'status': 'ERROR', 'error': str(e)}
json.dump(out, open('ci/run-artifacts/shard-litmus-${LANG_ARG}-results.json', 'w'), indent=2)
print(json.dumps(out, indent=2))
"

# Exit non-zero if any cell is RED
python3 -c "
import json
r = json.load(open('ci/run-artifacts/shard-litmus-${LANG_ARG}-results.json'))
sys.exit(0 if r['status'] == 'PASSED' else 1)
" || exit 1
