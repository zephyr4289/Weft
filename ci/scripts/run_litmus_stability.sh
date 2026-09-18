#!/usr/bin/env bash
# run_litmus_stability.sh — run litmus for a single language × N iterations.
# Args: $1 = lang (c|rust|ts), $2 = iterations (default 5)
# Catches flaky EXPOSURE-SHORTFALLs by counting pass/fail distribution.
#
# NOTE (2026-09-16): the original version of this script was syntactically
# invalid bash — a stray quote on the RESULTS_JSON close line caused
# "unexpected EOF while looking for matching `''", so the nightly
# litmus-stability job could never execute. The JSON assembly also appended
# fields AFTER closing the root object (a second, latent invalidity). Both
# are fixed by delegating JSON construction to Python; the threshold check
# additionally no longer depends on `bc`.
set -euo pipefail
LANG_ARG="${1:?usage: run_litmus_stability.sh <c|rust|ts> [iterations]}"
ITERATIONS="${2:-5}"

mkdir -p ci/run-artifacts

case "$LANG_ARG" in
  c)    make build-c ;;
  rust) make build-rust ;;
  ts)   make build-ts ;;
esac

RUNS_FILE="ci/run-artifacts/shard-litmus-stability-${LANG_ARG}-runs.txt"
: > "$RUNS_FILE"

PASS_COUNT=0
FAIL_COUNT=0

for i in $(seq 1 "$ITERATIONS"); do
  echo ""
  echo "=== Iteration $i/$ITERATIONS ($LANG_ARG) ==="
  if python3 tools/litmus_driver.py --langs "$LANG_ARG" --timeout 180 2>&1 | \
     tee -a "ci/run-artifacts/shard-litmus-stability-${LANG_ARG}.log"; then
    PASS_COUNT=$((PASS_COUNT + 1))
    printf '{"iteration":%s,"status":"PASSED"}\n' "$i" >> "$RUNS_FILE"
  else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    # Capture the failing cell(s) from the driver's results snapshot
    FAILING=$(python3 -c "
import json
r = json.load(open('litmus/results.json'))
matrix = r.get('matrix', {})
fails = [k for k, v in matrix.items() if not v.get('pass')]
print(','.join(fails) if fails else 'unknown')
" 2>/dev/null || echo "unknown")
    printf '{"iteration":%s,"status":"FAILED","failing":"%s"}\n' "$i" "$FAILING" >> "$RUNS_FILE"
  fi
done

STATUS=$( [ "$FAIL_COUNT" -le 1 ] && echo PASSED || echo FAILED )

python3 - "$LANG_ARG" "$ITERATIONS" "$PASS_COUNT" "$FAIL_COUNT" <<'PYEOF'
import json, sys
lang, iterations, npass, nfail = (
    sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]))
runs = []
with open(f'ci/run-artifacts/shard-litmus-stability-{lang}-runs.txt') as f:
    for line in f:
        if line.strip():
            runs.append(json.loads(line))
pass_rate = round(npass / iterations * 100, 1)
status = 'PASSED' if pass_rate >= 80.0 else 'FAILED'
data = {
    'shard': f'litmus-stability-{lang}',
    'iterations': iterations,
    'runs': runs,
    'pass_count': npass,
    'fail_count': nfail,
    'status': status,
    'pass_rate': pass_rate,
}
out = f'ci/run-artifacts/shard-litmus-stability-{lang}-results.json'
json.dump(data, open(out, 'w'), indent=2)
print(json.dumps(data, indent=2))
PYEOF

echo ""
echo "=== Stability summary ($LANG_ARG): $PASS_COUNT/$ITERATIONS PASS, $FAIL_COUNT/$ITERATIONS FAIL ==="

# Stability shard: PASS iff pass_rate >= 80% (allow 1 failure in 5 runs for
# flakiness), but always report the actual rate.
PASS_RATE=$(python3 -c "import json; print(json.load(open('ci/run-artifacts/shard-litmus-stability-${LANG_ARG}-results.json'))['pass_rate'])")
if python3 -c "import sys; sys.exit(0 if float(sys.argv[1]) >= 80.0 else 1)" "$PASS_RATE"; then
  echo "✅ Stability threshold met (>=80%)"
  sed -i '1s/^/STATUS: PASSED\n/' "ci/run-artifacts/shard-litmus-stability-${LANG_ARG}.log" 2>/dev/null || true
  exit 0
else
  echo "❌ Stability threshold NOT met (<80% — investigate flakiness)"
  sed -i '1s/^/STATUS: FAILED\n/' "ci/run-artifacts/shard-litmus-stability-${LANG_ARG}.log" 2>/dev/null || true
  exit 1
fi
