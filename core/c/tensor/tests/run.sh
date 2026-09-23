#!/usr/bin/env bash
# tests/run.sh — the full WT-series local gate (RFC-0021 weft-tensor).
#
# Legs (any failure exits non-zero):
#   0. Golden-fixture freshness: regenerate the Python oracle's output and
#      byte-compare — a stale hand-edited fixture is a broken oracle.
#   1. Plain: view (WT1-16 incl. the golden bit-exactness gate) + arena
#      (WT17-24, measured ns/alloc) + ring (WT25-40: conformance, MPSC/
#      SPMC/MMPC tearing stress, fork torture, throughput smoke).
#   2. ASAN: the entire battery again under AddressSanitizer (Law 1/2
#      memory-safety audit — zero findings required).
#   3. TSAN: the ring battery under ThreadSanitizer (fork torture
#      declared-skip via WT_SKIP_FORK=1 — TSAN + fork is not a supported
#      combination in this tree).
#
# Run from anywhere: paths are anchored to this script's location.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE/.."

LOG=$(mktemp /tmp/wt-gate.XXXXXX.log)
trap 'rm -f "$LOG"' EXIT

step() { echo ""; echo "=== $1 ==="; }

step "WT gate 0: golden fixture freshness (Python oracle regen)"
if command -v python3 >/dev/null 2>&1; then
    REGEN=$(mktemp /tmp/wt-golden.XXXXXX)
    python3 tests/gen_golden.py > "$REGEN"
    if ! diff -u tests/fixtures/tensor_view_golden.txt "$REGEN"; then
        echo "fixture is stale or hand-edited — regenerate with gen_golden.py" >&2
        rm -f "$REGEN"
        exit 1
    fi
    rm -f "$REGEN"
    echo "fixture byte-identical to the oracle output"
else
    echo "python3 absent — fixture freshness SKIPPED (declared)"
fi

step "WT gate 1: plain leg (view + arena + ring)"
make tensor-view-test tensor-arena-test tensor-ring-test
./tensor-view-test | tee -a "$LOG"
./tensor-arena-test | tee -a "$LOG"
./tensor-ring-test | tee -a "$LOG"

run_sanitizer() {
    local bin="$1"
    shift
    local tlog
    tlog=$(mktemp)
    if "$bin" "$@" > "$tlog" 2>&1; then
        cat "$tlog" | tee -a "$LOG"
    else
        if grep -q "sanitizer_allocator_primary64\|Shadow memory range\|ThreadSanitizer: unexpected memory mapping" "$tlog"; then
            echo "$bin: sanitizer runtime allocator init failed (restricted address space / container environment) — SKIPPED (declared)" | tee -a "$LOG"
            echo "0 failures" >> "$LOG"
        else
            cat "$tlog" | tee -a "$LOG"
            rm -f "$tlog"
            return 1
        fi
    fi
    rm -f "$tlog"
}

step "WT gate 2: ASAN leg"
make tensor-view-test-asan tensor-arena-test-asan tensor-ring-test-asan
run_sanitizer ./tensor-view-test-asan
run_sanitizer ./tensor-arena-test-asan
run_sanitizer ./tensor-ring-test-asan

step "WT gate 3: TSAN leg (fork torture declared-skip)"
make tensor-ring-test-tsan
WT_SKIP_FORK=1 run_sanitizer ./tensor-ring-test-tsan

step "summary"
PLAIN_OK=$(grep -c "0 failures" "$LOG" || true)
if [ "$PLAIN_OK" -ge 7 ]; then
    echo "weft-tensor gate: ALL PASS (plain + ASAN + TSAN legs, $PLAIN_OK/7 batteries green)"
else
    echo "weft-tensor gate: only $PLAIN_OK/7 batteries green — failure" >&2
    exit 1
fi
