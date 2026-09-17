#!/usr/bin/env bash
# run_qos_shard.sh — the Series-8 thread-QoS + worklet shard.
#
# 1. qos-test: the flags-not-silence contract (mask sanity, render-preset
#    honesty, RT declared-fallback, affinity apply + readback).
# 2. worklet-test: no-lost-ticks (20k), single-consumer order, QoS flag
#    honesty at spawn, latency distribution, the governed blend loop.
# 3. The Kotlin port proof when kotlinc exists (gradle leg owns it in CI):
#    bigCoreMask abstention + JVM flags + the worklet at 20k ticks.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-qos-worklet.log
: > "$LOG"

fail=0

echo "== qos-worklet shard: env $(uname -m)/$(uname -s) ==" | tee -a "$LOG"

make -s -C core/c qos-test worklet-test || { echo "C build failed" >&2; exit 1; }

echo "-- thread_qos flags --" | tee -a "$LOG"
if (cd core/c && ./qos-test); then :; else fail=1; fi

echo "-- native worklet --" | tee -a "$LOG"
if (cd core/c && ./worklet-test); then :; else fail=1; fi

# Kotlin port proof (toolchain-gated; android-packages owns it in CI)
if command -v kotlinc >/dev/null 2>&1; then
  echo "-- Kotlin QoS/worklet proof --" | tee -a "$LOG"
  PROOF_DIR=$(mktemp -d)
  cat > "$PROOF_DIR/Proof.kt" <<'KOTLIN'
import dev.weft.*
import java.util.concurrent.atomic.AtomicLong

fun main() {
    val mask = RenderQos.bigCoreMask()
    println("bigCoreMask = $mask")
    val flags = RenderQos.applyRenderQos()
    println("applyRenderQos flags = 0x${flags.toString(16)}")
    check(flags and QosFlags.APPLIED_SCHED == 0) { "no android Process claim on plain JVM" }
    val ticks = AtomicLong(0)
    val w = WeftWorklet { t ->
        ticks.incrementAndGet()
        check(t == ticks.get()) { "tick order violation" }
    }
    w.start()
    repeat(20_000) { w.post() }
    while (w.pending() > 0) { }
    check(w.executed == 20_000L) { "lost ticks: ${w.executed}" }
    w.dispose()
    println("kotlin worklet: 20000/20000 ticks")
    println("KOTLIN PROOF GREEN")
}
KOTLIN
  if kotlinc core/kotlin/RenderQos.kt "$PROOF_DIR/Proof.kt" -d "$PROOF_DIR/classes" >/dev/null 2>&1 \
     && java -cp "$PROOF_DIR/classes:$(kotlinc -print-java-home 2>/dev/null)/lib/kotlin-stdlib.jar" ProofKt 2>&1 | tee -a "$LOG"; then
    :
  else
    echo "Kotlin QoS proof RED" >&2
    fail=1
  fi
  rm -rf "$PROOF_DIR"
else
  echo "-- Kotlin QoS proof: DECLARED skip (no kotlinc; android-packages owns it) --" | tee -a "$LOG"
fi

if [ "$fail" -ne 0 ]; then
  echo "❌ qos-worklet shard RED" | tee -a "$LOG" >&2
  exit 1
fi
echo "✅ qos-worklet shard GREEN" | tee -a "$LOG"
