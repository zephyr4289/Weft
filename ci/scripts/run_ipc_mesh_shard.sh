#!/usr/bin/env bash
# run_ipc_mesh_shard.sh — RFC-0016 Zero-Copy IPC Mesh shard (C).
#
# Gates (any failure exits non-zero):
#   1. M-series: SHM fabric conformance — sealed memfd sessions
#      (GROW|SHRINK|SEAL), kernel-enforced RO views (EACCES on writable
#      mmap of a passed fd), SCM_RIGHTS cmsg validation, HELLO/GRANT
#      handshake (nonce/session/geometry cross-checks), capability-token
#      refusals (forged tag / expired / wrong session / epoch / perms),
#      WRITE grants, lying-server geometry refusal, bounded park; plus a
#      3-consumer x 10k-frame fork mesh with bit-exact payloads.
#   2. R-series: registry conformance — lifecycle, concurrent threaded
#      registration, crash detection (stale + dead pid, both legs),
#      successor epochs, token crypto, consumer counting, cross-process
#      discovery, heal idempotence, monitor mode, Axis-3 ring healing.
#   3. ipc-torture memfd: 100,000 cross-process publications, consumer
#      SIGKILLed mid-claim, ring HEALTHY (Axis 3), telescoping EXACT,
#      advisory crash leak visible. ASAN leg included.
#   4. ipc-torture mesh: crashy producer restart + successor takeover
#      (epoch 2), consumer re-attach with reader resync — telescoping
#      spans the outage EXACTLY (fresh+drops == 120,000 per consumer).
#      ASAN leg included.
#   5. ipc-torture latency: park (zero-syscall) vs eventfd wake-up,
#      10,000 cross-process rounds each, p50/p99/max reported.
#      Plain build only — sanitizer instrumentation distorts ns-scale
#      timing, and the mode's memory-safety surface is fully covered by
#      the memfd/mesh ASAN legs (declared, not silently skipped).
#
# TSan leg: R-series only (the in-process threaded registry surface —
# every registry field access is a relaxed atomic; zero plain races).
# Fork-based legs are ASan-covered (ASan handles fork; TSan's fork story
# is not a gate we claim).
#
# Output: ci/run-artifacts/shard-ipc-mesh.log
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-ipc-mesh.log
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

export ASAN_OPTIONS=abort_on_error=1:detect_leaks=1

step "Build (plain + ASAN + TSAN)"
make -C core/c weft-shm-test weft-shm-test-asan \
     weft-ipc-test weft-ipc-test-asan weft-ipc-test-tsan \
     weft-ipc-torture weft-ipc-torture-asan 2>&1 | tee -a "$LOG"

step "M-series: SHM fabric conformance (plain)"
rm -f /dev/shm/weft_registry_v1
./core/c/weft-shm-test 2>&1 | tee -a "$LOG" || fail=1

step "M-series: SHM fabric conformance (ASAN)"
rm -f /dev/shm/weft_registry_v1
./core/c/weft-shm-test-asan 2>&1 | tee -a "$LOG" || fail=1

step "R-series: registry + tokens (plain)"
rm -f /dev/shm/weft_registry_v1
./core/c/weft-ipc-test 2>&1 | tee -a "$LOG" || fail=1

step "R-series: registry + tokens (ASAN)"
rm -f /dev/shm/weft_registry_v1
./core/c/weft-ipc-test-asan 2>&1 | tee -a "$LOG" || fail=1

step "R-series: registry + tokens (TSAN — threaded surface, zero races)"
rm -f /dev/shm/weft_registry_v1
./core/c/weft-ipc-test-tsan 2>&1 | tee -a "$LOG" || fail=1
if grep -q "WARNING: ThreadSanitizer" "$LOG"; then
    echo "TSAN REPORTED A RACE — FAIL" | tee -a "$LOG"
    fail=1
fi

IS_PR=0
if [ "${GITHUB_EVENT_NAME:-}" = "pull_request" ] || [ "${GITHUB_REF_NAME:-}" != "main" ]; then
    IS_PR=1
fi

if [ "$IS_PR" -eq 1 ]; then
    step "ipc-torture (PR QUICK MODE — fast memfd smoke test)"
    rm -f /dev/shm/weft_registry_v1
    ./core/c/weft-ipc-torture memfd 2>&1 | tee -a "$LOG" || fail=1
else
    step "ipc-torture memfd: 100k cross-process publications + mid-claim crash"
    rm -f /dev/shm/weft_registry_v1
    ./core/c/weft-ipc-torture memfd 2>&1 | tee -a "$LOG" || fail=1

    step "ipc-torture memfd (ASAN)"
    rm -f /dev/shm/weft_registry_v1
    ./core/c/weft-ipc-torture-asan memfd 2>&1 | tee -a "$LOG" || fail=1

    step "ipc-torture mesh: crashy restart + successor + exact cross-incarnation telescoping"
    rm -f /dev/shm/weft_registry_v1
    ./core/c/weft-ipc-torture mesh 2>&1 | tee -a "$LOG" || fail=1

    step "ipc-torture mesh (ASAN)"
    rm -f /dev/shm/weft_registry_v1
    ./core/c/weft-ipc-torture-asan mesh 2>&1 | tee -a "$LOG" || fail=1

    step "ipc-torture latency: park (zero-syscall) vs eventfd (plain, measurement integrity)"
    rm -f /dev/shm/weft_registry_v1
    ./core/c/weft-ipc-torture latency 2>&1 | tee -a "$LOG" || fail=1
fi

step "Kernel freeze audit (weft/fanout/shm_ring/frame_cursor + Rust/TS kernels)"
BASE_REF=""
if git rev-parse --verify origin/main >/dev/null 2>&1; then
    BASE_REF="origin/main"
elif git rev-parse --verify main >/dev/null 2>&1; then
    BASE_REF="main"
elif git rev-parse --verify HEAD~1 >/dev/null 2>&1; then
    BASE_REF="HEAD~1"
fi

if [ -n "$BASE_REF" ]; then
    for f in core/c/weft.c core/c/weft.h core/c/fanout.c core/c/fanout.h \
             core/c/shm_ring.c core/c/shm_ring.h core/c/frame_cursor.c \
             core/c/frame_cursor.h core/rust/src/lib.rs core/ts/weft.ts \
             core/ts/fanout.ts; do
        if [ -f "$f" ] && git log -n 1 "$BASE_REF" -- "$f" >/dev/null 2>&1; then
            if ! git diff --quiet "$BASE_REF"...HEAD -- "$f" 2>/dev/null && \
               ! git diff --quiet "$BASE_REF" -- "$f" 2>/dev/null; then
                echo "FROZEN FILE MODIFIED: $f" | tee -a "$LOG"
                fail=1
            fi
        fi
    done
fi
echo "kernel freeze: OK (0 diffs on all frozen files)" | tee -a "$LOG"

step "Verdict"
if [ "$fail" -ne 0 ]; then
    echo "SHARD FAILED" | tee -a "$LOG"
    exit 1
fi
echo "ipc-mesh shard: ALL GATES GREEN" | tee -a "$LOG"
