#!/usr/bin/env bash
# run_f10_parity.sh — F10 100k torture PARITY across the fan-out ports.
#
# WHY EXISTS (the "runs everywhere, proven" gate): every fan-out port
# carries an F10 — the 100k-frame torture with mixer-validated payloads,
# zero tolerance for torn frames ACCEPTED, exact per-reader telescoping
# identity, and convergence on the final frame. Until now those four F10s
# existed separately and nothing proved they stayed at parity (same frame
# count, same reader shape, same gates). This driver makes parity
# mechanical, three ways:
#
#   1. RUN   — C baseline: fanout-runner torture 100000 4 64 3, both
#              ordering regimes (the canonical F10 shape: 100k frames,
#              4 slots, 64-word payloads, 3 readers).
#   2. RUN   — JVM harness (fixtures/jni-fanout): real JVM threads over
#              the same C ring when a JDK is present (loudly skipped
#              otherwise — never silently).
#   3. PARITY-OF-CONTRACT (always): each port's F10 SOURCE must still
#      carry every parity gate — 100k frames, mixer word validation, the
#      telescoping identity assert, convergence. A port that quietly
#      downgrades its F10 (drops the identity assert, shrinks to 1k
#      frames) goes RED here.
#
# Output: ci/run-artifacts/shard-f10-parity.log
#         ci/run-artifacts/f10-parity-results.json
# Exit:   0 only if every RUN leg passed and every PARITY check passed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-f10-parity.log
RESULTS=ci/run-artifacts/f10-parity-results.json
: > "$LOG"

fail=0
c_status="PASS"
jvm_status="SKIPPED (declared)"
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

# --- 1: C baseline, both regimes (the canonical F10 shape) ---
step "C baseline: fanout-runner torture 100000 4 64 3 (fenced acq/rel)"
make -C core/c fanout-runner fanout-runner-seq 2>&1 | tee -a "$LOG"
if ./core/c/fanout-runner torture 100000 4 64 3 2>&1 | tee -a "$LOG"; then :; else fail=1; c_status="FAILED"; fi
step "C baseline: fanout-runner-seq torture 100000 4 64 3 (all-seq_cst)"
if ./core/c/fanout-runner-seq torture 100000 4 64 3 2>&1 | tee -a "$LOG"; then :; else fail=1; c_status="FAILED"; fi

# --- 2: JVM harness over real threads (same C ring, host JDK) ---
step "JVM harness (host JVM threads over the C ring)"
if command -v javac >/dev/null 2>&1 && command -v gcc >/dev/null 2>&1; then
  if EVIDENCE=0 bash fixtures/jni-fanout/run.sh 2>&1 | tee -a "$LOG"; then
    jvm_status="PASS"
  else
    jvm_status="FAILED"
    fail=1
  fi
else
  echo "javac/gcc not found — JVM leg SKIPPED (declared, loud)" | tee -a "$LOG"
  if [ -n "${CI:-}" ]; then
    echo "CI runners carry both toolchains — a missing one is a failure" | tee -a "$LOG"
    jvm_status="FAILED (toolchain missing in CI)"
    fail=1
  fi
fi

# --- 3: parity-of-contract over each port's F10 source (always runs) ---
step "Parity-of-contract: each port's F10 source carries every gate"
check_gate() {
  # check_gate <port> <file> <pattern> <human label>
  local port="$1" file="$2" pattern="$3" label="$4"
  if [ ! -f "$file" ]; then
    echo "  [RED] $port: $label — FILE MISSING: $file" | tee -a "$LOG"
    fail=1
    return
  fi
  if grep -q -- "$pattern" "$file"; then
    echo "  [ok]  $port: $label" | tee -a "$LOG"
  else
    echo "  [RED] $port: $label — pattern not found: $pattern" | tee -a "$LOG"
    fail=1
  fi
}

# Kotlin (FanoutTest.kt): 100k frames, mixer word validation, telescoping
# identity, convergence.
KT=android/weft-core/src/test/kotlin/dev/weft/FanoutTest.kt
check_gate kotlin "$KT" 'val frames = 100_000' "F10 runs 100_000 frames"
check_gate kotlin "$KT" 'expectFrame(r.view(), c.seq.toInt(), words)' "every fresh claim word-validated (mixer)"
check_gate kotlin "$KT" 'st.drops, frames.toLong() - st.fresh' "telescoping identity assert"
check_gate kotlin "$KT" 'assertEquals(readers\[i\].claim().seq, frames.toLong())' "convergence assert"

# Swift (FanoutTests.swift).
SW=Tests/WeftTests/FanoutTests.swift
check_gate swift "$SW" 'let frames = 100_000' "F10 runs 100_000 frames"
check_gate swift "$SW" 'FanoutTests.expectFrame(r.view(), UInt32(c.seq), words)' "every fresh claim word-validated (mixer)"
check_gate swift "$SW" 'UInt64(frames) - st.fresh, st.drops' "telescoping identity assert"
check_gate swift "$SW" 'UInt64(frames), r.claim().seq' "convergence assert"

# Dart (fanout_test.dart) — the single-isolate analog with real
# mid-overwrite windows.
DA=packages/flutter_weft/test/fanout_test.dart
check_gate dart "$DA" 'const frames = 100000' "F10 runs 100000 frames"
check_gate dart "$DA" '_expectFrame(readers\[i\].view(), c.seq, words)' "every fresh claim word-validated (mixer)"
check_gate dart "$DA" 'st.drops, frames - st.fresh' "telescoping identity assert"
check_gate dart "$DA" "expect(r0.claim().seq, frames)" "convergence assert"
check_gate dart "$DA" 'await Future<void>.delayed(Duration.zero)' "mid-overwrite window exercised (split begin/publish)"

# --- results JSON (no jq dependency — hand-assembled, pipefail-safe) ---
python3 - "$RESULTS" "$fail" "$c_status" "$jvm_status" <<'PYEOF'
import json, sys
results_path, fail = sys.argv[1], sys.argv[2]
c_status, jvm_status = sys.argv[3], sys.argv[4]
doc = {
    "gate": "f10-parity",
    "shape": {"frames": 100000, "slots": 4, "words": 64, "readers": 3},
    "legs": {
        "c-fenced": c_status,
        "c-seq_cst": c_status,
        "jvm-harness": jvm_status,
    },
    "parity_of_contract": ["kotlin", "swift", "dart"],
    "status": "PASS" if fail == "0" else "FAILED",
}
with open(results_path, "w") as f:
    json.dump(doc, f, indent=2)
PYEOF

if [ "$fail" -ne 0 ]; then
  echo "" | tee -a "$LOG"
  echo "F10 PARITY: FAILED (see $LOG)" | tee -a "$LOG"
  exit 1
fi
echo "" | tee -a "$LOG"
echo "F10 PARITY: PASS (C both regimes + contract parity kotlin/swift/dart)" | tee -a "$LOG"
