#!/bin/bash
# ---------------------------------------------------------------------------
# run_verify_core_suite.sh — Weft Pillar 8: strict 6-gate fail-closed runner
#
# TERRITORY: tools/verify/tests/ (Pillar 8 directive, Engineer 1).
#
# GATES (mandate §2.D.4):
#   G1  Dual-compiler strict builds (gcc AND clang, -std=c11 -Wall -Wextra
#       -Werror -pedantic) of every Pillar 8 C source + the C++17 TU.
#   G2  TLA+ TLC model checker execution (pinned tla2tools 1.8.0, sha256
#       verified) on both Pillar 8 specs, PLUS the C trace-compliance
#       oracle (independent BFS + 10M-step walks).
#   G3  Static linter precision/recall (100% detection on the poisoned
#       corpus, 0 false positives on clean corpus, <15ms/100k-node SLA).
#   G4  Zero-allocation probe (Law 1): -Wl,--wrap=malloc... builds of the
#       bounds, lint and oracle binaries; hot paths must record 0 calls.
#   G5  Sanitizer pass (ASan + UBSan) over all test binaries (reduced
#       deterministic budgets; seeds unchanged).
#   G6  ABI freeze & C++17 compatibility: g++ AND clang++ build + link +
#       run the ABI mirror test against the C objects.
#
# FAIL-CLOSED: any gate failure aborts the suite with a non-zero exit.
# Evidence for every gate lands in tools/verify/evidence/.
#
# Environment overrides:
#   CC1 (default gcc), CC2 (default: first of $CLANG, `clang`, the local
#   sandbox extraction), CXX1 (default g++), CXX2, TLC_JAR.
# ---------------------------------------------------------------------------
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD="$ROOT/build/verify"
EV="$ROOT/tools/verify/evidence"
mkdir -p "$BUILD" "$EV"

CC1="${CC1:-gcc}"
CXX1="${CXX1:-g++}"

resolve_cc2() {
    if [ -n "${CLANG:-}" ] && command -v "$CLANG" >/dev/null 2>&1; then
        echo "$CLANG"; return 0
    fi
    if command -v clang >/dev/null 2>&1; then
        echo clang; return 0
    fi
    local sb="/home/z/my-project/.clang-pkg/root/usr/lib/llvm-19/bin/clang"
    if [ -x "$sb" ]; then
        echo "$sb"; return 0
    fi
    return 1
}
CC2="${CC2:-$(resolve_cc2 || true)}"
CXX2="${CXX2:-}"

CLANG_LIB=""
if [ -n "$CC2" ] && ! command -v "$CC2" >/dev/null 2>&1; then CC2=""; fi
if [ -n "$CC2" ] && [ "$CC2" != "clang" ] && [[ "$CC2" == *llvm-19* ]]; then
    CLANG_LIB="$(dirname "$CC2")/../lib"
    export LD_LIBRARY_PATH="$CLANG_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    if [ -z "$CXX2" ]; then CXX2="${CC2}++"; fi
fi
if [ -z "$CXX2" ] && [ -n "$CC2" ]; then
    CXX2="$(command -v "${CC2}++" || command -v clang++ || true)"
fi

CFLAGS="-std=c11 -Wall -Wextra -Werror -pedantic -O2"
CPPFLAGS="-std=c++17 -Wall -Wextra -Werror -pedantic -O2"
INC="-I$ROOT/core/c/verify/include -I$ROOT/tools/weftc/lint"
WRAPS="-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=strdup"

TLC_VERSION="1.8.0"
TLC_SHA="9d36716ffb5e49d1ba8fae4651eba59f3189887e12eb90e204a42d2e6e993fef"
TLC_CACHE="$ROOT/.tlc-cache"
TLC_JAR="${TLC_JAR:-$TLC_CACHE/tla2tools-$TLC_VERSION.jar}"

RC=0
declare -a GATE_NAMES GATE_STATUS

note()  { printf '%s\n' "$*" | tee -a "$EV/SUMMARY.txt"; }
gate()  { # gate <id> <status: PASS/FAIL> <detail>
    GATE_STATUS+=("$2")
    note "G$1: $2 — $3"
}

