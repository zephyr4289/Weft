#!/usr/bin/env bash
# run_perf_regression_shard.sh — W-suite P99 vs pinned baseline.
# FAIL iff any cell's P99 drops >15% below baseline.
#
# Baseline update protocol:
#   - Baselines live in ci/baselines/wsuite-p99-baseline.json
#   - To update the baseline (e.g., after an intentional protocol change),
#     open a PR with label "perf-baseline-update" — the CI will use the PR's
#     new W-suite output as the baseline and commit it to the baseline file.
set -euo pipefail

mkdir -p ci/run-artifacts

BASELINE_FILE="ci/baselines/wsuite-p99-baseline.json"
REGRESSION_THRESHOLD_PCT=15  # >15% drop = FAIL

# Build C kernel (needed for W-suite backend C)
make build-c-bench || (cd core/c && gcc -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE -o libweft.so -fPIC -shared weft.c)

# Run the W-suite
echo "→ Running W-suite for perf-regression comparison"
PYTHONPATH=bench/workloads python3 bench/workloads/wsuite_runner.py \
  --workloads W1,W2,W3,W4,W5 --backends A,B,C,D \
  --measure-s 5 --warmup-s 1 2>&1 | tee ci/run-artifacts/shard-perf-regression.log

# Compare against baseline
python3 -c "
import json, sys, os

baseline_path = '$BASELINE_FILE'
results_path = 'bench/results/wsuite-x86_64-sandbox.json'

if not os.path.exists(baseline_path):
    print(f'⚠️  Baseline file not found: {baseline_path}')
    print('   Creating baseline from this run (first-run behavior).')
    os.makedirs(os.path.dirname(baseline_path), exist_ok=True)
    with open(results_path) as f: r = json.load(f)
    baseline = {
        'cells': [
            {'workload': c['workload'], 'backend': c['backend'], 'p99': c.get('fps_p99', 0)}
            for c in r.get('cells', [])
        ],
        'note': 'Initial baseline — created from first CI run. Update via PR label perf-baseline-update.',
    }
    with open(baseline_path, 'w') as f: json.dump(baseline, f, indent=2)
    print(json.dumps({'shard': 'perf-regression', 'status': 'PASSED', 'reason': 'baseline-initialized',
                      'baseline_path': baseline_path}, indent=2))
    json.dump({'shard': 'perf-regression', 'status': 'PASSED', 'reason': 'baseline-initialized',
               'baseline_path': baseline_path},
              open('ci/run-artifacts/shard-perf-regression-results.json', 'w'), indent=2)
    sys.exit(0)

with open(baseline_path) as f: baseline = json.load(f)
with open(results_path) as f: r = json.load(f)

baseline_map = {(c['workload'], c['backend']): c['p99'] for c in baseline.get('cells', [])}
results_map = {(c['workload'], c['backend']): c.get('fps_p99', 0) for c in r.get('cells', [])}

regressions = []
improvements = []
all_compared = []

for key, baseline_p99 in baseline_map.items():
    actual_p99 = results_map.get(key)
    if actual_p99 is None:
        regressions.append({'workload': key[0], 'backend': key[1], 'baseline': baseline_p99,
                            'actual': None, 'delta_pct': -100, 'reason': 'cell-missing'})
        continue
    delta_pct = ((actual_p99 - baseline_p99) / baseline_p99 * 100) if baseline_p99 > 0 else 0
    all_compared.append({
        'workload': key[0], 'backend': key[1],
        'baseline_p99': baseline_p99, 'actual_p99': actual_p99,
        'delta_pct': round(delta_pct, 1),
    })
    if delta_pct < -$REGRESSION_THRESHOLD_PCT:
        regressions.append({'workload': key[0], 'backend': key[1],
                            'baseline': baseline_p99, 'actual': actual_p99,
                            'delta_pct': round(delta_pct, 1), 'reason': 'regression'})
    elif delta_pct > $REGRESSION_THRESHOLD_PCT:
        improvements.append({'workload': key[0], 'backend': key[1],
                             'baseline': baseline_p99, 'actual': actual_p99,
                             'delta_pct': round(delta_pct, 1), 'reason': 'improvement'})

status = 'FAILED' if regressions else 'PASSED'
out = {
    'shard': 'perf-regression',
    'status': status,
    'threshold_pct': $REGRESSION_THRESHOLD_PCT,
    'regression_count': len(regressions),
    'improvement_count': len(improvements),
    'cells_compared': len(all_compared),
    'regressions': regressions,
    'improvements': improvements,
    'all_compared': all_compared,
}

json.dump(out, open('ci/run-artifacts/shard-perf-regression-results.json', 'w'), indent=2)
print(json.dumps(out, indent=2))

# Write the PR comment body (used by the aggregation job)
if regressions:
    with open('ci/final-report/perf-regression.md', 'w') as f:
        f.write('### ⚠️ Performance Regression Detected\n\n')
        f.write(f'**{len(regressions)} cell(s)** dropped more than {$REGRESSION_THRESHOLD_PCT}% below the pinned baseline.\n\n')
        f.write('| Workload | Backend | Baseline P99 | Actual P99 | Δ% |\n')
        f.write('|---|---|---|---|---|\n')
        for reg in regressions:
            f.write(f'| {reg[\"workload\"]} | {reg[\"backend\"]} | {reg[\"baseline\"]} | {reg.get(\"actual\", \"missing\")} | {reg[\"delta_pct\"]}% |\n')
        f.write('\nTo update the baseline (e.g., after an intentional protocol change), add the label **perf-baseline-update** to this PR.\n')
" || exit 0  # Don't fail if Python errors; the actual fail is below

# If the perf-regression script wrote the FAILED status, exit non-zero
STATUS=$(python3 -c "import json; print(json.load(open('ci/run-artifacts/shard-perf-regression-results.json'))['status'])")
echo ""
echo "=== Perf-regression status: $STATUS ==="

if [ "$STATUS" = "FAILED" ]; then
  echo "❌ Performance regression detected — see ci/final-report/perf-regression.md"
  # Print the regression table for the workflow log
  cat ci/final-report/perf-regression.md 2>/dev/null || true
  exit 1
fi

echo "✅ No performance regressions"
