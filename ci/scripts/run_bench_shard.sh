#!/usr/bin/env bash
# run_bench_shard.sh — run the B-suite for a single language.
# Args: $1 = lang (c | rust | ts)
# Outputs: ci/run-artifacts/shard-bench-b-<lang>.log + bench/results.json (scratch)
set -euo pipefail
LANG_ARG="${1:?usage: run_bench_shard.sh <c|rust|ts>}"

mkdir -p ci/run-artifacts

# Build the needed kernel + bench runner
case "$LANG_ARG" in
  c)    make build-c-bench ;;
  rust) make build-rust ;;
  ts)   make build-ts ;;
  *)    echo "unknown lang: $LANG_ARG" >&2; exit 2 ;;
esac

# Canonical bundle isolation per WO-P5-RELEASE decision 1:
# Back up the canonical results.json, run the harness, restore it.
CANON_SHA="unknown"
if [ -f bench/results.json ]; then
  cp bench/results.json /tmp/weft-canonical-results.json.bak
  CANON_SHA=$(sha256sum bench/results.json | cut -d' ' -f1)
  echo "Canonical bundle pre-run sha256: $CANON_SHA"
fi

# Run B-suite for the requested language only — writes to bench/results.json
python3 tools/bench_driver.py --langs "$LANG_ARG" --timeout 120 2>&1 | tee ci/run-artifacts/shard-bench-b-${LANG_ARG}.log

# Restore canonical bundle
if [ -f /tmp/weft-canonical-results.json.bak ]; then
  cp /tmp/weft-canonical-results.json.bak bench/results.json
  rm /tmp/weft-canonical-results.json.bak
  RESTORED_SHA=$(sha256sum bench/results.json | cut -d' ' -f1)
  echo "Canonical bundle restored: $RESTORED_SHA"
fi

# Write structured results JSON
python3 -c "
import json, sys
r = json.load(open('bench/results.json'))
matrix = r.get('matrix', {})
cells = {k: v.get('pass') for k, v in matrix.items() if k.startswith('${LANG_ARG}/')}
n_pass = sum(1 for v in cells.values() if v)
n_total = len(cells)
out = {
    'shard': 'bench-b-${LANG_ARG}',
    'status': 'PASSED' if n_pass == n_total else 'FAILED',
    'cells_pass': n_pass,
    'cells_total': n_total,
    'cells': cells,
    'canonical_sha256_prefix': '${CANON_SHA:0:8}',
}
json.dump(out, open('ci/run-artifacts/shard-bench-b-${LANG_ARG}-results.json', 'w'), indent=2)
print(json.dumps(out, indent=2))
"

# Exit non-zero if any cell is RED
python3 -c "
import json, sys
r = json.load(open('ci/run-artifacts/shard-bench-b-${LANG_ARG}-results.json'))
sys.exit(0 if r['status'] == 'PASSED' else 1)
" || exit 1