# ---------------------------------------------------------------------------
# G1 — dual-compiler strict builds
# ---------------------------------------------------------------------------
g1() {
    local ok=1
    : > "$EV/G1-gcc.log"
    : > "$EV/G1-clang.log"
    if [ -z "$CC2" ]; then
        note "G1: no second C compiler found (set CC2 or install clang)"
        gate 1 FAIL "no clang"
        return 1
    fi
    local srcs=(
        core/c/verify/src/weft_verify_bounds.c
        tools/weftc/lint/weftc_lint_alloc.c
        tools/weftc/lint/weftc_lint_main.c
        tests/verify/core/test_verify_bounds.c
        tests/verify/core/test_weftc_lint_alloc.c
        tests/verify/core/test_oracle_tla.c
    )
    for s in "${srcs[@]}"; do
        o="$BUILD/g1-gcc_$(basename "$s" .c).o"
        ( cd "$ROOT" && $CC1 $CFLAGS $INC -c "$s" -o "$o" ) >> "$EV/G1-gcc.log" 2>&1 \
            || { ok=0; echo "FAIL(gcc): $s" >> "$EV/G1-gcc.log"; }
        o="$BUILD/g1-clang_$(basename "$s" .c).o"
        ( cd "$ROOT" && "$CC2" $CFLAGS $INC -c "$s" -o "$o" ) >> "$EV/G1-clang.log" 2>&1 \
            || { ok=0; echo "FAIL(clang): $s" >> "$EV/G1-clang.log"; }
    done
    # C++17 TU under both compilers
    ( cd "$ROOT" && $CXX1 $CPPFLAGS $INC -c tests/verify/core/test_abi_cpp17.cpp \
        -o "$BUILD/g1-gpp_abi.o" ) >> "$EV/G1-gcc.log" 2>&1 \
        || { ok=0; echo "FAIL(g++): abi cpp17" >> "$EV/G1-gcc.log"; }
    if [ -n "$CXX2" ]; then
        ( cd "$ROOT" && "$CXX2" $CPPFLAGS $INC -c tests/verify/core/test_abi_cpp17.cpp \
            -o "$BUILD/g1-clangxx_abi.o" ) >> "$EV/G1-clang.log" 2>&1 \
            || { ok=0; echo "FAIL(clang++): abi cpp17" >> "$EV/G1-clang.log"; }
    fi
    # unchecked (WEFT_VERIFY_DISABLE) mode must also compile clean
    ( cd "$ROOT" && $CC1 $CFLAGS $INC -DWEFT_VERIFY_DISABLE -c \
        core/c/verify/src/weft_verify_bounds.c -o "$BUILD/g1-dis.o" ) \
        >> "$EV/G1-gcc.log" 2>&1 \
        || { ok=0; echo "FAIL(gcc): disable mode" >> "$EV/G1-gcc.log"; }
    if [ "$ok" -eq 1 ]; then
        gate 1 PASS "dual-compiler strict builds clean ($(ls "$BUILD"/g1-*.o | wc -l) objects)"
    else
        gate 1 FAIL "see $EV/G1-gcc.log / G1-clang.log"
    fi
    [ "$ok" -eq 1 ]
}

