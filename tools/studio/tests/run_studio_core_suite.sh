#!/usr/bin/env bash
# tools/studio/tests/run_studio_core_suite.sh — Weft Studio core gate battery.
#
# G1  dual-compiler strict build: GCC and Clang, -std=c11 -Wall -Wextra
#     -Werror -pedantic, engines + tests + tools, plus the
#     WEFT_STUDIO_WASM_PORTABLE profile (software CRC only, no x86
#     intrinsics — the wasm32 compilation surface) and the kernel-freeze
#     check (frozen core files untouched).
# G2  oracle suites: compiler golden parity (Pillar-1 weftc), LSP
#     JSON-RPC goldens, .weftrec playback oracles.
# G3  10,000,000-cycle fuzz (schema + corrupted traces), fixed seeds.
# G4  allocation probe: malloc interposition, 0 events over 200,000
#     steady-state cycles (Law 1).
# G5  sanitizers: ASan + UBSan legs of the oracles and a declared
#     reduced fuzz leg (the interposition probe is plain-only: ASan's
#     own malloc interceptors preempt interposition — the mallinfo2
#     delta is the sanitizer-leg allocation evidence, run inside the
#     ASan oracle binaries).
# G6  ABI freeze + C++17 gate: the frozen header compiled from C++17
#     with all _Static_asserts firing; LSP stdio server transcript is
#     byte-identical to its golden; weftrec-inspect round-trip.
#
# Fail-closed: any gate failure aborts with a non-zero exit.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
STUDIO="$ROOT/tools/studio"
SRC="$ROOT/core/c/studio/src"
INC="$ROOT/core/c/include"
TESTS="$ROOT/tests/studio/core"
EVID="$STUDIO/evidence"
CC="${CC:-gcc}"
CLANG="${CLANG:-}"
if [ -z "$CLANG" ]; then
    for c in clang clang-19 clang-21 /home/z/my-project/scripts/clang-root/usr/lib/llvm-19/bin/clang; do
        if command -v "$c" >/dev/null 2>&1; then CLANG="$c"; break; fi
    done
fi

STRICT="-std=c11 -Wall -Wextra -Werror -pedantic"
mkdir -p "$EVID" /tmp/p7build

pass() { printf 'GATE %-4s PASS  %s\n' "$1" "$2"; }
fail() { printf 'GATE %-4s FAIL  %s\n' "$1" "$2" >&2; exit 1; }

echo "== Weft Studio core suite (Pillar 7) =="
echo "gcc:   $($CC --version | head -1)"
echo "clang: ${CLANG:-none} $($CLANG --version 2>/dev/null | head -1 || true)"
echo

# ---------------------------------------------------------------- G1
echo "-- G1: dual-compiler strict builds"
UNITS="weftc_inmem weft_lsp weftrec_engine"
for u in $UNITS; do
    $CC $STRICT -I"$INC" -c "$SRC/$u.c" -o /tmp/p7build/$u.gcc.o \
        2> "$EVID/g1-$u-gcc.log" || fail G1 "gcc $u"
    if [ -n "$CLANG" ]; then
        $CLANG $STRICT -I"$INC" -c "$SRC/$u.c" -o /tmp/p7build/$u.clang.o \
            2> "$EVID/g1-$u-clang.log" || fail G1 "clang $u"
    fi
done
# wasm-portable profile (software CRC, no target attributes)
$CC $STRICT -DWEFT_STUDIO_WASM_PORTABLE -I"$INC" \
    -c "$SRC/weftrec_engine.c" -o /tmp/p7build/weftrec_engine.wasm.o \
    2> "$EVID/g1-weftrec-wasm.log" || fail G1 "wasm-portable profile"
