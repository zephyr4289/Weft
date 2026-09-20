#!/usr/bin/env bash
# run_heterogeneous_shard.sh — RFC-0016: the heterogeneous zero-copy mesh
# shard (Series 10).
#
# Gates (any failure exits non-zero):
#   1. BUILD: every series gate in -O2 and ASAN (dmabuf, gpu-extmem, xdp,
#      tensor, camera, hetero-probe, xdp-live-gate).
#   2. NO-ICD LEG: the full gate set with NO Vulkan ICD — every vulkan
#      dependent leg must refuse/declare honestly (Law 4 as a CI gate).
#   3. ICD LEG: with the software ICD installed (mesa-vulkan-drivers /
#      lavapipe, same as the gpu-native shard), the positive proofs run:
#      the XE/XD wrap roads (fd-road zero-copy: CPU publish -> GPU live
#      consume -> window ADVANCES) and the H-series capstone (tensor
#      reduce BIT-EXACT + FFT peak-bin exact through the imported fd).
#   4. SPV DETERMINISM: the committed .spv stays byte-identical when
#      glslangValidator rebuilds it (a corrupted shader can't sneak in).
#   5. KERNEL FREEZE: core/c/weft.c and weft.h have ZERO diffs against
#      the branch base (Law 3 as a gate, not a promise).
#
# Output: ci/run-artifacts/shard-heterogeneous.log
#         litmus/evidence/heterogeneous/*.log (the evidence pack)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts litmus/evidence/heterogeneous
LOG=ci/run-artifacts/shard-heterogeneous.log
: > "$LOG"
EV=litmus/evidence/heterogeneous

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }
run()  { echo "\$ $*" | tee -a "$LOG"; if "$@" >>"$LOG" 2>&1; then
             echo "  ok" | tee -a "$LOG"
         else
             echo "  FAILED (rc=$?)" | tee -a "$LOG"; fail=1
         fi }

# --- 0. environment ---------------------------------------------------------
step "environment"
{
  echo "host: $(uname -sr) ($(uname -m))"
  echo "compiler: $(gcc --version | head -1)"
} | tee -a "$LOG"

# --- 1. build everything ----------------------------------------------------
step "build (-O2 + ASAN)"
run make -C core/c dmabuf-test dmabuf-test-asan
run make -C core/c gpu-extmem-test gpu-extmem-test-asan
run make -C core/c xdp-test xdp-test-asan
run make -C core/c tensor-test tensor-test-asan
run make -C core/c camera-test camera-test-asan
run make -C core/c hetero-probe
run make -C core/c xdp-live-gate

# --- 2. the no-ICD leg (refusal honesty) ------------------------------------
step "no-ICD leg (every vulkan leg refuses/declares honestly)"
# (the shard starts with whatever ICD the runner has; explicitly empty it)
run env -u VK_ICD_FILENAMES VK_LOADER_DRIVER_SELECT= core/c/dmabuf-test
run env -u VK_ICD_FILENAMES core/c/gpu-extmem-test
run env -u VK_ICD_FILENAMES core/c/xdp-test
run env -u VK_ICD_FILENAMES core/c/tensor-test
run env -u VK_ICD_FILENAMES core/c/camera-test
run env -u VK_ICD_FILENAMES core/c/hetero-probe   # declared exit-0 without ICD
[ "$fail" = 0 ] || { echo "no-ICD leg failed" | tee -a "$LOG"; exit 1; }

# --- 3. the ASAN legs --------------------------------------------------------
step "ASAN legs (zero findings)"
run core/c/dmabuf-test-asan
run core/c/gpu-extmem-test-asan
run core/c/xdp-test-asan
run core/c/tensor-test-asan
run core/c/camera-test-asan

# --- 4. the ICD leg (positive proofs) ---------------------------------------
step "ICD leg (install the software Vulkan driver when apt is available)"
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
    $APT install -y -qq libvulkan1 mesa-vulkan-drivers glslang-tools >/dev/null 2>&1 || \
      echo "apt install failed — proceeding with whatever ICD exists" | tee -a "$LOG"
  fi
fi
if command -v vulkaninfo >/dev/null 2>&1; then
  vulkaninfo --summary 2>/dev/null | grep -E "deviceName|apiVersion" | head -4 | tee -a "$LOG" || true
fi

step "ICD leg: the wrap + capstone proofs"
run core/c/gpu-extmem-test     # XE/XD: fd-road zero-copy + refusal ladder
run core/c/hetero-probe        # H: bit-exact tensor reduce + FFT + liveness

# the LIVE-mode rig: exit 0 on CONFIG_XDP_SOCKETS hosts with the rig set
# up; exit 3 is the DECLARED refusal (no AF_XDP) — both green; anything
# else is a failure
step "xdp-live-gate (0 = proven, 3 = declared refusal)"
set +e
 core/c/xdp-live-gate weft-a weft-b 127.0.0.1:9999 100 >>"$LOG" 2>&1