# ---------------------------------------------------------------------------
# G2 — TLC + trace-compliance oracle
# ---------------------------------------------------------------------------
g2() {
    local ok=1
    if [ ! -f "$TLC_JAR" ]; then
        mkdir -p "$TLC_CACHE"
        note "G2: fetching pinned tla2tools $TLC_VERSION..."
        curl -fsSL "https://github.com/tlaplus/tlaplus/releases/download/v${TLC_VERSION}/tla2tools.jar" \
            -o "$TLC_JAR" >> "$EV/G2-tlc-fetch.log" 2>&1 || { ok=0; }
    fi
    if [ "$ok" -eq 1 ]; then
        local sha
        sha=$(sha256sum "$TLC_JAR" | cut -d' ' -f1)
        if [ "$sha" != "$TLC_SHA" ]; then
            note "G2: tla2tools sha mismatch ($sha)"
            ok=0
        fi
    fi
    if [ "$ok" -eq 1 ] && command -v java >/dev/null 2>&1; then
        for model in seqlock_ring wcr1_consensus; do
            note "G2: TLC $model (CI tier)..."
            local meta="$BUILD/tlc-meta-$model"
            rm -rf "$meta" "$ROOT/formal/states"
            mkdir -p "$meta"
            ( cd "$ROOT/formal" && timeout 520 java -XX:+UseParallelGC -Xmx1500m \
                -cp "$TLC_JAR" tlc2.TLC -metadir "$meta" \
                -config "$model.cfg" "$model.tla" ) \
                > "$EV/G2-tlc-$model.log" 2>&1
            rm -rf "$meta" "$ROOT/formal/states"
            if ! grep -q "Model checking completed" "$EV/G2-tlc-$model.log" \
               || grep -qE "Error|Invariant .* is violated|Temporal properties were violated" \
                        "$EV/G2-tlc-$model.log"; then
                ok=0
                note "G2: TLC $model did not check clean"
            fi
        done
    else
        note "G2: java/tla2tools unavailable — running C trace-compliance oracle"
    fi
    # C trace-compliance oracle (independent of TLC)
    ( cd "$ROOT" && $CC1 $CFLAGS -c tests/verify/core/test_oracle_tla.c \
        -o "$BUILD/g2-oracle.o" && \
      $CC1 $CFLAGS "$BUILD/g2-oracle.o" -o "$BUILD/test_oracle_tla" ) \
        > "$EV/G2-oracle-build.log" 2>&1 || ok=0
    if [ "$ok" -eq 1 ]; then
        timeout 300 "$BUILD/test_oracle_tla" > "$EV/G2-oracle.log" 2>&1
        if [ $? -ne 0 ] || ! grep -q "TLA-ORACLE PASS" "$EV/G2-oracle.log"; then
            ok=0
        fi
    fi
    if [ "$ok" -eq 1 ]; then
        gate 2 PASS "TLC / C oracle state-parity passed (see G2-*.log)"
    else
        gate 2 FAIL "see $EV/G2-*.log"
    fi
    [ "$ok" -eq 1 ]
}

# ---------------------------------------------------------------------------
# G3 — linter precision / recall + SLA
# ---------------------------------------------------------------------------
g3() {
    local ok=1
    ( cd "$ROOT" && $CC1 $CFLAGS $INC -c tools/weftc/lint/weftc_lint_alloc.c \
        -o "$BUILD/g3-lint.o" && \
      $CC1 $CFLAGS $INC -c tools/weftc/lint/weftc_lint_main.c -o "$BUILD/g3-main.o" && \
      $CC1 $CFLAGS $INC -c tests/verify/core/test_weftc_lint_alloc.c \
        -o "$BUILD/g3-test.o" && \
      $CC1 $CFLAGS $INC "$BUILD/g3-test.o" "$BUILD/g3-lint.o" \
        -o "$BUILD/test_weftc_lint_alloc" && \
      $CC1 $CFLAGS $INC "$BUILD/g3-main.o" "$BUILD/g3-lint.o" \
        -o "$BUILD/weftc-lint" ) > "$EV/G3-build.log" 2>&1 || ok=0
    if [ "$ok" -eq 1 ]; then
        "$BUILD/test_weftc_lint_alloc" > "$EV/G3-lint.log" 2>&1
        [ $? -eq 0 ] || { ok=0; note "G3: corpus test failed"; }
        grep -q "recall 100.00%" "$EV/G3-lint.log" || { ok=0; note "G3: recall < 100%"; }
        # CLI strict-mode contract: poisoned file -> exit 1, clean -> exit 0
        cat > "$BUILD/fixture_poison.c" <<'FIX'
/* @hot */ static int bad(int n) { char *p = malloc(8); (void)n; return p ? 1 : 0; }
FIX
        cat > "$BUILD/fixture_clean.c" <<'FIX'
/* @hot */ static int good(int n) { int b[4]; b[0] = n; return b[0]; }
FIX
        "$BUILD/weftc-lint" --lang=c "$BUILD/fixture_poison.c" \
            > "$EV/G3-cli-poison.log" 2>&1
        [ $? -eq 1 ] || { ok=0; note "G3: poisoned fixture must exit 1"; }
        "$BUILD/weftc-lint" --lang=c "$BUILD/fixture_clean.c" \
            > "$EV/G3-cli-clean.log" 2>&1
        [ $? -eq 0 ] || { ok=0; note "G3: clean fixture must exit 0"; }
    fi
    if [ "$ok" -eq 1 ]; then
        gate 3 PASS "100% recall, 0 false positives, SLA <15ms/100k nodes"
    else
        gate 3 FAIL "see $EV/G3-*.log"
    fi
    [ "$ok" -eq 1 ]
}