# test binaries under clang too (the dual-compiler claim covers the
# full delivered surface, tests included)
if [ -n "$CLANG" ]; then
    for t in test_weftc_inmem test_oracle_lsp test_oracle_weftrec \
             test_fuzz_studio; do
        $CLANG $STRICT -I"$INC" -I"$SRC" -I"$TESTS" "$TESTS/$t.c" \
            "$SRC"/*.c -o /tmp/p7build/clang_$t 2>> "$EVID/g1-tests-clang.log" \
            || fail G1 "clang test $t"
    done
fi
# tools
for t in weft_lsp_server weftrec_inspect; do
    $CC $STRICT -O2 -I"$INC" -I"$SRC" "$STUDIO/$t.c" \
        "$SRC/weftc_inmem.c" "$SRC/weft_lsp.c" "$SRC/weftrec_engine.c" \
        -ldl -o /tmp/p7build/$t 2> "$EVID/g1-$t.log" || fail G1 "tool $t"
done
# kernel freeze (frozen core untouched)
if (cd "$ROOT" && git rev-parse --is-inside-work-tree >/dev/null 2>&1); then
    if (cd "$ROOT" && git diff --quiet -- core/c/weft.c core/c/weft.h \
        core/c/src 2>/dev/null); then
        pass G1 "kernel freeze (weft.c/weft.h untouched)"
    else
        fail G1 "kernel freeze: frozen core files modified"
    fi
fi
pass G1 "strict builds: gcc$([ -n "$CLANG" ] && echo " + clang"), wasm profile, tools"

# ---------------------------------------------------------------- G2
echo "-- G2: oracle suites"
$CC $STRICT -O2 -I"$INC" -I"$SRC" -I"$TESTS" "$TESTS/test_weftc_inmem.c" \
    "$SRC/weftc_inmem.c" -o /tmp/p7build/t_inmem 2> "$EVID/g2-inmem.log" \
    || fail G2 "build inmem"
/tmp/p7build/t_inmem 2>&1 | tee "$EVID/g2-inmem.log" | tail -2 \
    || fail G2 "C-series (weftc parity)"
$CC $STRICT -O2 -I"$INC" -I"$SRC" -I"$TESTS" "$TESTS/test_oracle_lsp.c" \
    "$SRC/weftc_inmem.c" "$SRC/weft_lsp.c" -o /tmp/p7build/t_lsp \
    2> "$EVID/g2-lsp.log" || fail G2 "build lsp"
/tmp/p7build/t_lsp 2>&1 | tee "$EVID/g2-lsp.log" | tail -1 \
    || fail G2 "L-series (LSP goldens)"
$CC $STRICT -O2 -I"$INC" -I"$SRC" -I"$TESTS" "$TESTS/test_oracle_weftrec.c" \
    "$SRC/weftc_inmem.c" "$SRC/weftrec_engine.c" -o /tmp/p7build/t_rec \
    2> "$EVID/g2-rec.log" || fail G2 "build weftrec"
/tmp/p7build/t_rec 2>&1 | tee "$EVID/g2-rec.log" | tail -6 \
    || fail G2 "R-series (weftrec oracles + SLA gates)"
pass G2 "compiler parity + LSP goldens + weftrec oracles + SLA gates"

# ---------------------------------------------------------------- G3
echo "-- G3: 10M-cycle fuzz (fixed seeds)"
$CC $STRICT -O2 -I"$INC" -I"$SRC" -I"$TESTS" "$TESTS/test_fuzz_studio.c" \
    "$SRC/weftc_inmem.c" "$SRC/weftrec_engine.c" -o /tmp/p7build/t_fuzz \
    2> "$EVID/g3-fuzz.log" || fail G3 "build fuzz"
/tmp/p7build/t_fuzz 2>&1 | tee "$EVID/g3-fuzz.log" | tail -6 \
    || fail G3 "10M-cycle fuzz"
pass G3 "10,000,000 cycles, 0 crashes, deterministic"

# ---------------------------------------------------------------- G4
echo "-- G4: allocation probe (Law 1)"
$CC $STRICT -O2 -I"$INC" -I"$SRC" -I"$TESTS" "$TESTS/test_alloc_probe.c" \
    "$SRC/weftc_inmem.c" "$SRC/weft_lsp.c" "$SRC/weftrec_engine.c" -ldl \
    -o /tmp/p7build/t_alloc 2> "$EVID/g4-alloc.log" || fail G4 "build probe"
/tmp/p7build/t_alloc 2>&1 | tee "$EVID/g4-alloc.log" | tail -5 \
    || fail G4 "interposition probe"
pass G4 "0 allocation events over 200,000 cycles"

# ---------------------------------------------------------------- G5
echo "-- G5: sanitizers (ASan + UBSan)"
ASAN="-std=c11 -Wall -Wextra -Werror -pedantic -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all"
$CC $ASAN -I"$INC" -I"$SRC" -I"$TESTS" "$TESTS/test_weftc_inmem.c" \
    "$SRC/weftc_inmem.c" -o /tmp/p7build/t_inmem_asan 2> "$EVID/g5-inmem.log" \
    || fail G5 "build inmem asan"
/tmp/p7build/t_inmem_asan > "$EVID/g5-inmem.log" 2>&1 \
    || fail G5 "C-series ASan+UBSan"
$CC $ASAN -I"$INC" -I"$SRC" -I"$TESTS" "$TESTS/test_oracle_lsp.c" \
    "$SRC/weftc_inmem.c" "$SRC/weft_lsp.c" -o /tmp/p7build/t_lsp_asan \
    2> "$EVID/g5-lsp.log" || fail G5 "build lsp asan"
/tmp/p7build/t_lsp_asan > "$EVID/g5-lsp.log" 2>&1 || fail G5 "L-series ASan+UBSan"
$CC $ASAN -I"$INC" -I"$SRC" -I"$TESTS" "$TESTS/test_oracle_weftrec.c" \
    "$SRC/weftc_inmem.c" "$SRC/weftrec_engine.c" -o /tmp/p7build/t_rec_asan \
    2> "$EVID/g5-rec.log" || fail G5 "build weftrec asan"
# SLA gates are production-build contracts; sanitizer instrumentation
# roughly doubles latency, so the ASan leg checks correctness only.
STUDIO_SKIP_SLA=1 /tmp/p7build/t_rec_asan > "$EVID/g5-rec.log" 2>&1 \
    || fail G5 "R-series ASan+UBSan"
$CC $ASAN -I"$INC" -I"$SRC" -I"$TESTS" "$TESTS/test_fuzz_studio.c" \
    "$SRC/weftc_inmem.c" "$SRC/weftrec_engine.c" -o /tmp/p7build/t_fuzz_asan \
    2> "$EVID/g5-fuzz.log" || fail G5 "build fuzz asan"
/tmp/p7build/t_fuzz_asan --reduced > "$EVID/g5-fuzz.log" 2>&1 \
    || fail G5 "reduced fuzz leg ASan+UBSan (declared reduction)"
pass G5 "ASan+UBSan clean (oracles + 2.5M-cycle reduced fuzz)"

# ---------------------------------------------------------------- G6
echo "-- G6: ABI freeze, C++17 gate, stdio transcript"
# C++17 inclusion with the offset asserts firing
cat > /tmp/p7build/abi_cpp.cpp <<'EOC'
#include "weft_studio.h"
/* Law 4: the frozen header must be C++17-include-safe and pin every
 * exported structure; instantiating the types proves completeness. */
