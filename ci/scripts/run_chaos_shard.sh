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

step "5. v2 extended fault classes (Issue #16 Tier 1) — stepped, both regimes"
# faultMask 15 = BIT_FLIP + CACHE_POISON + STORE_TEARING + DELAYED_VISIBILITY at
# a corruption density below the measured starvation threshold (slots=8 +
# rate>=100 whole-ring-poisons faster than the writer heals — that overload
# regime is evidence-filed, not gated; see litmus/evidence/chaos-v2/).
for cfg in "200000 4 4 2 200 50 1337 15" "100000 4 4 2 50 50 77 15" "1000000 2 2 1 40 25 424242 6"; do
  read -r steps slots words readers frames rate seed mask <<< "$cfg"
  echo "--- stepped-v2 $steps/$slots/$words/$readers/$frames/$rate seed=$seed mask=$mask (fenced) ---" | tee -a "$LOG"
  if ./core/c/fanout-chaos stepped "$steps" "$slots" "$words" "$readers" "$frames" "$rate" "$seed" "$mask" >>"$LOG" 2>&1; then :; else fail=1; echo "stepped-v2(fenced) RED: $cfg" | tee -a "$LOG"; fi
  echo "--- stepped-v2 $steps/$slots/$words/$readers/$frames/$rate seed=$seed mask=$mask (all-seq_cst) ---" | tee -a "$LOG"
  if ./core/c/fanout-chaos-seq stepped "$steps" "$slots" "$words" "$readers" "$frames" "$rate" "$seed" "$mask" >>"$LOG" 2>&1; then :; else fail=1; echo "stepped-v2(seqcst) RED: $cfg" | tee -a "$LOG"; fi
done

step "6. v2 free-running (real ring, real corruption, 200K frames)"
# BIT_FLIP/CACHE_POISON land as relaxed atomic u64 stores on the production
# ring's ctrl stamps; DELAYED_VISIBILITY stalls post-store; STORE_TEARING is
# structurally impossible on the real ring (atomic stamps) — counted as the
# honest 0 and documented. A torn accept under corruption FAILS loudly here
# (rare aliasing); the stepped engine owns the attributed tier.
if ./core/c/fanout-chaos free 200000 4 8 3 40 424242 15 >>"$LOG" 2>&1; then :; else fail=1; echo "free-v2 200K RED" | tee -a "$LOG"; fi

if [ "$fail" -eq 0 ]; then
  echo '{"shard":"chaos","status":"PASSED","gates":"selftest + stepped shapes x2 regimes + asan + free 2M + v2 extended x2 regimes + v2 free 200K"}' > "$RESULTS"
  echo "chaos shard: PASS" | tee -a "$LOG"
else
  echo '{"shard":"chaos","status":"FAILED"}' > "$RESULTS"
  echo "chaos shard: RED" | tee -a "$LOG"
  exit 1
fi