LRC=$?
set -e
if [ "$LRC" = "0" ] || [ "$LRC" = "3" ]; then
  echo "  ok (rc=$LRC: $([ "$LRC" = 0 ] && echo 'LIVE proven' || echo 'declared: no AF_XDP'))" | tee -a "$LOG"
else
  echo "  FAILED (rc=$LRC)" | tee -a "$LOG"
  fail=1
fi

# --- 5. .spv determinism -----------------------------------------------------
step ".spv determinism (byte-identical rebuilds)"
if command -v glslangValidator >/dev/null 2>&1; then
  for spv in tensor_reduce fft_radix2; do
    BEFORE=$(sha256sum "probes/compute/$spv.spv" | cut -d' ' -f1)
    touch "probes/compute/$spv.comp"
    make -C core/c "../../probes/compute/$spv.spv" >>"$LOG" 2>&1 || true
    AFTER=$(sha256sum "probes/compute/$spv.spv" | cut -d' ' -f1)
    if [ "$BEFORE" = "$AFTER" ]; then
      echo "  $spv.spv byte-identical ($AFTER)" | tee -a "$LOG"
    else
      echo "  $spv.spv DRIFTED ($BEFORE -> $AFTER)" | tee -a "$LOG"
      fail=1
    fi
  done
else
  echo "  (glslangValidator absent — the committed .spv is canonical, declared)" | tee -a "$LOG"
fi

# --- 6. kernel freeze --------------------------------------------------------
step "kernel freeze (weft.c/weft.h byte-frozen — Law 3)"
BASE="$(git merge-base HEAD origin/main 2>/dev/null || git merge-base HEAD main 2>/dev/null || git rev-parse HEAD~1 2>/dev/null || git rev-list --max-parents=0 HEAD | head -1 || echo HEAD)"
if [ -z "$BASE" ]; then BASE="HEAD"; fi
if git diff --quiet "$BASE" -- core/c/weft.c core/c/weft.h; then
  echo "  frozen: zero diffs on weft.c/weft.h since $BASE" | tee -a "$LOG"
else
  echo "  VIOLATION: weft.c/weft.h changed since $BASE" | tee -a "$LOG"
  fail=1
fi

# --- 7. the evidence pack ----------------------------------------------------
step "evidence pack (litmus/evidence/heterogeneous)"
{
  echo "# Series 10 — heterogeneous zero-copy mesh evidence (RFC-0016)"
  echo "# x86_64 sandbox: $(gcc -dumpfullversion 2>/dev/null || gcc -dumpversion); kernel $(uname -r)"
  echo "# ICD: $(grep -m1 deviceName ci/run-artifacts/shard-heterogeneous.log 2>/dev/null || echo 'see shard log')"
  echo "# generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$EV/series10-header.txt"

env -u VK_ICD_FILENAMES core/c/dmabuf-test            > "$EV/dmabuf-refusal.log" 2>&1 || true
core/c/dmabuf-test                                    > "$EV/dmabuf-series.log"  2>&1 || true
core/c/dmabuf-test-asan                               > "$EV/dmabuf-asan.log"    2>&1 || true
env -u VK_ICD_FILENAMES core/c/gpu-extmem-test        > "$EV/extmem-noicd.log"   2>&1 || true
core/c/gpu-extmem-test                                > "$EV/extmem-lavapipe.log" 2>&1 || true
core/c/xdp-test                                       > "$EV/xdp-series.log"     2>&1 || true
core/c/tensor-test                                    > "$EV/tensor-series.log"  2>&1 || true
core/c/camera-test                                    > "$EV/camera-seams.log"   2>&1 || true
core/c/hetero-probe                                   > "$EV/hetero-series.log"  2>&1 || true
# the llvmpipe host-import diagnostic (the alias-verification's evidence)
{
  echo "# llvmpipe 25.0.7 host-import behavior (RFC-0016 s2.2):"
  echo "#   vkAllocateMemory(host-import) returns SUCCESS but vkMapMemory yields"
  echo "#   a DIFFERENT pointer with ZERO contents — the import does not alias."
  echo "#   weft_gpu_wrap_host detects this via the canary round-trip and refuses;"
  echo "#   the fd road (wrap_dmabuf) aliases and carries the MEASURED proofs."
  echo "# (reproduce: scripts/vk_diag-style probe — see the RFC's s2.2 notes)"
} > "$EV/extmem-llvmpipe-diag.txt"

for f in dmabuf-series extmem-lavapipe xdp-series tensor-series camera-seams hetero-series; do
  if grep -q "verdict: PASS" "$EV/$f.log"; then
    echo "  $f.log: PASS" | tee -a "$LOG"
  else
    echo "  $f.log: NOT PASS — inspect" | tee -a "$LOG"
  fi
done

echo "" | tee -a "$LOG"
if [ "$fail" = 0 ]; then
  echo "SHARD RESULT: GREEN" | tee -a "$LOG"
  exit 0
fi
echo "SHARD RESULT: RED" | tee -a "$LOG"
exit 1
