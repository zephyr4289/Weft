#!/usr/bin/env bash
# run_chaos_shard.sh — RFC 0011 chaos engine shard (the C reference tier).
#
# Runs the deterministic chaos engine end to end:
#   1. selftest        — pinned PRNG/pattern vectors + a known-good tiny run
#   2. stepped shapes  — the deterministic scheduler across shapes and chaos
#                        rates, BOTH ordering regimes (fenced acq/rel and
#                        all-seq_cst) — every run must PASS with zero
#                        violations of L-C1..L-C6
#   3. ASAN stepped    — the sanitizer leg over a heavy-chaos shape
#   4. free-running    — real threads + seed-deterministic fault sequence
#                        (PR tier: 2M frames; the nightly tier runs 10M —
#                        see run_chaos_parity.sh and nightly-deep.yml)
#
# Output: ci/run-artifacts/shard-chaos.log
#         ci/run-artifacts/shard-chaos-results.json
# Exit:   0 only if every leg passed. set -euo pipefail — no silent-green.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-chaos.log
RESULTS=ci/run-artifacts/shard-chaos-results.json
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

step "Build: fanout-chaos (free mode), fanout-chaos-seq (A/B regime), fanout-chaos-asan"
make -C core/c fanout-chaos fanout-chaos-seq fanout-chaos-asan 2>&1 | tee -a "$LOG"

step "1. selftest (pinned chaos-contract vectors)"
if ./core/c/fanout-chaos selftest 2>&1 | tee -a "$LOG"; then :; else fail=1; echo "selftest RED" | tee -a "$LOG"; fi

step "2. stepped shapes — the deterministic scheduler (both regimes)"
for cfg in "50000 2 2 1 50 350 7" "200000 4 4 2 200 200 1337" "12345 3 1 2 77 999 424242"; do
  read -r steps slots words readers frames rate seed <<< "$cfg"
  echo "--- stepped $steps/$slots/$words/$readers/$frames/$rate seed=$seed (fenced) ---" | tee -a "$LOG"
  if ./core/c/fanout-chaos stepped "$steps" "$slots" "$words" "$readers" "$frames" "$rate" "$seed" >>"$LOG" 2>&1; then :; else fail=1; echo "stepped(fenced) RED: $cfg" | tee -a "$LOG"; fi
  echo "--- stepped $steps/$slots/$words/$readers/$frames/$rate seed=$seed (all-seq_cst) ---" | tee -a "$LOG"
  if ./core/c/fanout-chaos-seq stepped "$steps" "$slots" "$words" "$readers" "$frames" "$rate" "$seed" >>"$LOG" 2>&1; then :; else fail=1; echo "stepped(seqcst) RED: $cfg" | tee -a "$LOG"; fi
done

step "3. sanitizer leg (ASAN, heavy chaos)"
if ./core/c/fanout-chaos-asan stepped 60000 4 2 2 60 400 777 >>"$LOG" 2>&1; then :; else fail=1; echo "asan stepped RED" | tee -a "$LOG"; fi

step "4. free-running (real threads, seed-deterministic faults, 2M frames)"
if ./core/c/fanout-chaos free 2000000 4 8 3 40 424242 >>"$LOG" 2>&1; then :; else fail=1; echo "free 2M RED" | tee -a "$LOG"; fi

if [ "$fail" -eq 0 ]; then
  echo '{"shard":"chaos","status":"PASSED","gates":"selftest + stepped shapes x2 regimes + asan + free 2M"}' > "$RESULTS"
  echo "chaos shard: PASS" | tee -a "$LOG"
else
  echo '{"shard":"chaos","status":"FAILED"}' > "$RESULTS"
  echo "chaos shard: RED" | tee -a "$LOG"
  exit 1
fi
