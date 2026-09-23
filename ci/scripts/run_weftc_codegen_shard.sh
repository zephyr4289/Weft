#!/usr/bin/env bash
# run_weftc_codegen_shard.sh — Project weftc, Pillar 1 Unified Codegen Shard
# (Native C11/Rust/WGSL/GLSL + Managed TS/Python/Swift/Dart/React/Flutter).
#
# Gates:
#   Part A: Native Systems & GPU Codegen (Engineer 2)
#     - C11/Rust/WGSL/GLSL generators, Law 4 refusal matrix, C roundtrip x3 flavors,
#       Rust no_std lib + cross-language roundtrip, GPU double-entry, glslang SPIR-V.
#   Part B: Managed Runtimes & UI Fabric (Engineer 3)
#     - IR v1 loader validation, TS DataView runtime suite (heap probe, Laws 1/2/3/4),
#       Python struct/memoryview/NumPy suite, Swift/Dart static audits,
#       cross-backend parity matrix (12 rows), codegen determinism, C kernel harness E2E.
#
# Output: ci/run-artifacts/shard-weftc-codegen.log
#         ci/run-artifacts/shard-weftc-codegen-results.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG="ci/run-artifacts/shard-weftc-codegen.log"
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

# --- Part A: Native Codegen Gate ---
step "Part A: Native Systems & GPU Codegen (C11, Rust, WGSL, GLSL)"
if make -C tools/weftc/codegen test 2>&1 | tee -a "$LOG"; then
  echo "Native codegen gate green" | tee -a "$LOG"
else
  echo "Native codegen gate FAILED" | tee -a "$LOG"
  fail=1
fi

# --- Part B: Managed Codegen & UI Fabric Gate ---
step "Part B: Managed Runtimes & Reactive UI (TS, Python, Swift, Dart, React, Flutter)"
if bash tools/weftc/tests/run_integration.sh 2>&1 | tee -a "$LOG"; then
  echo "Managed codegen gate green" | tee -a "$LOG"
else
  echo "Managed codegen gate FAILED" | tee -a "$LOG"
  fail=1
fi

# --- Results Summary ---
if [ "$fail" -ne 0 ]; then
  echo '{"shard":"weftc-codegen","status":"FAILED"}' > ci/run-artifacts/shard-weftc-codegen-results.json
  exit 1
fi

echo '{"shard":"weftc-codegen","status":"PASSED","gates":"native (C11/Rust/WGSL/GLSL) + managed (TS/Python/Swift/Dart/React/Flutter) + parity matrix + E2E kernel round-trip"}' > ci/run-artifacts/shard-weftc-codegen-results.json
echo "✅ weftc-codegen shard ALL PASS" | tee -a "$LOG"
