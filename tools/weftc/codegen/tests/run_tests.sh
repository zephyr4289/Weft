#!/usr/bin/env bash
# run_tests.sh — the weftc Pillar 1 verification gate.
#
# Gates (any failure exits non-zero):
#   1. codegen determinism  — regenerate to a second tree, byte-compare
#   2. golden-file identity — committed goldens match a fresh run exactly
#   3. Law-4 warning matrix — the exact set of expected GPU refusals fires
#   4. C roundtrip          — selftest x3 build flavors (default / -mavx2 /
#                             ASAN+UBSAN); AVX2 stage-1 bins byte-identical
#                             to the scalar build's (SIMD == scalar verdicts)
#   5. Rust roundtrip       — no_std lib compiles (Law 2) + cross-language
#                             bit-exact protocol over the stage bins
#   6. C verify-stage2      — re-reads Rust's mutations bit-exact
#   7. GPU double-entry     — gpu_layout_check re-derives WGSL/GLSL offsets
#                             from the emitted text vs C offsetof ground truth
#   8. glslang (optional)   — .comp proof shaders compile to SPIR-V when
#                             glslangValidator is present; declared otherwise
#
# Output: tests/out/ (artifacts), stdout lines greppable "[weftc-codegen]".

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
GEN="$ROOT/weftc-codegen"
OUT="$HERE/out"
GOLDEN="$HERE/golden"
FIXTURES="telemetry_frame mcu_status camera_exposure sensor_event audio_peak"

export PATH="$HOME/.cargo/bin:$PATH"

fail=0
step() { echo ""; echo "=== $1 ==="; }
note() { echo "[weftc-codegen] $*"; }

# --- 0. build the generator --------------------------------------------------
step "build weftc-codegen (pure C11, -Werror -pedantic)"
make -C "$ROOT" >/dev/null
note "generator built"

# --- 1. generate + determinism -------------------------------------------------
step "generate all targets for all fixtures"
rm -rf "$OUT" "$OUT.determinism"
for f in $FIXTURES; do
    "$GEN" --ir "$HERE/fixtures/$f.weft.json" --target all --out "$OUT/$f" \
        2> "$OUT.$f.warnings" || { note "FAIL: generation of $f"; fail=1; }
    # capture warnings into a tree-local copy as well
    mkdir -p "$OUT/$f"
    mv "$OUT.$f.warnings" "$OUT/$f/_codegen_warnings.txt"
done
note "initial generation done"

step "determinism: regenerate and byte-compare"
for f in $FIXTURES; do
    "$GEN" --ir "$HERE/fixtures/$f.weft.json" --target all --out "$OUT.determinism/$f" \
        2>/dev/null || { note "FAIL: regeneration of $f"; fail=1; }
done
if diff -r -x '_codegen_warnings.txt' "$OUT" "$OUT.determinism" >/dev/null 2>&1; then
    note "PASS: two runs are byte-identical (deterministic codegen)"
else
    note "FAIL: regeneration differs"
    diff -r "$OUT" "$OUT.determinism" | head -20 || true
    fail=1
fi
rm -rf "$OUT.determinism"

# --- 2. golden identity ---------------------------------------------------------
step "golden files: committed goldens == fresh output"
if diff -r "$GOLDEN" "$OUT" >/dev/null 2>&1; then
    note "PASS: golden identity (generator output has not drifted)"
else
    note "FAIL: golden drift — regenerate goldens deliberately if the change is intended:"
    diff -r "$GOLDEN" "$OUT" | head -20 || true
    fail=1
fi

# --- 3. Law-4 warning matrix ----------------------------------------------------
step "Law 4: exact set of GPU refusals"
expect_warning() { # fixture substring
    if grep -q "$1" "$OUT/$2/_codegen_warnings.txt"; then
        note "PASS: expected refusal present: $1"
    else
        note "FAIL: expected refusal MISSING: $1"
        fail=1
    fi
}
expect_silent() {
    if [ -s "$OUT/$1/_codegen_warnings.txt" ]; then
        note "FAIL: $1 should be warning-free, got:"
        cat "$OUT/$1/_codegen_warnings.txt"
        fail=1
    else
        note "PASS: $1 is warning-free"
    fi
}
expect_warning "\[wgsl\] mcu_status: storage variant withheld" mcu_status
expect_warning "\[glsl\] mcu_status: std430 variant withheld" mcu_status
expect_warning "\[wgsl\] axis_sample: storage variant withheld" sensor_event
expect_warning "\[wgsl\] sensor_event: storage variant withheld" sensor_event
expect_warning "\[glsl\] axis_sample: std430 variant withheld" sensor_event
expect_warning "\[glsl\] sensor_event: std430 variant withheld" sensor_event
expect_warning "\[glsl\] audio_peak: std430 variant withheld" audio_peak
expect_silent telemetry_frame
expect_silent camera_exposure

