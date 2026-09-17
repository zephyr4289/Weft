#!/usr/bin/env bash
# run_chaos_parity.sh — RFC 0011 cross-language chaos PARITY (the flagship gate).
#
# The stepped chaos engine exists in five ports (C, TS, JVM/Kotlin, Dart,
# Swift). Given the same config, EVERY port executes the bit-identical
# schedule and must emit the BYTE-IDENTICAL verdict JSON. This driver:
#
#   1. C (reference oracle): stepped run -> verdict JSON.
#   2. TS (always, via node): same config -> verdict JSON -> byte diff vs C.
#   3. JVM (kotlinc when present): same -> byte diff vs C (loud skip when
#      the toolchain is absent — never silent).
#   4. Dart (dart when present): same -> byte diff vs C (loud skip).
#   5. Swift (SOURCE-ONLY in the Linux sandbox — pinned to the committed
#      golden fixture by the XCTest leg on macOS runners; this script
#      verifies the golden fixture itself is a C oracle output).
#   6. Golden fixture check: the committed stepped-golden-200k.json must be
#      reproducible byte-for-byte by the C engine here and now.
#
# Output: ci/run-artifacts/shard-chaos-parity.log
#         ci/run-artifacts/shard-chaos-parity-results.json
# Exit:   0 only if every RUN leg passed and every PARITY diff is empty.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-chaos-parity.log
RESULTS=ci/run-artifacts/shard-chaos-parity-results.json
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

CFG_STEPS=200000; CFG_SLOTS=4; CFG_WORDS=4; CFG_READERS=2; CFG_FRAMES=200; CFG_RATE=200; CFG_SEED=1337
CFG2_STEPS=800000; CFG2_SLOTS=8; CFG2_WORDS=8; CFG2_READERS=4; CFG2_FRAMES=4000; CFG2_RATE=100; CFG2_SEED=90210

parity_run() {
  # parity_run <label> <command...> — runs a port, byte-diffs BOTH configs vs C
  local label="$1"; shift
  local c1 c2
  c1=$(./core/c/fanout-chaos stepped "$CFG_STEPS" "$CFG_SLOTS" "$CFG_WORDS" "$CFG_READERS" "$CFG_FRAMES" "$CFG_RATE" "$CFG_SEED" 2>/dev/null)
  c2=$(./core/c/fanout-chaos stepped "$CFG2_STEPS" "$CFG2_SLOTS" "$CFG2_WORDS" "$CFG2_READERS" "$CFG2_FRAMES" "$CFG2_RATE" "$CFG2_SEED" 2>/dev/null)
  local p1 p2
  p1=$("$@" "$CFG_STEPS" "$CFG_SLOTS" "$CFG_WORDS" "$CFG_READERS" "$CFG_FRAMES" "$CFG_RATE" "$CFG_SEED" 2>/dev/null)
  p2=$("$@" "$CFG2_STEPS" "$CFG2_SLOTS" "$CFG2_WORDS" "$CFG2_READERS" "$CFG2_FRAMES" "$CFG2_RATE" "$CFG2_SEED" 2>/dev/null)
  if [ "$c1" = "$p1" ] && [ "$c2" = "$p2" ]; then
    echo "[$label] BYTE-IDENTICAL to C on both configs" | tee -a "$LOG"
    return 0
  fi
  echo "[$label] PARITY BROKEN" | tee -a "$LOG"
  echo "  C:   $c1" | tee -a "$LOG"
  echo "  $label: $p1" | tee -a "$LOG"
  return 1
}

step "Build C oracle"
make -C core/c fanout-chaos 2>&1 | tee -a "$LOG"

