#!/usr/bin/env bash
# run_tsan_deep.sh — TSAN litmus × N iterations on C kernel.
# Args: $1 = iterations (default 5)
# Catches thread-sanity regressions in the kernel under adversarial scheduling.
set -euo pipefail
ITERATIONS="${1:-5}"

mkdir -p ci/run-artifacts

# Build C kernel with TSAN (already built by the workflow job; verify it exists)
if [ ! -x core/c/spike-tsan ]; then
  echo "→ Building C kernel with TSAN"
  (cd core/c && gcc -O1 -g -fsanitize=thread -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
    -o spike-tsan weft.c litmus_runner.c)
fi

PASS_COUNT=0
FAIL_COUNT=0
TSAN_HITS=0

for i in $(seq 1 $ITERATIONS); do
  echo ""
  echo "=== TSAN iteration $i/$ITERATIONS ==="
  # Run all 8 litmus tests with the TSAN runner
  for test in L1-tear L2-writer-steps L3-reader-steps L4-freshness L5-progress L6-ownership L7-revocation L8-envelope; do
    echo "  → $test"
    LOG_FILE="ci/run-artifacts/tsan-iter${i}-${test}.log"
    if ./core/c/spike-tsan "$test" > "$LOG_FILE" 2>&1; then
      echo "    ✅ PASS"
    else
      echo "    ❌ FAIL (exit $?)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
    # Check for TSAN warnings
    if grep -q "WARNING: ThreadSanitizer" "$LOG_FILE"; then
      TSAN_HITS=$((TSAN_HITS + 1))
      echo "    ⚠️  TSAN warning detected!"
      grep -A 5 "WARNING: ThreadSanitizer" "$LOG_FILE" | head -20
    fi
    cat "$LOG_FILE" >> ci/run-artifacts/shard-tsan-deep.log
  done
done

# Build results JSON
TOTAL_RUNS=$((ITERATIONS * 8))
python3 -c "
import json
out = {
    'shard': 'tsan-deep',
    'iterations': $ITERATIONS,
    'total_runs': $TOTAL_RUNS,
    'tsan_warning_hits': $TSAN_HITS,
    'fail_count': $FAIL_COUNT,
    'status': 'PASSED' if $TSAN_HITS == 0 and $FAIL_COUNT == 0 else 'FAILED',
}
json.dump(out, open('ci/run-artifacts/shard-tsan-deep-results.json', 'w'), indent=2)
print(json.dumps(out, indent=2))
"

echo ""
echo "=== TSAN deep summary: $TSAN_HITS TSAN warnings across $TOTAL_RUNS runs ==="
[ $TSAN_HITS -eq 0 ] || exit 1
