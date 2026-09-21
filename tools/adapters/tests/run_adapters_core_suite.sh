#!/usr/bin/env bash
#
# run_adapters_core_suite.sh - Weft Pillar 6 (weft-adapters) verification
#                              suite. The canonical CI shard entry.
#
# Gates (all must pass; the suite exits non-zero on any failure):
#   G1  Strict compile: gcc AND clang, -std=c11 -Wall -Wextra -Werror
#       -pedantic, library sources + all tests (Law 3).
#   G2  Golden oracles: ITCH 5.0 / OUCH 5.0 / MoldUDP64 / SBE bit-exact
#       + synthetic PCAP fixtures + checksum vectors (K/O/S series).
#   G3  Fuzz & torture: 10,000,000 malformed/truncated byte-injection
#       cycles (WEFT_FUZZ_CYCLES to override), zero crashes, legal
#       statuses only, zero heap growth.
#   G4  Allocation probe: malloc interposition, 200k cycles x 7 decode
#       windows, EXACTLY ZERO allocation events (Law 1).
#   G5  Sanitizer pass: ASAN + UBSAN on oracles + fuzz (200k cycles).
#   G6  Header FFI safety: g++ -std=c++17 -fsyntax-only include check.
#   G7  Optional (evidence, non-fatal): -march=native static-CRC build,
#       aarch64 cross syntax check when a cross toolchain exists.
#
# Usage: tools/adapters/tests/run_adapters_core_suite.sh
#         [--quick]  (reduces fuzz cycles to 200k for smoke runs)
# Env:    WEFT_FUZZ_CYCLES, WEFT_PROBE_CYCLES, CC, CXX, CLANG_BIN
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT"

CFLAGS_STRICT="-std=c11 -Wall -Wextra -Werror -pedantic"
OPT="-O2"
INC="-Icore/c/include"
SRC="core/c/src/adapters/weft_itch50.c core/c/src/adapters/weft_sbe.c core/c/src/adapters/weft_adapter_checksum.c"
TESTS_DIR="tests/adapters/core"
EV="litmus/evidence/adapters"
OUT="build/adapters"
mkdir -p "$OUT" "$EV"

QUICK=0
if [ "${1:-}" = "--quick" ]; then QUICK=1; fi

FUZZ_CYCLES="${WEFT_FUZZ_CYCLES:-10000000}"
if [ "$QUICK" = "1" ] && [ -z "${WEFT_FUZZ_CYCLES:-}" ]; then FUZZ_CYCLES=200000; fi
PROBE_CYCLES="${WEFT_PROBE_CYCLES:-200000}"

GCC_BIN="${CC:-gcc}"
CXX_BIN="${CXX:-g++}"
CLANG_BIN="${CLANG_BIN:-}"
if [ -z "$CLANG_BIN" ] && command -v clang >/dev/null 2>&1; then
    CLANG_BIN="$(command -v clang)"
fi
# user-local extracted clang fallback (sandbox provisioning)
if [ -z "$CLANG_BIN" ] && [ -x "$HOME/../z/scripts/clang-root/usr/lib/llvm-19/bin/clang" ]; then
    CLANG_ROOT="/home/z/my-project/scripts/clang-root"
elif [ -z "$CLANG_BIN" ] && [ -x "/home/z/my-project/scripts/clang-root/usr/lib/llvm-19/bin/clang" ]; then
    CLANG_ROOT="/home/z/my-project/scripts/clang-root"
fi
if [ -n "${CLANG_ROOT:-}" ]; then
    export PATH="$CLANG_ROOT/usr/lib/llvm-19/bin:$PATH"
    export LD_LIBRARY_PATH="$CLANG_ROOT/usr/lib/llvm-19/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    CLANG_BIN="$CLANG_ROOT/usr/lib/llvm-19/bin/clang"
fi

GATE_FAIL=0
note()  { printf '[adapters] %s\n' "$*"; }
fail()  { printf '[adapters] GATE-FAIL: %s\n' "$*"; GATE_FAIL=1; }

# ----------------------------------------------------------------------
# G1 + G2: gcc strict build + oracles + layout dump
# ----------------------------------------------------------------------
note "G1/G2: gcc strict build + oracle run"
mkdir -p "$TESTS_DIR/fixtures"
{
    $GCC_BIN $CFLAGS_STRICT $OPT $INC -DWEFT_TEST_BUILD \
        $TESTS_DIR/test_oracle_itch50.c $SRC -o $OUT/test_oracle_itch50 &&
    $GCC_BIN $CFLAGS_STRICT $OPT $INC \
        $TESTS_DIR/test_oracle_sbe.c $SRC -o $OUT/test_oracle_sbe &&
    $GCC_BIN $CFLAGS_STRICT $OPT $INC \
        $TESTS_DIR/test_checksum.c $SRC -o $OUT/test_checksum &&
    $GCC_BIN $CFLAGS_STRICT $OPT $INC \
        tools/adapters/layout_dump.c $SRC -o $OUT/layout_dump
} > "$EV/gcc-strict-build.log" 2>&1 || fail "gcc strict compile"

