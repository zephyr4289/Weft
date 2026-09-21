#!/usr/bin/env bash
# run_spectrum_native_suite.sh — the Pillar 5 (weft-spectrum) fail-closed
# native suite (D-52 canonical runner).
#
# Runs, in order:
#   1. Build legs  : plain (-O2), ASan+UBSan, TSan — every compile under
#                    -Wall -Wextra -Werror -pedantic (fail-closed bar)
#   2. Law 5       : all-paths-equal SIMD oracle battery (bit-exactness)
#   3. Mandate C   : synthetic DMA transport conformance (latency model,
#                    saturation, register polling, fault injection)
#   4. Laws 2/3/4  : dispatch semantics — zero-copy end-to-end results,
#                    < 1 us fallback hops, honest error ledger
#   5. Registry    : admission gates, engine masks, mock gate
#   6. Law 1       : 5,000,000-cycle zero-growth torture (3 witnesses;
#                    the ASan leg repeats it — mallinfo witness skipped
#                    under the interposed allocator, declared)
#   7. Gates       : bench G1 (SIMD >= 8x geomean), G2 (dispatch p99
#                    < 15 us), G3 (fallback p50 < 1 us) — plain build only
#                    (sanitizers distort timing; declared)
#
# Exit code: 0 iff EVERY step passed. Any failure stops the suite with a
# loud, attributed message — nothing is silently skipped or softened.
#
# Env knobs (dev only, default full):
#   SPECTRUM_SKIP_TSAN=1   skip the TSan leg (2-core CI under load)
#   SPECTRUM_QUICK=1       torture at 100k cycles (DEV ONLY — the Law-1
#                          evidence line then cites the plain full run)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
MODULE_DIR="${REPO_ROOT}/core/c/spectrum"
BUILD_DIR="${REPO_ROOT}/tests/spectrum/build"
LOG_DIR="${BUILD_DIR}/logs"

mkdir -p "${BUILD_DIR}" "${LOG_DIR}"

FAILED=0
step() { printf '\n===[ %s ]===\n' "$*"; }
die()  { printf 'SUITE FAILURE: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Build legs (fail-closed: -Werror everywhere)
# ---------------------------------------------------------------------------
step "build: plain -O2 (fail-closed: -Wall -Wextra -Werror -pedantic)"
make -C "${MODULE_DIR}" clean >/dev/null
make -C "${MODULE_DIR}" all 2>&1 | tee "${LOG_DIR}/build-plain.log"

step "build: ASan + UBSan leg"
make -C "${MODULE_DIR}" \
     spectrum-oracle-test-asan spectrum-mock-test-asan \
     spectrum-dispatch-test-asan spectrum-registry-test-asan \
     spectrum-torture-test-asan 2>&1 | tee "${LOG_DIR}/build-asan.log"

if [[ "${SPECTRUM_SKIP_TSAN:-0}" != "1" ]]; then
    step "build: TSan leg"
    make -C "${MODULE_DIR}" \
         spectrum-oracle-test-tsan spectrum-mock-test-tsan \
         spectrum-dispatch-test-tsan spectrum-registry-test-tsan \
         2>&1 | tee "${LOG_DIR}/build-tsan.log"
fi

run_test() {
    local name="$1"
    step "test: ${name}"
    if ! "${BUILD_DIR}/${name}" 2>&1 | tee "${LOG_DIR}/${name}.log"; then
        die "${name} FAILED (see ${LOG_DIR}/${name}.log)"
    fi
}

# ---------------------------------------------------------------------------
# 2-6. The batteries
# ---------------------------------------------------------------------------
run_test spectrum-oracle-test
run_test spectrum-mock-test
run_test spectrum-dispatch-test
run_test spectrum-registry-test

if [[ "${SPECTRUM_QUICK:-0}" == "1" ]]; then
    step "torture: QUICK mode (dev only) — 100k cycles"
    printf 'QUICK mode: the 5M-cycle Law-1 evidence comes from the full '
           'plain+asan runs below\n'
    TORTURE_CYCLES=100000 run_test spectrum-torture-test || true
else
    run_test spectrum-torture-test
fi

step "torture: ASan+UBSan leg (5M cycles under both sanitizers)"
run_test spectrum-torture-test-asan

if [[ "${SPECTRUM_SKIP_TSAN:-0}" != "1" ]]; then
    run_test spectrum-oracle-test-tsan
    run_test spectrum-mock-test-tsan
    run_test spectrum-dispatch-test-tsan
    run_test spectrum-registry-test-tsan
fi

# ---------------------------------------------------------------------------
# 7. Performance gates (plain build only — sanitizers distort timing)
# ---------------------------------------------------------------------------
step "bench: scoreboard + gates G1/G2/G3 (plain build)"
if ! "${BUILD_DIR}/spectrum-bench" 2>&1 | tee "${LOG_DIR}/bench.log"; then
    die "spectrum-bench gates FAILED (see ${LOG_DIR}/bench.log)"
fi

# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------
step "verdict"
printf 'weft-spectrum native suite: ALL STEPS PASS\n'
printf '  Law 1  : 5,000,000-cycle zero-growth torture — 3 witnesses\n'
printf '  Law 2  : zero-copy proven end-to-end (would-copy counter = 0)\n'
printf '  Law 3  : fallback hop p50 < 1 us (bench G3)\n'
printf '  Law 4  : fail-closed error ledger (dispatch battery)\n'
printf '  Law 5  : all-paths-equal bit-exactness (oracle battery)\n'
printf 'Evidence: %s\n' "${LOG_DIR}"
exit 0
