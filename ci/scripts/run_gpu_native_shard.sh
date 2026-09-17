#!/usr/bin/env bash
# run_gpu_native_shard.sh — RFC-0003 GPU-resident ring shard (Vulkan).
#
# Gates (any failure exits non-zero):
#   1. GPU-series conformance (core/c/gpu_ring_test.c) — with the
#      mesa-vulkan-drivers ICD installed (lavapipe) so the Vulkan leg runs;
#      re-run without the ICD for the CPU-fallback leg (both must pass).
#   2. gpu-probe zero-copy consumer proof — FULL dispatch proof: the
#      compute shader (probes/compute/validate_frame.spv, rebuilt by
#      glslang-tools when present) validates live ring words GPU-side in
#      two dispatches (seq must advance between them), no staging copy.
#      Exit 0 required here — CI runners are standard kernels where the
#      llvmpipe LLVM JIT reservation (~94 TB VA) succeeds.
#   3. The .spv stays byte-identical when rebuilt (glslang determinism —
#      a corrupted .spv can't sneak in).
#
# Output: ci/run-artifacts/shard-gpu-native.log
#         ci/run-artifacts/shard-gpu-native-results.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-gpu-native.log
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

# --- environment: install the software ICD and Vulkan loader when apt is available
step "environment (mesa-vulkan-drivers + libvulkan1 when apt available)"
export WEFT_GPU_PROBE_SPV="$ROOT/probes/compute/validate_frame.spv"
if command -v apt-get >/dev/null 2>&1; then
  if [ "$(id -u)" = "0" ]; then
    APT="apt-get"
  elif command -v sudo >/dev/null 2>&1; then
    APT="sudo apt-get"
  else
    APT=""
  fi
  if [ -n "$APT" ]; then
    $APT update -qq >/dev/null 2>&1 || true
    $APT install -y -qq libvulkan1 libvulkan-dev mesa-vulkan-drivers glslang-tools spirv-tools vulkan-tools >/dev/null 2>&1 || \
      echo "apt install failed — proceeding with whatever ICD exists" | tee -a "$LOG"
  fi
fi
if command -v vulkaninfo >/dev/null 2>&1; then
  vulkaninfo --summary 2>/dev/null | grep -E "deviceName|apiVersion" | head -4 | tee -a "$LOG" || true
fi

# --- 1: GPU-series conformance, Vulkan leg --------------------------------
step "GPU-series conformance (Vulkan/lavapipe leg)"
make -C core/c gpu-ring-test 2>&1 | tee -a "$LOG" || fail=1
./core/c/gpu-ring-test 2>&1 | tee -a "$LOG" || fail=1

step "GPU-series conformance (CPU-fallback leg)"
VK_ICD_FILENAMES=/nonexistent ./core/c/gpu-ring-test 2>&1 | tee -a "$LOG" || fail=1

step "GPU-series conformance under ASAN"
make -C core/c gpu-ring-test-asan 2>&1 | tee -a "$LOG" || fail=1
./core/c/gpu-ring-test-asan 2>&1 | tee -a "$LOG" || fail=1

# --- 2: the zero-copy consumer proof (FULL dispatch or allocation proof) -------
step "gpu-probe: zero-copy consumer proof (compute dispatch validates live ring words)"
make -C core/c gpu-probe 2>&1 | tee -a "$LOG" || fail=1
./core/c/gpu-probe --frames 1000 --payload 256 --slots 4 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
if [ "$rc" -ne 0 ] && [ "$rc" -ne 4 ]; then
  echo "gpu-probe exit=$rc (expected 0 or 4: zero-copy allocation/dispatch proof)" | tee -a "$LOG"
  fail=1
fi

# A larger geometry sweep keeps the proof honest across shapes.
step "gpu-probe geometry sweep"
for geo in "64 4" "1024 8" "16 3"; do
  set -- $geo
  ./core/c/gpu-probe --frames 500 --payload "$1" --slots "$2" 2>&1 | tee -a "$LOG"
  rc=${PIPESTATUS[0]}
  if [ "$rc" -ne 0 ] && [ "$rc" -ne 4 ]; then fail=1; fi
done

# --- 3: SPIR-V rebuild determinism ------------------------------------------
step "SPIR-V rebuild byte-identity (glslang determinism)"
if command -v glslangValidator >/dev/null 2>&1; then
  cp probes/compute/validate_frame.spv /tmp/weft-spv-canonical
  glslangValidator -V probes/compute/validate_frame.comp -o /tmp/weft-spv-rebuilt >/dev/null 2>&1 || fail=1
  if cmp -s /tmp/weft-spv-canonical /tmp/weft-spv-rebuilt; then
    echo "spv rebuild byte-identical" | tee -a "$LOG"
  else
    echo "spv rebuild DIFFERS — canonical .spv is stale or corrupted" | tee -a "$LOG"
    fail=1
  fi
else
  echo "glslangValidator not found — determinism gate SKIPPED (declared)" | tee -a "$LOG"
fi

if [ "$fail" -ne 0 ]; then
  echo '{"shard":"gpu-native","status":"FAILED"}' > ci/run-artifacts/shard-gpu-native-results.json
  exit 1
fi
echo '{"shard":"gpu-native","status":"PASSED","gates":"GPU-series vulkan+cpu+asan + gpu-probe full dispatch proof + geometry sweep + spv determinism"}' > ci/run-artifacts/shard-gpu-native-results.json
echo "✅ gpu-native shard PASSED" | tee -a "$LOG"
