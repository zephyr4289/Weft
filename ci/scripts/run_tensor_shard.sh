#!/usr/bin/env bash
# run_tensor_shard.sh — RFC-0021 weft-tensor fabric shard (Pillar 2).
#
# Gates (any failure exits non-zero):
#   1. Golden-fixture freshness: the Python oracle's regenerated output
#      must be byte-identical to the committed fixture.
#   2. Full WT battery, plain leg: view geometry (WT1-16, incl. the
#      cross-implementation bit-exactness gate over 1D audio / 2D
#      spectrogram / 4D NCHW+NHWC / 5D KV-cache goldens), arena (WT17-24),
#      ring (WT25-40: lock-free claim/commit conformance, MPSC/SPMC/MMPC
#      tearing stress with per-ticket word verification, fork-torture
#      cross-process proof, bounded-wait deadlines, throughput fixture).
#   3. The entire battery under ASAN (Law 1/2 memory-safety audit).
#   4. The ring battery under TSAN (fork torture declared-skip).
#
# Zero kernel surface: core/c/tensor/ only; no frozen file is compiled,
# read, or written (Law 3 — the tree diff on this branch proves it).
#
# Output: ci/run-artifacts/shard-tensor.log
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-tensor.log
: > "$LOG"

step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

step "weft-tensor full gate (plain + ASAN + TSAN + golden freshness)"
make -C core/c/tensor clean 2>&1 | tee -a "$LOG"
bash core/c/tensor/tests/run.sh 2>&1 | tee -a "$LOG"

step "summary"
if grep -q "weft-tensor gate: ALL PASS" "$LOG"; then
    echo "weft-tensor shard: ALL PASS (plain + ASAN + TSAN legs)" | tee -a "$LOG"
else
    echo "weft-tensor shard: gate line missing — treating as failure" | tee -a "$LOG"
    exit 1
fi
