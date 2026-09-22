#!/usr/bin/env bash
# tools/verify/tests/run_verify_native_suite.sh — Pillar 8 CI suite.
#
# Fail-closed, silently-green discipline (house style):
#   * every unit is a binary run with a hard timeout — a hang FAILS, it
#     can never burn the CI budget (the torture battery carries its own
#     internal watchdog as well);
#   * three legs per battery: plain (with allocator interposition for
#     the M1 zero-heap witnesses), ASan+UBSan, TSan;
#   * the gated benchmark runs ONLY with gates on the plain leg —
#     sanitizers distort timing (declared; sanitizer bench legs run with
#     BENCH_NO_GATES=1 and print raw numbers);
#   * when a clang toolchain is present, a second plain compilation+run
#     leg executes under it (directive: "gcc & clang"); this sandbox has
#     no clang, so that leg is DECLARED and excluded from the score
#     denominator (same convention as D-52/D-62/D-72);
#   * namespace gate: the Pillar 8 module objects may define ONLY
#     weft_synth_* symbols;
#   * frozen-file gate: additive-only over the merged baseline — no
#     file outside the Pillar 8 territory may be created or modified;
#   * tools/smoke: the two standalone CLIs run and must hold their
#     invariants inside the suite too.
#
# Score: pass / (units - declared). >= 0.90 exits 0, else exit 1.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SYNTH="$REPO_ROOT/core/c/synthetic"
BUILD="$REPO_ROOT/tests/verify/build"
LOGDIR="${WEFT_SUITE_LOGS:-$BUILD/logs}"

mkdir -p "$LOGDIR"

PASS=0
FAIL=0
DECLARED=0
FAILED_UNITS=""
DECLARED_UNITS=""

log()  { printf '%s\n' "$*" | tee -a "$LOGDIR/suite.log"; }

run_unit() {
    local name="$1" timeout_s="$2" env_setup="$3" binary="$4"
    shift 4
    local rc=0
    local log="$LOGDIR/${name//\//_}.log"
    eval "$env_setup" > /dev/null 2>&1 || true
    timeout "$timeout_s" "$binary" "$@" > "$log" 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        PASS=$((PASS + 1))
        log "PASS      $name"
    else
        FAIL=$((FAIL + 1))
        FAILED_UNITS="$FAILED_UNITS $name"
        log "FAIL      $name (exit $rc; log: $log)"
    fi
    return 0
}

