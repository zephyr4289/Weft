#!/usr/bin/env bash
# run_weftc_shard.sh — RFC-0017 weftc schema-compiler shard.
#
# Gates (any failure exits non-zero):
#   1. Full weftc gate, plain build: WL layout conformance + WH hash
#      quality (avalanche, mutation matrix, 1M collision corpus) + WD
#      artifact goldens + CLI exit codes + external JSON validation
#   2. The verify-header handshake demo: hand-written C structs must
#      compile clean against the generated _Static_assert pins (positive)
#      and a field-swapped drift variant must FAIL to compile (negative)
#   3. The entire gate re-run with every binary built under ASAN
#      (Law 2 memory-safety audit)
#
# Zero kernel surface: tools/weftc only; no core/, packages/, or frozen
# file is read or written by the compiler itself.
#
# Output: ci/run-artifacts/shard-weftc.log
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-weftc.log
: > "$LOG"

step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

step "weftc full gate (plain: WL + WH + WD + CLI + goldens)"
make -C tools/weftc clean-bin 2>&1 | tee -a "$LOG"
make -C tools/weftc test 2>&1 | tee -a "$LOG"

step "weftc full gate (ASAN leg — Law 2 memory-safety audit)"
make -C tools/weftc clean-bin 2>&1 | tee -a "$LOG"
make -C tools/weftc test-asan 2>&1 | tee -a "$LOG"

step "summary"
if grep -q "weftc gate: ALL PASS" "$LOG"; then
    echo "weftc shard: ALL PASS (both legs)" | tee -a "$LOG"
else
    echo "weftc shard: gate line missing — treating as failure" | tee -a "$LOG"
    exit 1
fi
