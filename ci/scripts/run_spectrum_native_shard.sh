#!/usr/bin/env bash
# ci/scripts/run_spectrum_native_shard.sh — CI shard wrapper for the
# Pillar 5 (weft-spectrum) native suite. Drives the canonical runner:
#   tools/spectrum/tests/run_spectrum_native_suite.sh
# Everything fail-closed: the shard's exit code IS the suite's verdict.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# CI runners are noisy shared machines: keep the TSan leg (the module is
# single-writer by contract — TSan verifies the atomics discipline), but
# the timing gates come from the plain build only (declared in D-52).
export SPECTRUM_SKIP_TSAN="${SPECTRUM_SKIP_TSAN:-0}"

"${REPO_ROOT}/tools/spectrum/tests/run_spectrum_native_suite.sh"
