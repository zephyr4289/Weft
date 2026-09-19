#!/usr/bin/env bash
# run_python_binding_shard.sh — Python binding gate (issue #18-6, Series 9).
#
# Builds the cffi extension from the SAME C sources every other native
# binding uses (weft.c, fanout.c, fanout_simd.c, fanout_batch.c) and runs
# the PL-series battery: kernel roundtrip + I6 lifecycle + zero-copy views,
# L-series analogs, fan-out roundtrip/drops/raw-ring, 100-frame batch,
# numpy zero-copy, concurrent reader-thread torture, RSS stability.
#
# Output: ci/run-artifacts/shard-python-binding.log + -results.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-python-binding.log
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

step "python toolchain"
python3 --version | tee -a "$LOG"

step "build cffi extension in-tree"
(cd core/python && python3 setup.py build_ext --inplace) 2>&1 | tee -a "$LOG" || fail=1

step "PL-series battery (11 tests: kernel/litmus/fanout/batch/numpy/torture/rss)"
(cd core/python && python3 test_weft.py) 2>&1 | tee -a "$LOG" || fail=1

step "pip packaging sanity (sdist metadata builds)"
if python3 -m pip --version >/dev/null 2>&1; then
  (cd core/python && python3 -m pip install --no-build-isolation --no-deps . \
     && python3 -c "import weft, sys; sys.exit(0) if weft.__version__ else sys.exit(1)") \
     2>&1 | tee -a "$LOG" || fail=1
  # NOTE: PyPI upload itself is a maintainer step (credentials) — declared.
fi

if [ "$fail" -ne 0 ]; then
  echo '{"shard":"python-binding","status":"FAILED"}' > ci/run-artifacts/shard-python-binding-results.json
  exit 1
fi
echo '{"shard":"python-binding","status":"PASSED","gates":"cffi build + PL-series (11) + pip install sanity"}' > ci/run-artifacts/shard-python-binding-results.json
echo "✅ python-binding shard PASSED" | tee -a "$LOG"