step "1. C reference oracle (sanity: deterministic double-run)"
./core/c/fanout-chaos stepped "$CFG_STEPS" "$CFG_SLOTS" "$CFG_WORDS" "$CFG_READERS" "$CFG_FRAMES" "$CFG_RATE" "$CFG_SEED" 2>/dev/null > /tmp/chaos-c1.json
./core/c/fanout-chaos stepped "$CFG_STEPS" "$CFG_SLOTS" "$CFG_WORDS" "$CFG_READERS" "$CFG_FRAMES" "$CFG_RATE" "$CFG_SEED" 2>/dev/null > /tmp/chaos-c1b.json
if diff -q /tmp/chaos-c1.json /tmp/chaos-c1b.json >/dev/null; then
  echo "C oracle deterministic: byte-identical double-run" | tee -a "$LOG"
else
  echo "C oracle NON-DETERMINISTIC — contract broken" | tee -a "$LOG"; fail=1
fi

step "2. TS port (node, always)"
parity_run ts node -e '
const cfg = process.argv.slice(1).map(Number);
import(process.cwd() + "/core/ts/fanout_chaos.ts").then(m => {
  const v = m.runSteppedChaos({seed: cfg[6]>>>0, steps: cfg[0], slots: cfg[1],
    words: cfg[2], readers: cfg[3], frames: cfg[4], chaosRate: cfg[5]});
  process.stdout.write(v.json);
});' || fail=1

step "3. JVM port (kotlinc when present)"
if command -v kotlinc >/dev/null 2>&1 || [ -x "$HOME/kotlinc-home/kotlinc/bin/kotlinc" ]; then
  KOTLINC="$(command -v kotlinc || echo "$HOME/kotlinc-home/kotlinc/bin/kotlinc")"
  if [ ! -f /tmp/chaos-jvm.jar ] || [ core/kotlin/FanoutChaos.kt -nt /tmp/chaos-jvm.jar ]; then
    "$KOTLINC" core/kotlin/FanoutChaos.kt -include-runtime -d /tmp/chaos-jvm.jar >>"$LOG" 2>&1
  fi
  if parity_run jvm java -cp /tmp/chaos-jvm.jar FanoutChaosKt stepped; then :; else fail=1; fi
else
  echo "kotlinc not found — JVM parity leg SKIPPED (declared; android-packages gradle CI covers)" | tee -a "$LOG"
fi

step "4. Dart port (dart when present)"
DART_BIN="$(command -v dart || echo "$HOME/dart-sdk/dart-sdk/bin/dart")"
if [ -x "$(echo "$DART_BIN" | cut -d' ' -f1)" ] || command -v dart >/dev/null 2>&1; then
  if parity_run dart "$DART_BIN" run core/dart/fanout_chaos.dart stepped; then :; else fail=1; fi
else
  echo "dart not found — Dart parity leg SKIPPED (declared; flutter-packages CI covers)" | tee -a "$LOG"
fi

step "5. Swift port (SOURCE-ONLY here; golden-fixture parity via XCTest on macOS)"
echo "Swift parity is proven in the Apple leg (Tests/WeftTests/FanoutChaosTests.swift" | tee -a "$LOG"
echo "diffs the committed golden fixture byte-for-byte) — declared, not silent." | tee -a "$LOG"

step "6. Golden fixture = the C oracle, reproduced here and now"
if ./core/c/fanout-chaos stepped 200000 4 4 2 200 200 1337 2>/dev/null | \
   diff -q - tools/chaos-fixtures/stepped-golden-200k.json >/dev/null; then
  echo "golden fixture reproduced byte-for-byte" | tee -a "$LOG"
else
  echo "golden fixture DIVERGED from the C oracle — regenerate or investigate" | tee -a "$LOG"
  fail=1
fi

if [ "$fail" -eq 0 ]; then
  echo '{"shard":"chaos-parity","status":"PASSED","gates":"C==TS(+JVM+Dart when present) byte-identical on 2 configs + golden fixture"}' > "$RESULTS"
  echo "chaos-parity shard: PASS" | tee -a "$LOG"
else
  echo '{"shard":"chaos-parity","status":"FAILED"}' > "$RESULTS"
  echo "chaos-parity shard: RED" | tee -a "$LOG"
  exit 1
fi
