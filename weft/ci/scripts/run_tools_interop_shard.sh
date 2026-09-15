#!/usr/bin/env bash
# run_tools_interop_shard.sh — record/replay interop matrix.
# 4 combos: C→C, C→Rust, Rust→Rust, Rust→C. All replay byte-identical (exit 0).
set -euo pipefail

mkdir -p ci/run-artifacts

# Build both record tools
echo "→ building C record tool"
(cd tools/weft-record && \
  gcc -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
    -I ../../core/c weft_record.c ../../core/c/weft.c -o weft_record)

echo "→ building Rust record tool"
(cd core/rust && cargo build --release --bin record --bin probe)

C_RUNNER="tools/weft-record/weft_record"
RUST_RUNNER="core/rust/target/release/record"
PAYLOAD=64
SECS=5
PASS_COUNT=0
FAIL_COUNT=0
RESULTS_JSON='{"shard":"tools-interop","combos":['

run_combo() {
  local name="$1"
  local capture_runner="$2"
  local replay_runner="$3"
  local capture_file="ci/run-artifacts/interop-${name}.weftrec"

  echo ""
  echo "=== $name: capture via $capture_runner, replay via $replay_runner ==="

  # Capture
  if ! "$capture_runner" capture "$capture_file" --hz 120 --payload $PAYLOAD --secs $SECS 2>&1 | \
       tee -a ci/run-artifacts/shard-tools-interop.log; then
    echo "❌ $name: capture FAILED"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    RESULTS_JSON+="{\"combo\":\"$name\",\"status\":\"CAPTURE_FAILED\"},"
    return
  fi

  # Replay
  if "$replay_runner" replay "$capture_file" 2>&1 | tee -a ci/run-artifacts/shard-tools-interop.log; then
    echo "✅ $name: PASS"
    PASS_COUNT=$((PASS_COUNT + 1))
    RESULTS_JSON+="{\"combo\":\"$name\",\"status\":\"PASSED\"},"
  else
    echo "❌ $name: replay FAILED"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    RESULTS_JSON+="{\"combo\":\"$name\",\"status\":\"REPLAY_FAILED\"},"
  fi
}

# Strip trailing comma and close JSON
RESULTS_JSON=${RESULTS_JSON%,}]}

run_combo "c-to-c"     "$C_RUNNER"   "$C_RUNNER"
run_combo "c-to-rust"  "$C_RUNNER"   "$RUST_RUNNER"
run_combo "rust-to-rust" "$RUST_RUNNER" "$RUST_RUNNER"
run_combo "rust-to-c"  "$RUST_RUNNER" "$C_RUNNER"

# Finalize results JSON
python3 -c "
import json
data = json.loads('''$RESULTS_JSON''')
combos = data['combos']
n_pass = sum(1 for c in combos if c['status'] == 'PASSED')
data['status'] = 'PASSED' if n_pass == 4 else 'FAILED'
data['combos_pass'] = n_pass
data['combos_total'] = 4
json.dump(data, open('ci/run-artifacts/shard-tools-interop-results.json', 'w'), indent=2)
print(json.dumps(data, indent=2))
"

echo ""
echo "=== Interop summary: $PASS_COUNT/4 PASS, $FAIL_COUNT/4 FAIL ==="

[ "$FAIL_COUNT" -eq 0 ] || exit 1
