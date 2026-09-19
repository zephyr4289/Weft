#!/usr/bin/env bash
# run_riscv_litmus_shard.sh — RISC-V RV64GC portability gate (issue #18-2).
#
# Cross-compiles the C kernel + driver layer for riscv64-linux-gnu (RV64GC)
# and runs the FULL litmus matrix (L1-L8) + the F-series fan-out conformance
# (both ordering regimes) under qemu-riscv64 user-mode emulation.
#
# Toolchain (ubuntu-latest, root): apt packages gcc-riscv64-linux-gnu +
# qemu-user. On hosts WITHOUT root (evidence sandboxes), the rootless recipe
# is: apt-get download {gcc-riscv64-linux-gnu cpp-riscv64-linux-gnu
# cpp-14-riscv64-linux-gnu gcc-14-riscv64-linux-gnu
# gcc-14-riscv64-linux-gnu-base libgcc-14-dev-riscv64-cross
# libgcc-s1-riscv64-cross binutils-riscv64-linux-gnu libc6-dev-riscv64-cross
# libc6-riscv64-cross linux-libc-dev-riscv64-cross qemu-user} then
# dpkg-deb -x into a prefix, LD_LIBRARY_PATH=<prefix>/usr/lib/x86_64-linux-gnu
# for the cross binutils, and --sysroot=<prefix>/usr/riscv64-linux-gnu (the
# sysroot's libc.so linker scripts hardcode /usr/riscv64-linux-gnu — sed them
# to sysroot-relative). Documented in docs/riscv-port.md.
#
# Gates:
#   1. L1-L8 kernel litmus under QEMU (the issue's acceptance criterion)
#   2. F-series fan-out conformance, fenced acq/rel + all-seq_cst regimes
#   3. The atomics/fence AUDIT: the emitted instruction mapping must contain
#      amoswap.d.aqrl (the kernel's exchange), fence rw,w / r,rw (release/
#      acquire) and fence rw,rw (SeqCst) — the contract the memory-ordering
#      proofs assume on RV64GC.
#
# Output: ci/run-artifacts/shard-riscv-litmus.log + -results.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-riscv-litmus.log
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

SUDO=""
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
  SUDO="sudo"
fi

if ! command -v riscv64-linux-gnu-gcc >/dev/null 2>&1; then
  step "install cross toolchain + qemu (apt)"
  $SUDO apt-get update -qq >> "$LOG" 2>&1 || true
  $SUDO apt-get install -y gcc-riscv64-linux-gnu libc6-dev-riscv64-cross qemu-user qemu-user-static >> "$LOG" 2>&1 \
    || { echo '{"shard":"riscv-litmus","status":"FAILED","reason":"toolchain install"}'; exit 1; }
fi

QEMU_BIN="qemu-riscv64"
if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
  if command -v qemu-riscv64-static >/dev/null 2>&1; then
    QEMU_BIN="qemu-riscv64-static"
  else
    step "install qemu-user (apt)"
    $SUDO apt-get update -qq >> "$LOG" 2>&1 || true
    $SUDO apt-get install -y qemu-user qemu-user-static >> "$LOG" 2>&1 \
      || { echo '{"shard":"riscv-litmus","status":"FAILED","reason":"qemu install"}'; exit 1; }
    if command -v qemu-riscv64 >/dev/null 2>&1; then
      QEMU_BIN="qemu-riscv64"
    elif command -v qemu-riscv64-static >/dev/null 2>&1; then
      QEMU_BIN="qemu-riscv64-static"
    fi
  fi
fi

if [ -n "$SUDO" ] || [ "$(id -u)" -eq 0 ]; then
  if ! command -v qemu-riscv64 >/dev/null 2>&1 && command -v qemu-riscv64-static >/dev/null 2>&1; then
    $SUDO ln -sf "$(command -v qemu-riscv64-static)" /usr/local/bin/qemu-riscv64 || true
  fi
fi

export QEMU_LD_PREFIX=/usr/riscv64-linux-gnu

step "cross-compile spike (kernel litmus) + fanout-test (F-series) for RV64GC"
riscv64-linux-gnu-gcc -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE -Icore/c \
  -o /tmp/spike-rv core/c/weft.c core/c/fanout.c core/c/frame_cursor.c core/c/fanout_simd.c core/c/litmus_runner.c >> "$LOG" 2>&1 || fail=1
riscv64-linux-gnu-gcc -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE -Icore/c \
  -o /tmp/fanout-test-rv core/c/weft.c core/c/fanout.c core/c/frame_cursor.c \
  core/c/fanout_simd.c core/c/fanout_test.c >> "$LOG" 2>&1 || fail=1
riscv64-linux-gnu-gcc -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE -Icore/c \
  -DWEFT_FANOUT_SEQ_CST=1 \
  -o /tmp/fanout-test-rv-seq core/c/weft.c core/c/fanout.c core/c/frame_cursor.c \
  core/c/fanout_simd.c core/c/fanout_test.c >> "$LOG" 2>&1 || fail=1

step "L1-L8 kernel litmus under qemu-riscv64"
for t in L1-tear L2-writer-steps L3-reader-steps L4-freshness L5-progress \
         L6-ownership L7-revocation L8-envelope; do
  timeout 300 "$QEMU_BIN" /tmp/spike-rv "$t" >> "$LOG" 2>&1 || fail=1
done

step "F-series fan-out conformance (fenced acq/rel regime)"
"$QEMU_BIN" /tmp/fanout-test-rv >> "$LOG" 2>&1 || fail=1
step "F-series fan-out conformance (all-seq_cst regime)"
"$QEMU_BIN" /tmp/fanout-test-rv-seq >> "$LOG" 2>&1 || fail=1

step "atomics/fence audit (the emitted mapping must match the proofs)"
riscv64-linux-gnu-gcc -O2 -Icore/c -S -o /tmp/audit-rv.s ci/riscv/audit_rv.c >> "$LOG" 2>&1 || fail=1
for pat in "amoswap\.d\.aqrl" "fence[[:space:]]+rw,[[:space:]]*w" "fence[[:space:]]+r,[[:space:]]*rw" "fence[[:space:]]+rw,[[:space:]]*rw"; do
  if grep -Eq "$pat" /tmp/audit-rv.s; then
    echo "audit: found '$pat'" | tee -a "$LOG"
  else
    echo "audit: MISSING '$pat'" | tee -a "$LOG"
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo '{"shard":"riscv-litmus","status":"FAILED"}' > ci/run-artifacts/shard-riscv-litmus-results.json
  exit 1
fi
echo '{"shard":"riscv-litmus","status":"PASSED","gates":"L1-L8 qemu-rv64 + F-series x2 regimes + atomics audit (amoswap.aqrl / fences)"}' > ci/run-artifacts/shard-riscv-litmus-results.json
echo "✅ riscv-litmus shard PASSED" | tee -a "$LOG"
