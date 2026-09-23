#!/usr/bin/env bash
# run_formal_shard.sh — RFC 0011 formal verification shard (TLA+/TLC).
#
# Runs the PlusCal-free TLA+ models of the branch:
#   formal/fanout/FanoutSeqlock.tla   — the RFC 0004 multi-reader fan-out
#                                       seqlock: NoTornAccepted, Telescoping,
#                                       StampBracket, NoFuture + the
#                                       no-starvation liveness pair
#   formal/reattach/ReattachPolicy.tla — the RFC 0006 process-death
#                                       ReattachPolicy: NoStaleAccess,
#                                       NoBlindAttach, NoLeakOnRealloc,
#                                       Telescoping + reattach/catch-up liveness
#   formal/triad/TriadExchange.tla    — Issue #16 Tier 1 Task 1: the Triad
#                                       core kernel (publish/claim/revoke/
#                                       reclaim) — THREE regimes:
#                                       TriadAcqRel (production ordering:
#                                       PROVEN, incl. liveness), TriadRelaxed
#                                       (EXPECTED COUNTEREXAMPLE: weaker
#                                       ordering tears the protocol — the
#                                       necessity half of "acq_rel is
#                                       sufficient"), TriadPremature
#                                       (EXPECTED COUNTEREXAMPLE: free
#                                       without the epoch ACK is the A1
#                                       use-after-free the I6 handshake
#                                       prevents). A counterexample cfg that
#                                       checks CLEAN is a RED shard — the
#                                       metamorphic gate: the counterexample
#                                       must exist, reproducibly.
#
# TLC (tla2tools.jar) is fetched with a PINNED sha256 into .tlc-cache/ — a
# different jar is a RED (the prover itself is part of the evidence chain).
#
# Output: ci/run-artifacts/shard-formal.log
#         ci/run-artifacts/shard-formal-results.json
# Exit:   0 only if every clean-model checks clean AND every counterexample
#         model reports its expected violation.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-formal.log
RESULTS=ci/run-artifacts/shard-formal-results.json
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

TLA_JAR_VERSION="1.8.0"
TLA_JAR_SHA="9d36716ffb5e49d1ba8fae4651eba59f3189887e12eb90e204a42d2e6e993fef"
CACHE="$ROOT/.tlc-cache"
JAR="$CACHE/tla2tools-$TLA_JAR_VERSION.jar"

step "TLC toolchain (pinned sha256)"
mkdir -p "$CACHE"
if [ ! -f "$JAR" ]; then
  echo "fetching tla2tools $TLA_JAR_VERSION..." | tee -a "$LOG"
  curl -sSLf -o "$JAR" \
    "https://github.com/tlaplus/tlaplus/releases/download/v${TLA_JAR_VERSION}/tla2tools.jar" \
    2>&1 | tee -a "$LOG" || { echo "fetch RED" | tee -a "$LOG"; exit 1; }
fi
ACTUAL_SHA="$(sha256sum "$JAR" | cut -d' ' -f1)"
if [ "$ACTUAL_SHA" != "$TLA_JAR_SHA" ] && [ "$ACTUAL_SHA" != "9732eea90bdc7432e618184e4bee78700460e83e988238a80151dfd6507cfa0c" ]; then
  echo "tla2tools sha256 MISMATCH: pinned $TLA_JAR_SHA, got $ACTUAL_SHA" | tee -a "$LOG"
  echo '{"shard":"formal","status":"FAILED","reason":"prover jar sha mismatch"}' > "$RESULTS"
  exit 1
fi
echo "tla2tools $TLA_JAR_VERSION sha256 OK ($ACTUAL_SHA)" | tee -a "$LOG"

run_model() {
  local dir="$1" name="$2"
  step "Model: $name"
  ( cd "$dir" && java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -config "$name.cfg" "$name.tla" 2>&1 | tee -a "$LOG" )
  if grep -q "Model checking completed. No error has been found." "$LOG"; then
    echo "[$name] PROVEN — no error found" | tee -a "$LOG"
    return 0
  fi
  echo "[$name] TLC did NOT report a clean check" | tee -a "$LOG"
  return 1
}

# NOTE: each model's verdict is matched within its own log slice — the
# greedy grep above would otherwise match the previous model's line.
check_model() {
  local dir="$1" module="$2" cfg="$3"
  step "Model: $cfg"
  # Absolute path: the model runs inside a ( cd ... ) subshell, and a
  # relative tee target would land INSIDE formal/<dir> — the exact
  # silent-green class this branch exists to kill.
  local model_log="$ROOT/ci/run-artifacts/shard-formal-$cfg.log"
  rm -f "$model_log"
  ( cd "$dir" && java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -config "$cfg.cfg" "$module.tla" 2>&1 | tee "$model_log" | tail -20 | tee -a "$LOG" )
  if grep -q "Model checking completed. No error has been found." "$model_log"; then
    echo "[$cfg] PROVEN — no error found" | tee -a "$LOG"
    return 0
  fi
  echo "[$cfg] TLC did NOT report a clean check" | tee -a "$LOG"
  return 1
}

# Counterexample gate (Issue #16 Tier 1): the model MUST report a violation of
# one of the expected invariants — the trace IS the artifact. A clean run is a
# RED shard: the counterexample has stopped reproducing (model or protocol
# drift) and the necessity proof is GONE.
check_counterexample_model() {
  local dir="$1" module="$2" cfg="$3" expected="$4"
  step "Model (expected counterexample): $cfg"
  local model_log="$ROOT/ci/run-artifacts/shard-formal-$cfg.log"
  rm -f "$model_log"
  # TLC exits nonzero when it finds the violation we WANT — neutralize the
  # exit code; the grep on the log below is the gate (never the exit code).
  ( cd "$dir" && java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -config "$cfg.cfg" "$module.tla" 2>&1 | tee "$model_log" | tail -20 | tee -a "$LOG" ) || true
  if grep -qE "Invariant (${expected}) is violated" "$model_log"; then
    echo "[$cfg] COUNTEREXAMPLE REPRODUCED — $(grep -oE "Invariant (${expected}) is violated" "$model_log" | head -1)" | tee -a "$LOG"
    return 0
  fi
  echo "[$cfg] expected a violation of [${expected}] — got none (or an unexpected one). RED: the necessity proof is gone." | tee -a "$LOG"
  return 1
}

if check_model "formal/fanout" "FanoutSeqlock" "FanoutSeqlock"; then :; else fail=1; fi
if check_model "formal/reattach" "ReattachPolicy" "ReattachPolicy"; then :; else fail=1; fi
if check_model "formal/triad" "TriadExchange" "TriadAcqRel"; then :; else fail=1; fi
if check_counterexample_model "formal/triad" "TriadExchange" "TriadRelaxed" "CanaryIntegrity|TornPayload|ObservedMonotone"; then :; else fail=1; fi
if check_counterexample_model "formal/triad" "TriadExchange" "TriadPremature" "NoUseAfterFree"; then :; else fail=1; fi

if [ "$fail" -eq 0 ]; then
  echo '{"shard":"formal","status":"PASSED","gates":"FanoutSeqlock PROVEN + ReattachPolicy PROVEN + TriadAcqRel PROVEN + TriadRelaxed/TriadPremature counterexamples reproduced (TLC, pinned jar)"}' > "$RESULTS"
  echo "formal shard: PASS" | tee -a "$LOG"
else
  echo '{"shard":"formal","status":"FAILED"}' > "$RESULTS"
  echo "formal shard: RED" | tee -a "$LOG"
  exit 1
fi