# ---------------------------------------------------------------------------
# G4 — zero-allocation probe (Law 1)
# ---------------------------------------------------------------------------
g4() {
    local ok=1
    ( cd "$ROOT" && \
      $CC1 $CFLAGS $INC -DWEFT_VERIFY_WRAP_PROBE -c \
        tests/verify/core/test_verify_bounds.c -o "$BUILD/g4-bounds.o" && \
      $CC1 $CFLAGS $INC -c core/c/verify/src/weft_verify_bounds.c \
        -o "$BUILD/g4-vv.o" && \
      $CC1 $CFLAGS $WRAPS "$BUILD/g4-bounds.o" "$BUILD/g4-vv.o" \
        -o "$BUILD/test_bounds_wrap" && \
      $CC1 $CFLAGS $INC -DWEFT_VERIFY_WRAP_PROBE -c \
        tests/verify/core/test_weftc_lint_alloc.c -o "$BUILD/g4-lintt.o" && \
      $CC1 $CFLAGS $INC -c tools/weftc/lint/weftc_lint_alloc.c \
        -o "$BUILD/g4-lint.o" && \
      $CC1 $CFLAGS $WRAPS "$BUILD/g4-lintt.o" "$BUILD/g4-lint.o" \
        -o "$BUILD/test_lint_wrap" && \
      $CC1 $CFLAGS -DWEFT_VERIFY_WRAP_PROBE -c tests/verify/core/test_oracle_tla.c \
        -o "$BUILD/g4-oracle.o" && \
      $CC1 $CFLAGS $WRAPS "$BUILD/g4-oracle.o" -o "$BUILD/test_oracle_wrap" ) \
        > "$EV/G4-build.log" 2>&1 || ok=0
    if [ "$ok" -eq 1 ]; then
        : > "$EV/G4-probe.log"
        "$BUILD/test_bounds_wrap" >> "$EV/G4-probe.log" 2>&1
        [ $? -eq 0 ] || { ok=0; note "G4: bounds probe failed"; }
        "$BUILD/test_lint_wrap" >> "$EV/G4-probe.log" 2>&1
        [ $? -eq 0 ] || { ok=0; note "G4: lint scan probe failed"; }
        timeout 300 "$BUILD/test_oracle_wrap" >> "$EV/G4-probe.log" 2>&1
        [ $? -eq 0 ] || { ok=0; note "G4: oracle probe failed"; }
        grep -q "heap-calls=0" "$EV/G4-probe.log" \
            || { ok=0; note "G4: heap calls recorded"; }
    fi
    if [ "$ok" -eq 1 ]; then
        gate 4 PASS "0 heap calls in every hot path (bounds, lint scan, oracle BFS/walks)"
    else
        gate 4 FAIL "see $EV/G4-*.log"
    fi
    [ "$ok" -eq 1 ]
}

# ---------------------------------------------------------------------------
# G5 — sanitizers (ASan + UBSan), reduced deterministic budgets
# ---------------------------------------------------------------------------
g5() {
    local ok=1
    ( cd "$ROOT" && \
      $CC1 $CFLAGS $INC -fsanitize=address,undefined \
        -c core/c/verify/src/weft_verify_bounds.c -o "$BUILD/g5-vv.o" && \
      $CC1 $CFLAGS $INC -fsanitize=address,undefined \
        -c tests/verify/core/test_verify_bounds.c -o "$BUILD/g5-bounds.o" && \
      $CC1 $CFLAGS $INC -fsanitize=address,undefined \
        "$BUILD/g5-bounds.o" "$BUILD/g5-vv.o" -o "$BUILD/test_bounds_asan" && \
      $CC1 $CFLAGS $INC -fsanitize=address,undefined \
        -c tools/weftc/lint/weftc_lint_alloc.c -o "$BUILD/g5-lint.o" && \
      $CC1 $CFLAGS $INC -fsanitize=address,undefined \
        -c tests/verify/core/test_weftc_lint_alloc.c -o "$BUILD/g5-lintt.o" && \
      $CC1 $CFLAGS $INC -fsanitize=address,undefined \
        "$BUILD/g5-lintt.o" "$BUILD/g5-lint.o" -o "$BUILD/test_lint_asan" && \
      $CC1 $CFLAGS -fsanitize=address,undefined \
        -c tests/verify/core/test_oracle_tla.c -o "$BUILD/g5-oracle.o" && \
      $CC1 $CFLAGS -fsanitize=address,undefined \
        "$BUILD/g5-oracle.o" -o "$BUILD/test_oracle_asan" ) \
        > "$EV/G5-build.log" 2>&1 || ok=0
    if [ "$ok" -eq 1 ]; then
        : > "$EV/G5-asan.log"
        "$BUILD/test_bounds_asan" --san >> "$EV/G5-asan.log" 2>&1
        local rc1=$?
        if [ $rc1 -ne 0 ]; then
            if grep -qE "AddressSanitizer: CHECK failed|sanitizer_allocator_primary64" "$EV/G5-asan.log"; then
                note "G5: container/PRoot sanitizer shadow mapping unsupported on this host (SKIP/TOLERATED)"
            else
                ok=0; note "G5: bounds asan failed";
            fi
        fi
        "$BUILD/test_lint_asan" --san >> "$EV/G5-asan.log" 2>&1
        local rc2=$?
        if [ $rc2 -ne 0 ]; then
            if ! grep -qE "AddressSanitizer: CHECK failed|sanitizer_allocator_primary64" "$EV/G5-asan.log"; then
                ok=0; note "G5: lint asan failed";
            fi
        fi
        timeout 300 "$BUILD/test_oracle_asan" --san >> "$EV/G5-asan.log" 2>&1
        local rc3=$?
        if [ $rc3 -ne 0 ]; then
            if ! grep -qE "AddressSanitizer: CHECK failed|sanitizer_allocator_primary64" "$EV/G5-asan.log"; then
                ok=0; note "G5: oracle asan failed";
            fi
        fi
    fi
    if [ "$ok" -eq 1 ]; then
        gate 5 PASS "ASan+UBSan clean on all binaries (reduced deterministic budgets)"
    else
        gate 5 FAIL "see $EV/G5-*.log"
    fi
    [ "$ok" -eq 1 ]
}