static weftrec_header_t h;
static weftrec_index_entry_t ie;
static weftrec_frame_header_t fh;
static weft_ast_node_t an;
static weft_struct_layout_t sl;
static weft_field_layout_t fl;
static weft_diag_t dg;
int touch_abi(void)
{
    h.magic = WEFTREC1_MAGIC;
    ie.frame_offset = 64;
    fh.timestamp_ns = 1;
    an.kind = WEFT_AST_SCHEMA;
    sl.size = fl.offset + fl.size;
    dg.code = WEFT_D_CACHE_LINE_CROSS;
    return (int)(sizeof(weftrec_header_t) + sizeof(h) + sizeof(ie) +
                 sizeof(fh) + sizeof(an) + sizeof(sl) + sizeof(fl) +
                 sizeof(dg) + (int)WEFT_STUDIO_ABI_VERSION);
}
EOC
CXX="${CXX:-g++}"
$CXX -std=c++17 -Wall -Wextra -Werror -pedantic -I"$INC" \
    -c /tmp/p7build/abi_cpp.cpp -o /tmp/p7build/abi_cpp.o \
    2> "$EVID/g6-cpp17.log" || fail G6 "C++17 inclusion gate"
# LSP stdio transcript vs golden
/tmp/p7build/weft_lsp_server < "$STUDIO/tests/tools/fixture.lsp" \
    > /tmp/p7build/lsp_transcript.txt 2> "$EVID/g6-lsp.log" \
    || fail G6 "weft-lsp-server exit"
cmp -s /tmp/p7build/lsp_transcript.txt "$STUDIO/tests/tools/fixture.lsp.golden" \
    || fail G6 "LSP stdio transcript drift"
# weftrec-inspect round trip: build a trace with the R-series binary's
# methodology (small C helper), then inspect it
cat > /tmp/p7build/mk_trace.c <<'EOC'
#include "weft_studio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    uint8_t *buf = aligned_alloc(64, 1 << 20);
    weftrec_index_entry_t *idx = aligned_alloc(8, 64 * 64);
    weftrec_builder_t b;
    uint64_t len = 0;
    uint8_t pl[48];
    int i;
    for (i = 0; i < 48; i++) pl[i] = (uint8_t)(i * 3);
    weftrec_builder_init(&b, buf, 1 << 20, idx, 64);
    for (i = 0; i < 12; i++)
        weftrec_builder_append(&b, 1000 + i * 250, (uint32_t)(i % 2), pl,
                               48, (i % 2) ? WEFTREC_CODEC_DZV
                                          : WEFTREC_CODEC_RAW);
    weftrec_builder_finish(&b, &len);
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        fwrite(buf, 1, (size_t)len, f);
        fclose(f);
    }
    printf("%llu\n", (unsigned long long)len);
    return 0;
}
EOC
$CC $STRICT -O2 -I"$INC" /tmp/p7build/mk_trace.c "$SRC/weftrec_engine.c" \
    -o /tmp/p7build/mk_trace 2>> "$EVID/g6-inspect.log" || fail G6 "mk_trace build"
/tmp/p7build/mk_trace /tmp/p7build/roundtrip.weftrec > /dev/null \
    || fail G6 "trace build"
/tmp/p7build/weftrec_inspect /tmp/p7build/roundtrip.weftrec \
    > "$EVID/g6-inspect.log" 2>&1 || fail G6 "weftrec-inspect exit"
grep -q "walked 12 frames;" "$EVID/g6-inspect.log" \
    || fail G6 "inspect: 12-frame walk"
pass G6 "ABI asserts + C++17 + stdio transcript byte-exact + inspect round-trip"

echo
echo "ALL GATES GREEN: G1 G2 G3 G4 G5 G6"
echo "evidence: $EVID/"
exit 0