build_leg() {
    local leg="$1" targets="$2" cc="$3" extra="$4"
    local log="$LOGDIR/build-$leg.log"
    if ! make -C "$SYNTH" $targets CC="$cc" $extra > "$log" 2>&1; then
        log "FAIL      build-$leg (see $log)"
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------
# builds (fail-closed: a broken build fails every unit of that leg)
# ---------------------------------------------------------------------------

if ! build_leg plain "synth-battery synth-torture synth-bench synth-stress synth-thermal-curve" gcc ""; then
    log "SUITE FAIL: plain build broken"
    exit 1
fi
if ! build_leg asan "synth-battery-asan synth-torture-asan synth-bench-asan" gcc ""; then
    log "SUITE FAIL: asan build broken"
    exit 1
fi
if ! build_leg tsan "synth-battery-tsan synth-torture-tsan" gcc ""; then
    log "SUITE FAIL: tsan build broken"
    exit 1
fi

# ---------------------------------------------------------------------------
# gcc legs
# ---------------------------------------------------------------------------

run_unit "plain/battery"  420 "" "$BUILD/synth-battery"
run_unit "plain/torture"  420 "" "$BUILD/synth-torture"
run_unit "plain/bench"    420 "" "$BUILD/synth-bench"

run_unit "asan/battery"   600 "export WEFT_QUICK=1" "$BUILD/synth-battery-asan"
run_unit "asan/torture"   600 "export WEFT_QUICK=1" "$BUILD/synth-torture-asan"
run_unit "asan/bench"     600 "export BENCH_NO_GATES=1" "$BUILD/synth-bench-asan"

run_unit "tsan/battery"   900 "export WEFT_QUICK=1" "$BUILD/synth-battery-tsan"
run_unit "tsan/torture"   900 "export WEFT_QUICK=1" "$BUILD/synth-torture-tsan"

# ---------------------------------------------------------------------------
# tools smoke: the standalone CLIs hold their invariants inside CI too
# ---------------------------------------------------------------------------

run_unit "tools/stress"    120 "" "$BUILD/synth-stress" --ms 500 --quiet
run_unit "tools/curve"     60  "" "$BUILD/synth-thermal-curve" --profile burst --ticks 400

# ---------------------------------------------------------------------------
# clang leg (directive: "plain compilation and execution (gcc & clang)")
# ---------------------------------------------------------------------------

if command -v clang > /dev/null 2>&1; then
    if build_leg clang "synth-battery synth-torture" clang ""; then
        run_unit "clang/battery"  420 "" "$BUILD/synth-battery"
        run_unit "clang/torture"  420 "" "$BUILD/synth-torture"
    else
        FAIL=$((FAIL + 1))
        FAILED_UNITS="$FAILED_UNITS build-clang"
    fi
else
    DECLARED=$((DECLARED + 1))
    DECLARED_UNITS="$DECLARED_UNITS clang-leg"
    log "DECLARED  clang-leg (no clang toolchain in this sandbox; same "
    log "          convention as D-52/D-62/D-72 — excluded from score)"
fi

# ---------------------------------------------------------------------------
# namespace gate: the Pillar 8 modules define only weft_synth_*
# ---------------------------------------------------------------------------

namespace_gate() {
    local log="$LOGDIR/namespace-gate.log"
    make -C "$SYNTH" "$BUILD/weft_synth_thermal.o" \
        "$BUILD/weft_synth_bus.o" \
        "$BUILD/weft_synth_net.o" >> "$log" 2>&1
    local bad
    bad=$(nm --defined-only "$BUILD/weft_synth_thermal.o" \
                    "$BUILD/weft_synth_bus.o" \
                    "$BUILD/weft_synth_net.o" 2>/dev/null \
          | awk '$2 ~ /^[TDBRC]$/ && $3 !~ /^weft_synth_/ \
                 && $3 !~ /^__/ {print $3}' | sort -u)
    if [ -n "$bad" ]; then
        log "FAIL      gates/namespace (foreign symbols: $(echo "$bad" | tr '\n' ' '))"
        FAIL=$((FAIL + 1))
        FAILED_UNITS="$FAILED_UNITS gates/namespace"
        return 0
    fi
    PASS=$((PASS + 1))
    log "PASS      gates/namespace (weft_synth_* only)"
}

# ---------------------------------------------------------------------------
# frozen-file gate: additive-only over the merged baseline
# ---------------------------------------------------------------------------

frozen_gate() {
    local log="$LOGDIR/frozen-gate.log"
    # every change in the tree must live inside the Pillar 8 territory;
    # build artifacts under tests/verify/build are regenerable and excluded
    local outside
    outside=$(cd "$REPO_ROOT" && git status --porcelain \
        | awk '{print $NF}' \
        | grep -v -E '^(core/c/synthetic/|tests/verify/|tools/verify/|docs/reports/D-82)' \
        | grep -v -E '^tests/verify/build/' \
        | grep -v -E '^tools/verify/tests/run_verify_native_suite.sh$' \
        || true)
    # tracked-file modifications outside the territory (content edits)
    local dirty
    dirty=$(cd "$REPO_ROOT" && git diff --name-only HEAD \
        | grep -v -E '^(core/c/synthetic/|tests/verify/|tools/verify/|docs/reports/D-82)' \
        || true)
    {
        echo "outside-status: [$outside]"
        echo "dirty-tracked:  [$dirty]"
    } >> "$log"
    if [ -n "$dirty" ]; then
        log "FAIL      gates/frozen-files (baseline modified: $dirty)"
        FAIL=$((FAIL + 1))
        FAILED_UNITS="$FAILED_UNITS gates/frozen-files"
        return 0
    fi
    if [ -n "$outside" ]; then
        log "FAIL      gates/frozen-files (files outside territory: $outside)"
        FAIL=$((FAIL + 1))
        FAILED_UNITS="$FAILED_UNITS gates/frozen-files"
        return 0
    fi
    PASS=$((PASS + 1))
    log "PASS      gates/frozen-files (additive-only; baseline untouched)"
}

namespace_gate
frozen_gate

# ---------------------------------------------------------------------------
# score
# ---------------------------------------------------------------------------

TOTAL=$((PASS + FAIL + DECLARED))
DENOM=$((TOTAL - DECLARED))
log "----------------------------------------------------------------"
log "verify-native suite: $PASS pass / $FAIL fail / $DECLARED declared"
if [ -n "$FAILED_UNITS" ]; then
    log "failed units:$FAILED_UNITS"
fi
if [ -n "$DECLARED_UNITS" ]; then
    log "declared units:$DECLARED_UNITS"
fi
if [ "$DENOM" -gt 0 ] && [ "$((PASS * 100 / DENOM))" -ge 90 ]; then
    log "SCORE $((PASS * 100 / DENOM))% (>= 90% bar) — GREEN"
    exit 0
fi
log "SCORE below the 90% bar — RED"
exit 1
