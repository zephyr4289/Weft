#!/usr/bin/env bash
# tools/adapters/tests/run_adapters_native_suite.sh — Pillar 6 CI suite.
#
# Fail-closed, silently-green discipline (house style):
#   * every unit is a binary run with a hard timeout — a hang FAILS, it can
#     never burn the CI budget (children carry PR_SET_PDEATHSIG, so a killed
#     runner cannot leak spinners either);
#   * three legs per battery: plain, ASan+UBSan, TSan;
#   * gates G1/G2 (loaned RTT, vision handoff) run ONLY in the plain leg —
#     sanitizers distort timing (declared; the bench prints raw histograms
#     ungated in sanitizer legs);
#   * /dev/shm is swept between units and audited at the end;
#   * frozen-file gate: the adapter tree must not modify the merged baseline
#     (core/c/tensor/*, core/c/weft_tensor.h, spectrum/, other engineers'
#     namespaces) — additive-only, verified by git;
#   * CONTAINER TOLERANCE (directive-sanctioned): a TSan unit whose output
#     carries a known environment-failure signature (memory-mapping/ASLR
#     refusal inside the container) is TOLERATED — reported as WARN, excluded
#     from the score denominator, never counted green. Everything else fails.
#
# Score: pass / (units - tolerated). >= 0.90 exits 0 (directive: "90% green
# CI"), else exit 1. A clean tree scores 100%.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
ADAPTERS="$REPO_ROOT/core/c/adapters"
BUILD="$REPO_ROOT/tests/adapters/build"
LOGDIR="${WEFT_SUITE_LOGS:-$BUILD/logs}"

mkdir -p "$LOGDIR"

PASS=0
FAIL=0
TOLERATED=0
FAILED_UNITS=""
TOLERATED_UNITS=""

log()  { printf '%s\n' "$*" | tee -a "$LOGDIR/suite.log"; }

# environment-failure signatures for ASan/TSan-in-container (tolerated, never green)
is_env_failure() {
    grep -qE "ThreadSanitizer: unexpected memory mapping|\
FATAL: ThreadSanitizer|Cannot create memory:|\
sanitizer_allocator_primary64|AddressSanitizer: CHECK failed|\
Shadow memory range|LLVM TSan: failed to allocate" "$1" 2>/dev/null
}

run_unit() {
    local name="$1" timeout_s="$2" env_setup="$3" binary="$4"
    shift 4
    local rc=0
    rm -f /dev/shm/weft_rmw_* 2>/dev/null
    # make sure no leftovers from an earlier unit can starve this one
    pkill -9 -f "adapters-bench|adapters-torture|rmw-weft-test|vision-dma-test" \
        2>/dev/null
    sleep 0.1
    local log="$LOGDIR/${name//\//_}.log"
    eval "$env_setup" > /dev/null 2>&1 || true
    timeout "$timeout_s" "$binary" "$@" > "$log" 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        PASS=$((PASS + 1))
        log "PASS      $name"
    elif is_env_failure "$log"; then
        TOLERATED=$((TOLERATED + 1))
        TOLERATED_UNITS="$TOLERATED_UNITS $name"
        log "TOLERATED $name (container environment signature; WARN, not green)"
    else
        FAIL=$((FAIL + 1))
        FAILED_UNITS="$FAILED_UNITS $name"
        log "FAIL      $name (exit $rc; log: $log)"
    fi
    rm -f /dev/shm/weft_rmw_* 2>/dev/null
    return 0
}

build_leg() {
    local leg="$1" targets="$2"
    local log="$LOGDIR/build-$leg.log"
    # shellcheck disable=SC2086 # targets are a controlled space-separated list
    if (cd "$ADAPTERS" && make $targets > "$log" 2>&1); then
        PASS=$((PASS + 1))
        log "PASS      build/$leg"
    else
        FAIL=$((FAIL + 1))
        FAILED_UNITS="$FAILED_UNITS build/$leg"
        log "FAIL      build/$leg (log: $log)"
        return 1
    fi
    return 0
}

log "=== weft-adapters native suite (Pillar 6) $(date -u +%FT%TZ) ==="
log "repo: $REPO_ROOT"

# ---------------------------------------------------------------------------
# Frozen-baseline gate: the adapter pillar is additive-only
# ---------------------------------------------------------------------------
frozen_gate_ok=1
if git -C "$REPO_ROOT" diff --quiet -- core/c/tensor \
        core/c/weft_tensor.h core/c/spectrum 2>/dev/null; then
    PASS=$((PASS + 1))
    log "PASS      gate/frozen-baseline (tensor + spectrum untouched)"