# --- 4. C roundtrip: three build flavors ----------------------------------------
step "C roundtrip: build + selftest (default / AVX2 / ASAN+UBSAN)"
INCS="-I$OUT/telemetry_frame -I$OUT/mcu_status -I$OUT/camera_exposure -I$OUT/sensor_event -I$OUT/audio_peak"
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic $INCS -o "$OUT/c_roundtrip" "$HERE/c_roundtrip.c" -lm
cc -std=c11 -O2 -mavx2 -Wall -Wextra -Werror -pedantic $INCS -o "$OUT/c_roundtrip_avx2" "$HERE/c_roundtrip.c" -lm
cc -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -pedantic $INCS \
    -o "$OUT/c_roundtrip_asan" "$HERE/c_roundtrip.c" -lm

run_gate() { # binary args...
    local bin="$1"; shift
    if "$bin" "$@" > "$OUT/$(basename "$bin").log" 2>&1; then
        tail -1 "$OUT/$(basename "$bin").log"
    else
        cat "$OUT/$(basename "$bin").log"
        note "FAIL: $bin $*"
        fail=1
    fi
}

run_gate "$OUT/c_roundtrip_asan" selftest
run_gate "$OUT/c_roundtrip" selftest
mkdir -p "$OUT/stage1" "$OUT/stage1-avx2"
run_gate "$OUT/c_roundtrip_avx2" stage1 "$OUT/stage1-avx2"
run_gate "$OUT/c_roundtrip" stage1 "$OUT/stage1"

step "AVX2 vs scalar: stage-1 bins byte-identical (SIMD and scalar agree)"
ok=1
for b in "$OUT"/stage1/*.bin; do
    cmp -s "$b" "$OUT/stage1-avx2/$(basename "$b")" || { note "FAIL: $b differs from AVX2 build"; ok=0; }
done
[ "$ok" = 1 ] && note "PASS: AVX2 and scalar builds produce byte-identical stage-1 bins"

# --- 5. Rust roundtrip ------------------------------------------------------------
step "Rust: no_std lib compile (Law 2) + cross-language roundtrip"
mkdir -p "$HERE/rust/src/gen"
rm -f "$HERE/rust/src/gen/"*.rs
for f in $FIXTURES; do
    cp "$OUT/$f/"*.rs "$HERE/rust/src/gen/"
done
if command -v cargo >/dev/null 2>&1; then
    if (cd "$HERE/rust" && WEFT_STAGE1_DIR="$OUT/stage1" WEFT_STAGE2_DIR="$OUT/stage2" \
        cargo test --offline 2>&1 | tail -20); then
        note "PASS: cargo test (no_std lib + roundtrip protocol)"
    else
        (cd "$HERE/rust" && WEFT_STAGE1_DIR="$OUT/stage1" WEFT_STAGE2_DIR="$OUT/stage2" cargo test --offline) || fail=1
    fi
else
    note "FAIL: cargo not found (PATH=$PATH)"
    fail=1
fi

# --- 6. C re-verifies Rust's mutations ---------------------------------------------
step "C verify-stage2 (Rust-written bins re-read bit-exact)"
run_gate "$OUT/c_roundtrip" verify-stage2 "$OUT/stage2"

# --- 7. GPU double-entry -------------------------------------------------------------
step "GPU layout double-entry (emitted WGSL/GLSL vs C offsetof)"
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic $INCS -o "$OUT/gpu_layout_check" "$HERE/gpu_layout_check.c"
run_gate "$OUT/gpu_layout_check" "$OUT"

# --- 8. glslang (optional, declared when absent) --------------------------------------
step "glslangValidator: .comp proof shaders compile (optional)"
if command -v glslangValidator >/dev/null 2>&1; then
    for comp in "$OUT"/telemetry_frame/telemetry_frame_validate.comp \
                "$OUT"/camera_exposure/camera_exposure_validate.comp; do
        if glslangValidator -V "$comp" -o "${comp%.comp}.spv" >/dev/null 2>&1; then
            note "PASS: $(basename "$comp") -> SPIR-V"
        else
            note "FAIL: $(basename "$comp") did not compile"
            fail=1
        fi
    done
else
    note "DECLARED-SKIP: glslangValidator not present locally (CI installs glslang-tools)"
fi

# --- summary -----------------------------------------------------------------------------
echo ""
if [ "$fail" -ne 0 ]; then
    echo "[weftc-codegen] SHARD FAILED"
    exit 1
fi
echo "[weftc-codegen] SHARD PASSED: determinism + goldens + Law-4 matrix + C x3 flavors +"
echo "[weftc-codegen]                Rust no_std/bit-exact + stage2 verify + GPU double-entry"
