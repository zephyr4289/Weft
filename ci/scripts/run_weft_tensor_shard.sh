#!/usr/bin/env bash
# run_weft_tensor_shard.sh — RFC-0017: the accelerator pillar shard
# (Pillar 2 / weft-tensor).
#
# Gates (any failure exits non-zero):
#   1. BUILD: every AC-series gate binary + bench in -O2 (with the
#      Law-1 malloc-audit interposer) and the ASAN legs.
#   2. NO-ICD LEG: the full gate set with NO Vulkan ICD — every
#      Vulkan-dependent leg must refuse/declare honestly (Law 4 as a
#      CI gate, the heterogeneous shard's precedent).
#   3. ICD LEG: with the software ICD installed (mesa-vulkan-drivers /
#      lavapipe), the positive proofs run — the fd-road dma-buf import,
#      the bit-exact GPU preprocess, the alias canary, and the bench's
#      GPU road (with the software-ICD labels).
#   4. SPV DETERMINISM: the committed .spv stays byte-identical when
#      glslangValidator rebuilds it (kernel freeze as a gate).
#   5. KERNEL FREEZE: core/c has ZERO diffs against the branch base
#      (Law 3 — the adapters are tools-layer; the core is untouched).
#
# Output: ci/run-artifacts/shard-weft-tensor.log
#         litmus/evidence/accelerators/*.log (the evidence pack)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts litmus/evidence/accelerators
LOG=ci/run-artifacts/shard-weft-tensor.log
: > "$LOG"
EV=litmus/evidence/accelerators

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }
run()  { echo "\$ $*" | tee -a "$LOG"; if "$@" >>"$LOG" 2>&1; then
             echo "  ok" | tee -a "$LOG"
         else
             echo "  FAILED (rc=$?)" | tee -a "$LOG"; fail=1
         fi }

# --- 0. environment ---------------------------------------------------------
step "environment"
{ uname -a; gcc --version | head -1; } | tee -a "$LOG"

# --- 1. build ---------------------------------------------------------------
step "build (O2 + audit interposer + asan legs)"
run make -C tools/weft-tensor all
run make -C tools/weft-tensor test-view-asan
run make -C tools/weft-tensor test-cross-asan

# --- 2. no-ICD leg -----------------------------------------------------------
step "no-ICD leg (refusal honesty as a gate)"
cd tools/weft-tensor
for t in test-view test-vk test-metal test-ort test-ggml test-cross \
         weft-accel-bench; do
    if ./$t > "$ROOT/$EV/ac-noicd-$t.log" 2>&1; then
        echo "$t: ok" | tee -a "$ROOT/$LOG"
    else
        echo "$t: FAILED" | tee -a "$ROOT/$LOG"; fail=1
    fi
done
cd "$ROOT"

# --- 3. ICD leg (software ICD; skips honestly when absent) -------------------
step "ICD leg (lavapipe)"
# The gpu-native shard's installer already provisioned the ICD; if the
# loader finds none, this leg self-skips with the reason (the no-ICD leg
# above already gated the refusals).
if command -v vulkaninfo >/dev/null 2>&1 || ls /usr/share/vulkan/icd.d/*.json >/dev/null 2>&1; then
    cd tools/weft-tensor
    for t in test-vk test-cross weft-accel-bench; do
        if ./$t > "$ROOT/$EV/ac-icd-$t.log" 2>&1; then
            echo "$t (ICD): ok" | tee -a "$ROOT/$LOG"
        else
            echo "$t (ICD): FAILED" | tee -a "$ROOT/$LOG"; fail=1
        fi
    done
    cd "$ROOT"
else
    echo "  SKIP: no Vulkan ICD on this runner (the no-ICD leg gates the refusals)" \
        | tee -a "$LOG"
fi

# --- 4. spv determinism -------------------------------------------------------
step "spv determinism (kernel freeze)"
if command -v glslangValidator >/dev/null 2>&1; then
    run make -C tools/weft-tensor spv-check
else
    echo "  SKIP: glslangValidator absent on this runner" | tee -a "$LOG"
fi

# --- 5. kernel freeze (core untouched) ----------------------------------------
step "kernel freeze: core/c zero diffs vs branch base"
BASE="$(git merge-base HEAD origin/main 2>/dev/null || git rev-list --max-parents=0 HEAD)"
if git diff --quiet "$BASE" -- core/c/weft.c core/c/weft.h; then
    echo "  core/c/weft.{c,h}: zero diffs (Law 3 holds)" | tee -a "$LOG"
else
    echo "  FAILED: core/c changed — the adapters must stay tools-layer" \
        | tee -a "$LOG"; fail=1
fi

# --- verdict -------------------------------------------------------------------
step "verdict"
if [ "$fail" -eq 0 ]; then
    echo "SHARD: GREEN" | tee -a "$LOG"
    exit 0
fi
echo "SHARD: RED" | tee -a "$LOG"
exit 1
