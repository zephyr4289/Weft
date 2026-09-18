#!/usr/bin/env bash
# run_revoke_stress_shard.sh — Issue #16 Tier 1 Task 5: revocation handshake
# stress (core/c/revoke_stress.c).
#
# Legs:
#   1. plain   100,000 handshakes (concurrency 256 — the fast lane on small
#              VMs; the wave engine preserves the concurrent-racing property)
#   2. plain    5,000 handshakes at the MEASURED concurrency ceiling
#              (the probe reports the true simultaneously-alive limit)
#   3. ASAN    10,000 handshakes (use-after-free / leak oracle)
#   4. TSAN     2,000 handshakes, concurrency 64, probe capped (TSAN's
#              per-thread shadow setup makes the full probe and full
#              concurrency disproportionately slow — REDUCED AND DECLARED,
#              never silently skipped)
#
# The issue's "100K concurrent writers" is beyond any single sandbox's
# scheduler (this class of VM measures a ~724-thread ceiling); the wave
# decomposition delivers 100K handshakes under real concurrent racing and
# MEASURES the ceiling instead of assuming it. The full-scale run's
# evidence lives in litmus/evidence/revoke-stress/.
#
# Output: ci/run-artifacts/shard-revoke-stress.log + -results.json
# Exit: 0 only if every leg reports zero timeouts, zero ack failures,
# zero poison failures, zero spawn failures.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-revoke-stress.log
RESULTS=ci/run-artifacts/shard-revoke-stress-results.json
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

step "Build: revoke-stress, revoke-stress-asan, revoke-stress-tsan"
make -C core/c revoke-stress revoke-stress-asan revoke-stress-tsan 2>&1 | tee -a "$LOG"

step "1. plain 100,000 handshakes (concurrency 256)"
if ./core/c/revoke-stress 100000 256 2000 2>&1 | tee -a "$LOG"; then :; else fail=1; echo "leg 1 RED" | tee -a "$LOG"; fi

step "2. plain 5,000 handshakes at the measured ceiling"
if ./core/c/revoke-stress 5000 4096 2000 2>&1 | tee -a "$LOG"; then :; else fail=1; echo "leg 2 RED" | tee -a "$LOG"; fi

step "3. ASAN 10,000 handshakes (memory-safety oracle)"
if ./core/c/revoke-stress-asan 10000 256 2000 2>&1 | tee -a "$LOG"; then :; else fail=1; echo "leg 3 RED" | tee -a "$LOG"; fi

step "4. TSAN 2,000 handshakes (reduced — declared; data-race oracle)"
if ./core/c/revoke-stress-tsan 2000 64 2000 1024 2>&1 | tee -a "$LOG"; then :; else fail=1; echo "leg 4 RED" | tee -a "$LOG"; fi

if [ "$fail" -eq 0 ]; then
  echo '{"shard":"revoke-stress","status":"PASSED","gates":"100K handshakes + ceiling leg + ASAN + TSAN(reduced, declared) — zero timeouts/ack-failures/poison-failures"}' > "$RESULTS"
  echo "revoke-stress shard: PASS" | tee -a "$LOG"
else
  echo '{"shard":"revoke-stress","status":"FAILED"}' > "$RESULTS"
  echo "revoke-stress shard: RED" | tee -a "$LOG"
  exit 1
fi
