#!/usr/bin/env bash
# run_wsuite_shard.sh — run the full W-suite (5 workloads × 4 backends = 20 cells).
# Outputs: ci/run-artifacts/shard-wsuite.log + bench/results/wsuite-x86_64-sandbox.json
set -euo pipefail

mkdir -p ci/run-artifacts

# Build C kernel (needed for backend C via ctypes)
make build-c-bench || {
  # Fall back to direct build if Makefile target name differs
  (cd core/c && gcc -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE -o libweft.so -fPIC -shared weft.c)
}

# Run the W-suite
PYTHONPATH=bench/workloads python3 bench/workloads/wsuite_runner.py \
  --workloads W1,W2,W3,W4,W5 --backends A,B,C,D \
  --measure-s 5 --warmup-s 1 2>&1 | tee ci/run-artifacts/shard-wsuite.log

# Write structured results JSON
python3 -c "
import json
r = json.load(open('bench/results/wsuite-x86_64-sandbox.json'))
cells = r.get('cells', [])
n_pass = sum(1 for c in cells if c.get('pass'))
n_total = len(cells)
out = {
    'shard': 'wsuite',
    'status': 'PASSED' if n_pass == n_total else 'FAILED',
    'cells_pass': n_pass,
    'cells_total': n_total,
    'alloc_violations': sum(1 for c in cells if c.get('alloc_violation')),
    'cells': [
        {
            'workload': c.get('workload'),
            'backend': c.get('backend'),
            'pass': c.get('pass'),
            'p99': c.get('fps_p99'),
            'alloc_per_frame': c.get('alloc_bytes_per_frame'),
            'alloc_violation': c.get('alloc_violation'),
        } for c in cells
    ],
}
json.dump(out, open('ci/run-artifacts/shard-wsuite-results.json', 'w'), indent=2)
print(json.dumps(out, indent=2))
"

# Exit non-zero if any cell is RED
python3 -c "
import json, sys
r = json.load(open('ci/run-artifacts/shard-wsuite-results.json'))
sys.exit(0 if r['status'] == 'PASSED' else 1)
" || exit 1
