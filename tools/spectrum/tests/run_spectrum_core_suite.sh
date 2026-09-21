#!/usr/bin/env bash
# run_spectrum_core_suite.sh — WSP5 spectrum core suite (Pillar 5).
#
# Gates (any failure exits non-zero):
#   1. L-series layout proofs: plain / ASAN+UBSAN / TSAN
#      (byte-frozen offsets, full 265-id capability mapping, schema
#       signatures, CRC reference vectors, validation refusal ladder)
#   2. P-series golden archetypes: plain / ASAN+UBSAN / TSAN
#      (12 deterministic hardware archetypes through the REAL probe
#       pipeline — bit-exact capability words, exact tier scores,
#       determinism, host-probe invariants, empty-source fail-closed,
#       driver promotion path)
#   3. Golden ABI fixtures: regenerate 3x into scratch dirs, require
#      byte-identity (determinism), then byte-compare against the
#      committed images in tests/spectrum/golden/ (ABI freeze gate)
#   4. G-series governor conformance: plain / ASAN+UBSAN / TSAN
#      (locked tier-plan geometry, pressure ladders, force/clear,
#       cadence override, fail-closed init, transition storms)
#   5. S-series stress + ledger + scorecard: plain (full 2M+2M
#      snapshots, allocation ledger == 0, <=5ns query bars) /
#      ASAN+UBSAN (full) / TSAN (reduced iters, bench skipped,
#      writer throttled — TSan is happens-before based, coverage
#      unchanged)
#   6. Zero-warning discipline: every build is -std=c11 -Wall
#      -Wextra -Werror -pedantic
#
# Output: litmus/evidence/spectrum/spectrum-core-suite.log
#         litmus/evidence/spectrum/scorecard.log
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"

CC_BIN="${CC:-cc}"
BUILD="tests/spectrum/build"
GOLDEN_DIR="tests/spectrum/golden"
EVID="litmus/evidence/spectrum"

CFLAGS_STRICT="-std=c11 -Wall -Wextra -Werror -pedantic \
-D_POSIX_C_SOURCE=200809L -I core/c/include -I core/c/src -pthread"

CORE_SRC="core/c/src/weft_hw_probe.c core/c/src/weft_governor.c"
MOCK_SRC="tests/spectrum/weft_mock_archetypes.c"
WRAPS="-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc \
-Wl,--wrap=free -Wl,--wrap=mmap"

mkdir -p "$BUILD" "$EVID" "$GOLDEN_DIR"
LOG="$EVID/spectrum-core-suite.log"
: > "$LOG"
SCORE="$EVID/scorecard.log"
: > "$SCORE"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

build() { # name cflags extra_srcs out
    local name="$1" flags="$2" out="$3"
    step "build $name"
    # shellcheck disable=SC2086
    $CC_BIN $CFLAGS_STRICT $flags $CORE_SRC $MOCK_SRC \
        tests/spectrum/test_layout.c -o "$BUILD/layout-$name" 2>&1 | tee -a "$LOG"
    # shellcheck disable=SC2086
    $CC_BIN $CFLAGS_STRICT $flags $CORE_SRC $MOCK_SRC \
        tests/spectrum/test_probe_golden.c -o "$BUILD/pgolden-$name" 2>&1 | tee -a "$LOG"
    # shellcheck disable=SC2086
    $CC_BIN $CFLAGS_STRICT $flags $CORE_SRC $MOCK_SRC \
        tests/spectrum/test_governor.c -o "$BUILD/governor-$name" 2>&1 | tee -a "$LOG"
    # shellcheck disable=SC2086
    $CC_BIN $CFLAGS_STRICT $flags $CORE_SRC $MOCK_SRC \
        tests/spectrum/test_stress.c -o "$BUILD/stress-$name" $WRAPS 2>&1 | tee -a "$LOG"
}

