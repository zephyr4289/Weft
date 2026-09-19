#!/usr/bin/env bash
# run_weftc_codegen_shard.sh — Project weftc, Pillar 1 native codegen shard
# (C11 / Rust / WGSL / GLSL generators under tools/weftc/codegen).
#
# Gates (any failure exits non-zero):
#   1. weftc-codegen builds as pure C11 (-std=c11 -Werror -pedantic)
#   2. codegen determinism — regenerate, byte-compare (two runs identical)
#   3. golden-file identity — committed goldens == fresh output
#   4. Law-4 warning matrix — the exact expected set of GPU refusals fires
#      (and only that set — silent drift in either direction fails)
#   5. C roundtrip x3 build flavors (default / -mavx2 / ASAN+UBSAN):
#      compile-time ABI static_asserts, zero-copy cast refusals, packed
#      read/write through misaligned buffers, bitfield accessors, the
#      exhaustive 65536-pattern f16 identity, SIMD batch validation;
#      AVX2 stage-1 bins must be byte-identical to the scalar build's
#   6. Rust: no_std lib compiles (bare-metal/MCU/kernel, `core` only) +
#      cross-language bit-exact protocol (C-written bins -> Rust reads,
#      Rust mutations -> C re-reads)
#   7. GPU double-entry: gpu_layout_check re-parses the emitted WGSL/GLSL
#      text, independently recomputes every offset, confronts the C headers'
#      offsetof ground truth; refusals carry markers and emit nothing
#   8. glslangValidator (when present): *_validate.comp proof shaders
#      compile to SPIR-V — installed on CI runners; declared-skip locally
#
# Output: ci/run-artifacts/shard-weftc-codegen.log
#         ci/run-artifacts/shard-weftc-codegen-results.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-weftc-codegen.log
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

# --- environment: glslang for the SPIR-V leg (optional, like gpu-native) ----
step "environment (glslang-tools when apt available)"
if command -v glslangValidator >/dev/null 2>&1; then
  echo "glslangValidator present" | tee -a "$LOG"
elif command -v apt-get >/dev/null 2>&1; then
  if [ "$(id -u)" = "0" ]; then
    APT="apt-get"
  elif command -v sudo >/dev/null 2>&1; then
    APT="sudo apt-get"
  else
    APT=""
  fi
  if [ -n "$APT" ]; then
    $APT update -qq >/dev/null 2>&1 || true
    $APT install -y -qq glslang-tools >/dev/null 2>&1 || \
      echo "apt install glslang-tools failed — SPIR-V leg will be declared-skip" | tee -a "$LOG"
  fi
fi

# --- the gate itself ----------------------------------------------------------
step "weftc-codegen Pillar 1 verification gate"
if make -C tools/weftc/codegen test 2>&1 | tee -a "$LOG"; then
  echo "gate green" | tee -a "$LOG"
else
  echo "gate FAILED" | tee -a "$LOG"
  fail=1
fi

# --- results -------------------------------------------------------------------
if [ "$fail" -ne 0 ]; then
  echo '{"shard":"weftc-codegen","status":"FAILED"}' > ci/run-artifacts/shard-weftc-codegen-results.json
  exit 1
fi
echo '{"shard":"weftc-codegen","status":"PASSED","gates":"generator build (pure C11 -Werror -pedantic) + determinism + goldens + Law-4 refusal matrix + C roundtrip x3 flavors (65 gates each) + AVX2/scalar byte-identity + Rust no_std + xlang bit-exact stage bins + GPU double-entry (WGSL/GLSL vs C offsetof) + glslang SPIR-V (optional leg)"}' > ci/run-artifacts/shard-weftc-codegen-results.json
echo "✅ weftc-codegen shard PASSED" | tee -a "$LOG"
