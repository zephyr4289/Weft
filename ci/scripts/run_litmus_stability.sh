#!/usr/bin/env bash
# run_litmus_stability.sh — run litmus for a single language × N iterations.
# Args: $1 = lang, $2 = iterations (default 5)
# Catches flaky EXPOSURE-SHORTFALLs by counting pass/fail distribution.
set -euo pipefail
LANG_ARG="${1:?usage: run_litmus_stability.sh <c|rust|ts> [iterations]}"
ITERATIONS="${2:-5}"

mkdir -p ci/run-artifacts

case "$LANG_ARG" in
  c)    make build-c ;;
  rust) make build-rust ;;
  ts)   make build-ts ;;
esac

PASS_COUNT=0
FAIL_COUNT=0
RESULTS_JSON='{"shard":"litmus-stability-'$LANG_ARG'","iterations":'$ITERATIONS',"runs":['

for i in $(seq 1 $ITERATIONS); do
  echo ""
  echo "=== Iteration $i/$ITERATIONS ($LANG_ARG) ==="
  if python3 tools/litmus_driver.py --langs "$LANG_ARG" --timeout 180 2>&1 | \
     tee -a ci/run-artifacts/shard-litmus-stability-${LANG_ARG}.log; then
    PASS_COUNT=$((PASS_COUNT + 1))
    RESULTS_JSON+='{"iteration":'$i',"status":"PASSED"},'
  else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    # Capture the failing cell(s)
    FAILING=$(python3 -c "
import json
r = json.load(open('litmus/results.json'))
matrix = r.get('matrix', {})
fails = [k for k, v in matrix.items() if not v.get('pass')]
print(','.join(fails) if fails else 'unknown')
" 2>/dev/null || echo "unknown")
    RESULTS_JSON+="{\"iteration\":$i,\"status\":\"FAILED\",\"failing\":\"$FAILING\"},"
  fi
done

# Strip trailing comma and close
RESULTS_JSON=${RESULTS_JSON%,}']}
RESULTS_JSON+=',"pass_count":'$PASS_COUNT',"fail_count":'$FAIL_COUNT','
RESULTS_JSON+='"status":"'$( [ $FAIL_COUNT -eq 0 ] && echo PASSED || echo FAILED )'"}'

python3 -c "
import json
data = json.loads('''$RESULTS_JSON''')
data['pass_rate'] = round(data['pass_count'] / data['iterations'] * 100, 1)
json.dump(data, open('ci/run-artifacts/shard-litmus-stability-${LANG_ARG}-results.json', 'w'), indent=2)
print(json.dumps(data, indent=2))
"

echo ""
echo "=== Stability summary ($LANG_ARG): $PASS_COUNT/$ITERATIONS PASS, $FAIL_COUNT/$ITERATIONS FAIL ==="
echo "Pass rate: $(python3 -c "import json; print(json.load(open('ci/run-artifacts/shard-litmus-stability-${LANG_ARG}-results.json'))['pass_rate'])")%"

# Stability shard: PASS iff pass_rate >= 80% (allow 1 failure in 5 runs for flakiness)
# but always report the actual pass rate
PASS_RATE=$(python3 -c "import json; print(json.load(open('ci/run-artifacts/shard-litmus-stability-${LANG_ARG}-results.json'))['pass_rate'])")
if [ "$(echo "$PASS_RATE >= 80.0" | bc)" = "1" ]; then
  echo "✅ Stability threshold met (>=80%)"
  exit 0
else
  echo "❌ Stability threshold NOT met (<80% — investigate flakiness)"
  exit 1
fi