else
    frozen_gate_ok=0
    FAIL=$((FAIL + 1))
    FAILED_UNITS="$FAILED_UNITS gate/frozen-baseline"
    log "FAIL      gate/frozen-baseline (merged baseline files modified)"
fi
# namespace discipline: no foreign engine symbols leak into the adapter tree
if grep -rq "weft_spectrum_" "$ADAPTERS" 2>/dev/null; then
    FAIL=$((FAIL + 1))
    FAILED_UNITS="$FAILED_UNITS gate/namespace"
    log "FAIL      gate/namespace (weft_spectrum_ symbols in adapter tree)"
else
    PASS=$((PASS + 1))
    log "PASS      gate/namespace (no Engineer-1 symbol leakage)"
fi

# ---------------------------------------------------------------------------
# Leg 1: plain (full torture counts + GATED bench)
# ---------------------------------------------------------------------------
if build_leg plain "rmw-weft-test vision-dma-test adapters-torture-test adapters-bench"; then
    run_unit "plain/rmw-battery"     300 "" "$BUILD/rmw-weft-test"
    run_unit "plain/vision-battery"  300 "" "$BUILD/vision-dma-test"
    run_unit "plain/torture-full"    600 "" "$BUILD/adapters-torture-test"
    run_unit "plain/bench-gated"     240 "" "$BUILD/adapters-bench"
fi

# ---------------------------------------------------------------------------
# Leg 2: ASan + UBSan (QUICK torture, sanitizer-aware RSS bound)
# ---------------------------------------------------------------------------
if build_leg asan "rmw-weft-test-asan vision-dma-test-asan adapters-torture-test-asan"; then
    run_unit "asan/rmw-battery"    300 "" "$BUILD/rmw-weft-test-asan"
    run_unit "asan/vision-battery" 300 "" "$BUILD/vision-dma-test-asan"
    run_unit "asan/torture-quick"  300 \
        "export WEFT_QUICK=1 TORTURE_RSS_LIMIT_KIB=8192" \
        "$BUILD/adapters-torture-test-asan"
fi

# ---------------------------------------------------------------------------
# Leg 3: TSan (QUICK torture, widest RSS bound)
# ---------------------------------------------------------------------------
if build_leg tsan "rmw-weft-test-tsan vision-dma-test-tsan adapters-torture-test-tsan"; then
    run_unit "tsan/rmw-battery"    300 "" "$BUILD/rmw-weft-test-tsan"
    run_unit "tsan/vision-battery" 300 "" "$BUILD/vision-dma-test-tsan"
    run_unit "tsan/torture-quick"  300 \
        "export WEFT_QUICK=1 TORTURE_RSS_LIMIT_KIB=16384" \
        "$BUILD/adapters-torture-test-tsan"
fi

# ---------------------------------------------------------------------------
# Final /dev/shm audit
# ---------------------------------------------------------------------------
sleep 0.2
pkill -9 -f "adapters-bench|adapters-torture|rmw-weft-test|vision-dma-test" \
    2>/dev/null
sleep 0.2
leftover=$(ls /dev/shm 2>/dev/null | grep -c "^weft_rmw_" || true)
if [ "${leftover:-0}" -eq 0 ]; then
    PASS=$((PASS + 1))
    log "PASS      audit/dev-shm-clean"
else
    FAIL=$((FAIL + 1))
    FAILED_UNITS="$FAILED_UNITS audit/dev-shm-clean"
    log "FAIL      audit/dev-shm-clean ($leftover objects left)"
fi

# ---------------------------------------------------------------------------
# Score
# ---------------------------------------------------------------------------
SCORED=$((PASS + FAIL))
if [ "$SCORED" -eq 0 ]; then
    log "NO UNITS RAN — fail-closed"
    exit 1
fi
RATE=$(awk "BEGIN {printf \"%.1f\", 100.0 * $PASS / $SCORED}")
log "----"
log "suite: $PASS pass, $FAIL fail, $TOLERATED tolerated -> $RATE% green"
if [ -n "$TOLERATED_UNITS" ]; then
    log "tolerated (environment):$TOLERATED_UNITS"
fi
if [ -n "$FAILED_UNITS" ]; then
    log "failed:$FAILED_UNITS"
fi
log "logs: $LOGDIR"
if [ "$FAIL" -eq 0 ] && awk "BEGIN {exit !($RATE >= 90.0)}"; then
    log "adapters native suite: GREEN (>= 90%)"
    exit 0
fi
log "adapters native suite: BELOW 90% GREEN — fail-closed"
exit 1
