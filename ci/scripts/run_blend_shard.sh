#!/usr/bin/env bash
# run_blend_shard.sh — the Series-8 SIMD blend shard.
#
# 1. The C kernel's own gates (boundaries, 154-check parity sweep, the
#    full 0..4096 alpha sweep, in-place aliasing).
# 2. The xlang-blend golden gate: the committed CSV must match the
#    kernel byte-for-byte, and every present-toolchain port (TS required;
#    Kotlin/Dart per leg; Swift declared skip) must reproduce all 63
#    golden digests.
# 3. The blend_runner numbers, recorded into the shard log (hot / 1080p /
#    4K / 8K, scalar vs dispatched SIMD).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-simd-blend.log
: > "$LOG"
exec 2> >(tee -a "$LOG" >&2)

fail=0

echo "== simd-blend shard: env $(uname -m)/$(uname -s) ==" | tee -a "$LOG"

if ! command -v gcc >/dev/null 2>&1; then
  echo "FATAL: no C toolchain on host — the golden source cannot be verified" | tee -a "$LOG" >&2
  exit 1
fi

echo "-- C kernel gates --" | tee -a "$LOG"
make -s -C core/c blend-test blend-runner || { echo "C build failed" >&2; exit 1; }
if (cd core/c && ./blend-test); then
  echo "blend-test: ALL GATES GREEN" | tee -a "$LOG"
else
  echo "blend-test RED" >&2
  fail=1
fi

echo "-- xlang-blend golden gate --" | tee -a "$LOG"
if bash fixtures/xlang-blend/run.sh; then
  echo "xlang-blend parity GREEN" | tee -a "$LOG"
else
  echo "xlang-blend parity RED" >&2
  fail=1
fi

echo "-- blend_runner benchmark --" | tee -a "$LOG"
(cd core/c && ./blend-runner) 2>&1 | tee -a "$LOG"

if [ "$fail" -ne 0 ]; then
  echo "❌ simd-blend shard RED" | tee -a "$LOG" >&2
  exit 1
fi
echo "✅ simd-blend shard GREEN" | tee -a "$LOG"