if [ "$GATE_FAIL" = "0" ]; then
    WEFT_FIXTURES_DIR="$TESTS_DIR/fixtures" $OUT/test_oracle_itch50 \
        | tee "$EV/oracle-itch50.log" || fail "oracle itch50"
    WEFT_FIXTURES_DIR="$TESTS_DIR/fixtures" $OUT/test_oracle_sbe \
        | tee "$EV/oracle-sbe.log" || fail "oracle sbe"
    $OUT/test_checksum | tee "$EV/checksum.log" || fail "checksum"
    $OUT/layout_dump > "$EV/layout-dump.log" 2>&1 || fail "layout dump"
fi

# ----------------------------------------------------------------------
# G1: clang strict build (both compilers per Law 3)
# ----------------------------------------------------------------------
if [ -n "$CLANG_BIN" ] && [ -x "$CLANG_BIN" ]; then
    note "G1: clang strict build ($($CLANG_BIN --version | head -1))"
    {
        $CLANG_BIN $CFLAGS_STRICT $OPT $INC \
            $TESTS_DIR/test_oracle_itch50.c $SRC -o $OUT/test_oracle_itch50_clang &&
        $CLANG_BIN $CFLAGS_STRICT $OPT $INC \
            $TESTS_DIR/test_oracle_sbe.c $SRC -o $OUT/test_oracle_sbe_clang &&
        $CLANG_BIN $CFLAGS_STRICT $OPT $INC \
            $TESTS_DIR/test_checksum.c $SRC -o $OUT/test_checksum_clang &&
        $CLANG_BIN $CFLAGS_STRICT $OPT $INC \
            $TESTS_DIR/test_bench_throughput.c $SRC -o $OUT/test_bench_clang &&
        $CLANG_BIN $CFLAGS_STRICT $OPT $INC \
            $TESTS_DIR/test_fuzz_torture.c $SRC -o $OUT/test_fuzz_clang
    } > "$EV/clang-strict-build.log" 2>&1 || fail "clang strict compile"
    if [ "$GATE_FAIL" = "0" ]; then
        WEFT_FUZZ_CYCLES=50000 $OUT/test_fuzz_clang \
            | tee "$EV/fuzz-clang.log" || fail "clang fuzz run"
    fi
else
    note "G1: clang NOT FOUND - declared boundary for this host; gcc gate still enforced"
fi

# ----------------------------------------------------------------------
# G3: fuzz & torture (gcc, full cycles)
# ----------------------------------------------------------------------
note "G3: fuzz & torture - $FUZZ_CYCLES cycles"
{
    $GCC_BIN $CFLAGS_STRICT $OPT $INC \
        $TESTS_DIR/test_fuzz_torture.c $SRC -o $OUT/test_fuzz
} > "$EV/fuzz-build.log" 2>&1 || fail "fuzz compile"
if [ "$GATE_FAIL" = "0" ]; then
    WEFT_FUZZ_CYCLES=$FUZZ_CYCLES $OUT/test_fuzz \
        | tee "$EV/fuzz-torture.log" || fail "fuzz torture run"
fi

# ----------------------------------------------------------------------
# G4: allocation probe (malloc interposition; plain build, no sanitizers)
# ----------------------------------------------------------------------
note "G4: allocation probe (Law 1) - $PROBE_CYCLES cycles/window"
{
    $GCC_BIN $CFLAGS_STRICT $OPT $INC -DWEFT_ENABLE_MALLOC_PROBE \
        $TESTS_DIR/test_alloc_probe.c $SRC -ldl -o $OUT/test_alloc_probe
} > "$EV/probe-build.log" 2>&1 || fail "alloc probe compile"
if [ "$GATE_FAIL" = "0" ]; then
    WEFT_PROBE_CYCLES=$PROBE_CYCLES $OUT/test_alloc_probe \
        | tee "$EV/alloc-probe.log" || fail "alloc probe run"
fi