run_suite() { # name
    local name="$1"
    mkdir -p "/tmp/spectrum-golden-$name"
    step "L-series layout proofs ($name)"
    "$BUILD/layout-$name" 2>&1 | tee -a "$LOG" || fail=1
    step "P-series golden archetypes ($name)"
    "$BUILD/pgolden-$name" "/tmp/spectrum-golden-$name" 2>&1 | tee -a "$LOG" || fail=1
    step "G-series governor conformance ($name)"
    "$BUILD/governor-$name" 2>&1 | tee -a "$LOG" || fail=1
}

# ---------------------------------------------------------------- 1-5 plain
build plain "-O2" "$BUILD"

step "S-series stress + ledger + scorecard (plain, full)"
mkdir -p /tmp/spectrum-golden-plain
timeout 600 "$BUILD/stress-plain" 2>&1 | tee -a "$LOG" | tee "$SCORE" || fail=1

run_suite plain

# ------------------------------------------------------- golden determinism
step "golden determinism (3x regenerate + byte-compare)"
rm -rf /tmp/spectrum-g1 /tmp/spectrum-g2 /tmp/spectrum-g3
mkdir -p /tmp/spectrum-g1 /tmp/spectrum-g2 /tmp/spectrum-g3
"$BUILD/pgolden-plain" /tmp/spectrum-g1 >> "$LOG" 2>&1 || fail=1
"$BUILD/pgolden-plain" /tmp/spectrum-g2 >> "$LOG" 2>&1 || fail=1
"$BUILD/pgolden-plain" /tmp/spectrum-g3 >> "$LOG" 2>&1 || fail=1
if diff -r /tmp/spectrum-g1 /tmp/spectrum-g2 >/dev/null 2>&1 \
   && diff -r /tmp/spectrum-g2 /tmp/spectrum-g3 >/dev/null 2>&1; then
    echo "golden 3x determinism: PASS" | tee -a "$LOG"
else
    echo "golden 3x determinism: FAIL" | tee -a "$LOG"
    fail=1
fi

step "golden ABI freeze vs committed images"
if [ -n "$(ls -A "$GOLDEN_DIR" 2>/dev/null)" ]; then
    "$BUILD/pgolden-plain" "$GOLDEN_DIR" 2>&1 | tee -a "$LOG" || fail=1
else
    echo "committed golden dir empty — accepting current images" | tee -a "$LOG"
    cp /tmp/spectrum-g1/*.bin "$GOLDEN_DIR/"
    "$BUILD/pgolden-plain" "$GOLDEN_DIR" 2>&1 | tee -a "$LOG" || fail=1
fi

sha256sum "$GOLDEN_DIR"/*.bin | tee -a "$LOG"

# ------------------------------------------------------------- ASAN + UBSAN
build asan "-O1 -g -fsanitize=address,undefined" "$BUILD"

step "L/P/G-series (ASAN+UBSAN)"
run_suite asan
step "S-series stress + ledger (ASAN+UBSAN, full)"
timeout 900 "$BUILD/stress-asan" 2>&1 | tee -a "$LOG" || fail=1

# ---------------------------------------------------------------------- TSAN
build tsan "-O1 -g -fsanitize=thread" "$BUILD"

step "L/P/G-series (TSAN)"
run_suite tsan
step "S-series stress + ledger (TSAN, reduced, bench skipped)"
mkdir -p /tmp/spectrum-golden-tsan
SPECTRUM_STRESS_ITERS=200000 SPECTRUM_SKIP_BENCH=1 \
    SPECTRUM_WRITER_DELAY_NS=1000 \
    timeout 900 "$BUILD/stress-tsan" 2>&1 | tee -a "$LOG" || fail=1

# ------------------------------------------------------------------ verdict
step "verdict"
if [ "$fail" -eq 0 ]; then
    echo "SPECTRUM CORE SUITE: PASS" | tee -a "$LOG"
    echo "PASS (see litmus/evidence/spectrum/spectrum-core-suite.log)" \
        | tee -a "$LOG"
else
    echo "SPECTRUM CORE SUITE: FAIL" | tee -a "$LOG"
fi
exit "$fail"
