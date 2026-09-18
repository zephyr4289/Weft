#!/usr/bin/env bash
# run_clean_tree_deep.sh — full clean-tree validation.
# Args: $1 = path to release tarball
# Unpacks into empty dir, runs all 5 make targets, archives full per-target logs.
set -euo pipefail
TARBALL="${1:?usage: run_clean_tree_deep.sh <tarball>}"

mkdir -p ci/run-artifacts
WORK_DIR="ci/_clean_tree_nightly"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

echo "=== Clean-tree validation (nightly deep) ===" | tee ci/run-artifacts/shard-clean-tree.log
echo "Tarball: $TARBALL" | tee -a ci/run-artifacts/shard-clean-tree.log
echo "Work dir: $WORK_DIR" | tee -a ci/run-artifacts/shard-clean-tree.log
echo ""

# Unpack
tar -xzf "$TARBALL" -C "$WORK_DIR"
if [ -d "$WORK_DIR/weft" ]; then
  WEFT_TREE="$WORK_DIR/weft"
elif [ -d "$WORK_DIR/weft-sandbox-v0.1" ]; then
  WEFT_TREE="$WORK_DIR/weft-sandbox-v0.1"
else
  WEFT_TREE="$WORK_DIR"
fi

# Source cargo env if present
if [ -f "$HOME/.cargo/env" ]; then
  source "$HOME/.cargo/env"
fi

# Run all 5 make targets in sequence, log each
TARGETS=(build litmus bench site validate)
RESULTS_JSON='{"shard":"clean-tree-deep","targets":['
OVERALL_PASS=1

for target in "${TARGETS[@]}"; do
  echo "" | tee -a ci/run-artifacts/shard-clean-tree.log
  echo "--- make $target ---" | tee -a ci/run-artifacts/shard-clean-tree.log
  LOG_FILE="ci/run-artifacts/clean-tree-${target}.log"

  if (cd "$WEFT_TREE" && make "$target") > "$LOG_FILE" 2>&1; then
    STATUS="PASSED"
    echo "  ✅ make $target: PASS" | tee -a ci/run-artifacts/shard-clean-tree.log
  else
    STATUS="FAILED"
    OVERALL_PASS=0
    echo "  ❌ make $target: FAIL" | tee -a ci/run-artifacts/shard-clean-tree.log
    # Show last 10 lines of the failure
    tail -10 "$LOG_FILE" | tee -a ci/run-artifacts/shard-clean-tree.log
  fi
  cat "$LOG_FILE" >> ci/run-artifacts/shard-clean-tree.log
  RESULTS_JSON+="{\"target\":\"$target\",\"status\":\"$STATUS\"},"
done

# Determinism check
echo "" | tee -a ci/run-artifacts/shard-clean-tree.log
echo "--- site determinism re-render ---" | tee -a ci/run-artifacts/shard-clean-tree.log
SITE_INDEX="$WEFT_TREE/bench/site/index.html"
if [ -f "$SITE_INDEX" ]; then
  FIRST=$(sha256sum "$SITE_INDEX" | cut -d' ' -f1)
  (cd "$WEFT_TREE" && make site) > /dev/null 2>&1
  SECOND=$(sha256sum "$SITE_INDEX" | cut -d' ' -f1)
  if [ "$FIRST" = "$SECOND" ]; then
    DETERMINISM="PASSED"
    echo "  ✅ site deterministic ($FIRST)" | tee -a ci/run-artifacts/shard-clean-tree.log
  else
    DETERMINISM="FAILED"
    OVERALL_PASS=0
    echo "  ❌ site NON-deterministic" | tee -a ci/run-artifacts/shard-clean-tree.log
  fi
  RESULTS_JSON+="{\"target\":\"site-determinism\",\"status\":\"$DETERMINISM\"},"
fi

# Canonical hash check
CANON_FILE="$WEFT_TREE/bench/results.json"
if [ -f "$CANON_FILE" ]; then
  CANON_SHA=$(sha256sum "$CANON_FILE" | cut -c1-8)
  if [ "$CANON_SHA" = "16b5c663" ]; then
    HASH_STATUS="PASSED"
    echo "  ✅ canonical hash 16b5c663 verified" | tee -a ci/run-artifacts/shard-clean-tree.log
  else
    HASH_STATUS="FAILED"
    OVERALL_PASS=0
    echo "  ❌ canonical hash mismatch ($CANON_SHA != 16b5c663)" | tee -a ci/run-artifacts/shard-clean-tree.log
  fi
  RESULTS_JSON+="{\"target\":\"canonical-hash\",\"status\":\"$HASH_STATUS\",\"sha_prefix\":\"$CANON_SHA\"},"
fi

# Strip trailing comma, close JSON
RESULTS_JSON=${RESULTS_JSON%,}']}
RESULTS_JSON+=',"status":"'$( [ $OVERALL_PASS -eq 1 ] && echo PASSED || echo FAILED )'"}'

python3 -c "
import json
data = json.loads('''$RESULTS_JSON''')
json.dump(data, open('ci/run-artifacts/shard-clean-tree-results.json', 'w'), indent=2)
print(json.dumps(data, indent=2))
"

echo ""
echo "=== Clean-tree validation: $( [ $OVERALL_PASS -eq 1 ] && echo 'ALL PASS ✅' || echo 'HAS FAILURES ❌' ) ==="

# Clean up the work tree
rm -rf "$WORK_DIR"

if [ $OVERALL_PASS -eq 1 ]; then
  sed -i '1s/^/STATUS: PASSED\n/' ci/run-artifacts/shard-clean-tree.log 2>/dev/null || true
  exit 0
else
  sed -i '1s/^/STATUS: FAILED\n/' ci/run-artifacts/shard-clean-tree.log 2>/dev/null || true
  exit 1
fi