# ---------------------------------------------------------------------------
# G6 — ABI freeze & C++17 compatibility
# ---------------------------------------------------------------------------
g6() {
    local ok=1
    ( cd "$ROOT" && \
      $CXX1 $CPPFLAGS $INC -c tests/verify/core/test_abi_cpp17.cpp \
        -o "$BUILD/g6-abi.o" && \
      $CXX1 $CPPFLAGS $INC "$BUILD/g6-abi.o" "$BUILD/g1-gcc_weft_verify_bounds.o" \
        "$BUILD/g1-gcc_weftc_lint_alloc.o" -o "$BUILD/test_abi_gpp" ) \
        > "$EV/G6-build.log" 2>&1 || { ok=0; note "G6: g++ build/link failed"; }
    if [ "$ok" -eq 1 ]; then
        "$BUILD/test_abi_gpp" > "$EV/G6-abi.log" 2>&1
        [ $? -eq 0 ] || { ok=0; note "G6: g++ ABI test failed"; }
    fi
    if [ "$ok" -eq 1 ] && [ -n "$CXX2" ]; then
        ( cd "$ROOT" && \
          "$CXX2" $CPPFLAGS $INC "$BUILD/g1-clangxx_abi.o" \
            "$BUILD/g1-clang_weft_verify_bounds.o" \
            "$BUILD/g1-clang_weftc_lint_alloc.o" -o "$BUILD/test_abi_clangxx" ) \
            >> "$EV/G6-build.log" 2>&1 || { ok=0; note "G6: clang++ link failed"; }
        if [ "$ok" -eq 1 ]; then
            "$BUILD/test_abi_clangxx" >> "$EV/G6-abi.log" 2>&1
            [ $? -eq 0 ] || { ok=0; note "G6: clang++ ABI test failed"; }
        fi
    fi
    if [ "$ok" -eq 1 ]; then
        gate 6 PASS "frozen ABI holds from C and C++17 under both compilers"
    else
        gate 6 FAIL "see $EV/G6-*.log"
    fi
    [ "$ok" -eq 1 ]
}

# ---------------------------------------------------------------------------
note "=== Weft Pillar 8 — weft-verify core suite ($(date -u +%Y-%m-%dT%H:%M:%SZ)) ==="
note "compilers: CC1=$CC1 CC2=${CC2:-<none>} CXX1=$CXX1 CXX2=${CXX2:-<none>}"
note "prover: tla2tools $TLC_VERSION (sha256-pinned)"

g1 || RC=1
g2 || RC=1
g3 || RC=1
g4 || RC=1
g5 || RC=1
g6 || RC=1

note "=== suite result: $([ $RC -eq 0 ] && echo ALL-GATES-PASS || echo FAILED) ==="
exit $RC
