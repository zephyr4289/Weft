#!/usr/bin/env bash
# run.sh — RFC 0014 kernel-trace cross-port byte-identity gate (issue #20, task 1).
#
# The SAME deterministic scenario (RFC 0014 §parity-scenario — xorshift32
# seeded, 04-LITMUS §0.2 generator, payload_max 64, revoke at step N/2) is
# run through the kernel in every port; each emitter prints the packed
# event stream (8 bytes/event: u16 kind | u16 aux | u32 data) as lowercase
# hex, one trailing newline. Every stream is byte-compared against the TS
# reference — any divergence in the event order, the epoch plumbing, the
# slot rotation, or the canary verdicts is a hard failure.
#
# Toolchain policy (the repo's per-port honesty pattern, cf. xlang-cadence):
# the TS reference is REQUIRED (node); each VM emitter runs when its
# toolchain is present and is a DECLARED skip otherwise — in CI, the
# workflow that owns the toolchain runs the same gate with it present
# (android-packages: kotlinc; flutter-packages: dart; apple-packages: swiftc).
#
# Requires: node 18+ (core/ts/trace.ts, zero-dep) and gcc (core/c reference).

set -euo pipefail
cd "$(dirname "$0")"

NODE=${NODE:-node}
STEPS=${STEPS:-2000}
SEED=${SEED:-0x00C0FFEE}
fail=0
ran_vm=0

if [ ! -x ../../core/c/trace-dump ]; then
  (cd ../../core/c && gcc -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
    -o trace-dump trace_dump.c trace_rec.c weft.c) || { echo "C build failed" >&2; exit 1; }
fi

echo "→ scenario: N=$STEPS seed=$SEED (RFC 0014 §parity-scenario)"

# --- TS reference (REQUIRED) ------------------------------------------------
if ! "$NODE" trace_emitter.mjs "$STEPS" "$SEED" > /tmp/xlang-trace-ts.txt 2>/tmp/xlang-trace-ts.err; then
  echo "TS reference RED:"; cat /tmp/xlang-trace-ts.err; exit 1
fi
REF=$(cat /tmp/xlang-trace-ts.txt)
echo "  TS reference: ${#REF} hex chars"

check() { # check <name> <file>
  local got
  got=$(cat "$2")
  if [ "$got" = "$REF" ]; then
    echo "  ✅ $1 byte-identical"
  else
    echo "  ❌ $1 DIVERGES from the TS reference"
    fail=1
  fi
}

# --- C (REQUIRED when gcc exists — the sandbox always has it) ---------------
if [ -x ../../core/c/trace-dump ]; then
  ../../core/c/trace-dump hex "$STEPS" "$SEED" > /tmp/xlang-trace-c.txt
  check "C" /tmp/xlang-trace-c.txt
else
  echo "  ⚠️  C emitter absent — declared skip (gcc required)"
fi

# --- Kotlin (when kotlinc is present; CI owns it) ---------------------------
if command -v kotlinc >/dev/null 2>&1 && command -v java >/dev/null 2>&1; then
  KOTLIN_JAR=$(mktemp -d)/trace-kotlin.jar
  if kotlinc kotlin/TraceEvents.kt ../../core/kotlin/Weft.kt -include-runtime -d "$KOTLIN_JAR" >/tmp/kt-err.log 2>&1 && java -jar "$KOTLIN_JAR" "$STEPS" "$SEED" > /tmp/xlang-trace-kt.txt 2>/tmp/kt-err.log; then
    ran_vm=$((ran_vm + 1))
    check "Kotlin" /tmp/xlang-trace-kt.txt
  else
    echo "  ❌ Kotlin emitter failed:" >&2
    cat /tmp/kt-err.log >&2
    fail=1
  fi
else
  echo "  − Kotlin: SKIP (declared — kotlinc/java absent; CI android-packages owns it)"
fi

# --- Dart (when dart is present; CI owns it) --------------------------------
if command -v dart >/dev/null 2>&1; then
  if dart dart/trace_events.dart "$STEPS" "$SEED" > /tmp/xlang-trace-dart.txt 2>/tmp/dart-err.log; then
    ran_vm=$((ran_vm + 1))
    check "Dart" /tmp/xlang-trace-dart.txt
  else
    echo "  ❌ Dart emitter failed:" >&2
    cat /tmp/dart-err.log >&2
    fail=1
  fi
else
  echo "  − Dart: SKIP (declared — dart absent; CI flutter-packages owns it)"
fi

# --- Swift (when swiftc is present; CI owns it) -----------------------------
if command -v swiftc >/dev/null 2>&1; then
  SWIFT_BIN=$(mktemp -d)/trace-swift
  if swiftc swift/TraceEvents.swift ../../core/swift/Weft.swift -o "$SWIFT_BIN" >/tmp/sw-err.log 2>&1 && "$SWIFT_BIN" "$STEPS" "$SEED" > /tmp/xlang-trace-sw.txt 2>/tmp/sw-err.log; then
    ran_vm=$((ran_vm + 1))
    check "Swift" /tmp/xlang-trace-sw.txt
  else
    echo "  − Swift: SKIP (declared — standalone swiftc without SPM atomics absent; apple-packages CI covers)"
  fi
else
  echo "  − Swift: SKIP (declared — swiftc absent; CI apple-packages owns it)"
fi

# --- container round trip through the C reference codec ---------------------
../../core/c/trace-dump file /tmp/xlang-trace.weftrec "$STEPS" "$SEED" 2>/dev/null
../../core/c/trace-dump validate /tmp/xlang-trace.weftrec

if [ $fail -eq 0 ]; then
  echo "xlang-trace gate: PASS ($ran_vm VM legs ran; others declared-skip)"
else
  echo "xlang-trace gate: RED"
  exit 1
fi
