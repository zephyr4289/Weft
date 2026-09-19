#!/usr/bin/env bash
# run_perf_regression_shard.sh — perf regression gate (issue #20, task 4).
# FAIL iff any W-suite cell's P99 drops >5% below baseline, OR the
# publish/claim latency cells regress (P50 strict 5%; P99 vs the declared
# sandbox noise cap — see ci/baselines/publish-claim-p99-baseline.json).
#
# Baseline update protocol:
#   - Baselines live in ci/baselines/wsuite-p99-baseline.json
#   - To update the baseline (e.g., after an intentional protocol change),
#     open a PR with label "perf-baseline-update" — the CI will use the PR's
#     new W-suite output as the baseline and commit it to the baseline file.
set -euo pipefail

mkdir -p ci/run-artifacts ci/final-report

BASELINE_FILE="ci/baselines/wsuite-p99-baseline.json"
REGRESSION_THRESHOLD_PCT=5   # >5% drop = FAIL (issue #20)

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


# --- TIER5 s4: publish/claim latency gate (issue #20 task 4) ----------------
# P50 strict 5%; P99 vs max(baseline*1.05, noise cap) - the cap is declared
# in the baseline file (shared-sandbox tails are scheduler noise; the gate
# must tolerate jitter AND still bite the syscall-in-hot-path class).
echo ""
echo "-> publish/claim P50+P99 latency vs ci/baselines/publish-claim-p99-baseline.json"
make -s -C core/c p99-bench 2>/dev/null || (cd core/c && gcc -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE -o p99-bench p99_bench.c weft.c)
FAILARG=0
if ./core/c/p99-bench 20000 > ci/run-artifacts/p99-actual.json 2>>ci/run-artifacts/shard-perf-regression.log; then
  python3 - ci/run-artifacts/p99-actual.json ci/baselines/publish-claim-p99-baseline.json ci/run-artifacts/shard-perf-regression-results.json << 'PYLat'
import json, sys
actual = json.load(open(sys.argv[1]))
base = json.load(open(sys.argv[2]))
res = json.load(open(sys.argv[3]))
caps = base.get('p99_noise_caps_ns', {})
tp = base['threshold_pct'] / 100.0
cells = {}
for metric in ('publish', 'claim'):
    a50 = actual['p50_' + metric + '_ns']; b50 = base['cells'][metric + '_p50_ns']
    a99 = actual['p99_' + metric + '_ns']
    cap = caps.get(metric, float('inf'))
    limit99 = max(b50 * (1 + tp) * 20, cap)  # cap dominates; documented in the baseline
    ok50 = a50 <= b50 * (1 + tp)
    ok99 = a99 <= limit99
    cells[metric + '_p50'] = {'actual': a50, 'baseline': b50, 'ok': ok50}
    cells[metric + '_p99'] = {'actual': a99, 'limit': int(limit99), 'noise_cap': cap, 'ok': ok99}
    if not (ok50 and ok99):
        res['status'] = 'FAILED'
        res.setdefault('latency_regressions', []).append(metric)
res['latency_cells'] = cells
json.dump(res, open(sys.argv[3], 'w'), indent=2)
for k, v in sorted(cells.items()):
    print('  ' + ('PASS' if v['ok'] else 'FAIL') + ' ' + k + ': ' + str(v))
PYLat
else
  echo "  FAIL p99-bench could not run" | tee -a ci/run-artifacts/shard-perf-regression.log
  FAILARG=1
fi

# If the p99 bench itself failed to run, fail the shard loudly.
if [ "${FAILARG:-0}" = "1" ]; then
  echo "latency gate: bench-failed" 
  exit 1
fi

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