# ----------------------------------------------------------------------
# G5: sanitizers (ASAN + UBSAN), reduced fuzz cycles
# ----------------------------------------------------------------------
note "G5: ASAN+UBSAN pass"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
{
    $GCC_BIN $CFLAGS_STRICT -O1 $INC $SAN \
        $TESTS_DIR/test_oracle_itch50.c $SRC -o $OUT/test_oracle_itch50_asan &&
    $GCC_BIN $CFLAGS_STRICT -O1 $INC $SAN \
        $TESTS_DIR/test_oracle_sbe.c $SRC -o $OUT/test_oracle_sbe_asan &&
    $GCC_BIN $CFLAGS_STRICT -O1 $INC $SAN \
        $TESTS_DIR/test_checksum.c $SRC -o $OUT/test_checksum_asan &&
    $GCC_BIN $CFLAGS_STRICT -O1 $INC $SAN \
        $TESTS_DIR/test_fuzz_torture.c $SRC -o $OUT/test_fuzz_asan
} > "$EV/asan-build.log" 2>&1 || fail "asan compile"
if [ "$GATE_FAIL" = "0" ]; then
    WEFT_FIXTURES_DIR="$TESTS_DIR/fixtures" \
        ASAN_OPTIONS=detect_leaks=1 \
        $OUT/test_oracle_itch50_asan | tee "$EV/oracle-itch50-asan.log" \
        || fail "oracle itch50 asan"
    WEFT_FIXTURES_DIR="$TESTS_DIR/fixtures" \
        ASAN_OPTIONS=detect_leaks=1 \
        $OUT/test_oracle_sbe_asan | tee "$EV/oracle-sbe-asan.log" \
        || fail "oracle sbe asan"
    ASAN_OPTIONS=detect_leaks=1 \
        $OUT/test_checksum_asan | tee "$EV/checksum-asan.log" \
        || fail "checksum asan"
    WEFT_FUZZ_CYCLES=200000 $OUT/test_fuzz_asan \
        | tee "$EV/fuzz-asan.log" || fail "fuzz asan"
fi

# ----------------------------------------------------------------------
# G6: header FFI safety (C++ include)
# ----------------------------------------------------------------------
note "G6: C++ header FFI safety"
{
    printf '#include "weft_adapters.h"\nint main(){weft_itch_msg_t m{}; return (int)sizeof(m)-64;}\n' \
        > $OUT/ffi_smoke.cpp
    $CXX_BIN -std=c++17 -Wall -Wextra -Werror -pedantic $INC \
        -fsyntax-only $OUT/ffi_smoke.cpp
} > "$EV/ffi-cpp.log" 2>&1 || fail "C++ header include"

# ----------------------------------------------------------------------
# G7 (optional evidence): march=native static-CRC + aarch64 cross check
# ----------------------------------------------------------------------
if grep -q sse4.2 /proc/cpuinfo 2>/dev/null; then
    note "G7: -march=native static SSE4.2 CRC build (evidence)"
    {
        $GCC_BIN -std=c11 -Wall -Wextra -Werror -pedantic -O2 -march=native \
            $INC $TESTS_DIR/test_checksum.c $SRC -o $OUT/test_checksum_native &&
        $OUT/test_checksum_native
    } > "$EV/native-build.log" 2>&1 \
        || fail "march=native build/run (informational gate)"
else
    note "G7: no SSE4.2 on this host - skipped"
fi

if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    note "G7: aarch64 cross-compile syntax check"
    aarch64-linux-gnu-gcc -std=c11 -Wall -Wextra -Werror -pedantic $INC \
        -fsyntax-only $SRC > "$EV/aarch64-cross.log" 2>&1 \
        || fail "aarch64 cross syntax check"
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        aarch64-linux-gnu-gcc -std=c11 -Wall -Wextra -Werror -pedantic \
            -march=armv8-a+crc $INC -fsyntax-only $SRC \
            >> "$EV/aarch64-cross.log" 2>&1 \
            || fail "aarch64 crc cross syntax check"
    fi
else
    note "G7: aarch64-linux-gnu-gcc not installed - cross check skipped (declared boundary)"
fi

# ----------------------------------------------------------------------
# Throughput scorecard (informational, D-61 input)
# ----------------------------------------------------------------------
note "bench: throughput scorecard"
{
    $GCC_BIN $CFLAGS_STRICT $OPT $INC \
        $TESTS_DIR/test_bench_throughput.c $SRC -o $OUT/test_bench
} > "$EV/bench-build.log" 2>&1 || fail "bench compile"
if [ "$GATE_FAIL" = "0" ]; then
    $OUT/test_bench | tee "$EV/bench-scorecard.log" || fail "bench run"
fi

# ----------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------
echo
note "=================== ADAPTERS CORE SUITE SUMMARY ==================="
PASS_COUNT=$(grep -h "checks=" "$EV"/oracle-*.log "$EV"/checksum.log \
    "$EV"/fuzz-torture.log "$EV"/alloc-probe.log 2>/dev/null | \
    grep -c "PASS" || true)
FAIL_COUNT=$(grep -h "checks=" "$EV"/oracle-*.log "$EV"/checksum.log \
    "$EV"/fuzz-torture.log "$EV"/alloc-probe.log 2>/dev/null | \
    grep -c "FAIL" || true)
note "oracle/checksum/fuzz/probe test binaries: PASS=$PASS_COUNT FAIL=$FAIL_COUNT"
note "evidence logs: $EV/"
note "==================================================================="
if [ "$GATE_FAIL" != "0" ] || [ "$FAIL_COUNT" != "0" ]; then
    fail "suite failed"
    exit 1
fi
note "ADAPTERS CORE SUITE: PASS"
exit 0
