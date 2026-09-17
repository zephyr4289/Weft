#!/usr/bin/env bash
# run.sh — PC3 cross-language cadence-policy parity gate (RFC-0009 §cadence).
#
# The SAME deterministic arrival trace (xorshift32-seeded, the repo's
# canonical 04-LITMUS §0.2 generator) is run through ALL THREE cadence
# policies in every VM port; each emitter emits the packed decision log
# (per tick, per policy in kind order: b1 = present<<7 | interp<<6 |
# alphaQ12>>7, b2 = min(coalesced,255); hex-encoded, one trailing
# newline). Every emitted log is byte-compared against the TS reference —
# any divergence in the alpha ladder, the elision key, the gap EWMA, the
# reassess hysteresis, or the Law-4 counters is a hard failure.
#
# Toolchain policy (the repo's per-port honesty pattern, cf. the
# verifiedweft shard): the TS reference is REQUIRED (node + built dist);
# each VM emitter runs when its toolchain is present and is a DECLARED
# skip otherwise — in CI, the workflow that owns the toolchain runs the
# same gate with it present (android-packages: kotlinc; flutter-packages:
# dart; apple-packages: swiftc).
#
# Requires: node 18+ and ../../packages/core/dist built
#           (pnpm --filter @weft/core build).
set -euo pipefail
cd "$(dirname "$0")"

NODE=${NODE:-node}
STEPS=${STEPS:-10000}
SEED=${SEED:-0x00C0FFEE}
fail=0
ran_vm=0

echo "== PC3 cadence trace parity: env $(uname -m)/$(uname -s), node $($NODE --version), steps=$STEPS seed=$SEED =="

if [ ! -f ../../packages/core/dist/index.js ]; then
  echo "packages/core/dist not built — run: pnpm --filter @weft/core build" >&2
  exit 1
fi

echo "-- TS emitter (reference) --"
$NODE cadence_trace.mjs "$STEPS" "$SEED" > /tmp/pc3-ts.log

emit_compare() { # $1 = label, $2.. = command words
  local label="$1"; shift
  echo "-- $label emitter --"
  if "$@" > /tmp/pc3-vm.log 2>/tmp/pc3-err.log; then
    if cmp -s /tmp/pc3-ts.log /tmp/pc3-vm.log; then
      echo "   identical ($(wc -c < /tmp/pc3-ts.log) bytes)"
      ran_vm=$((ran_vm + 1))
    else
      echo "   MISMATCH:" >&2
      cmp /tmp/pc3-ts.log /tmp/pc3-vm.log >&2 || true
      fail=1
    fi
  else
    echo "   emitter failed:" >&2
    cat /tmp/pc3-err.log >&2
    fail=1
  fi
}

# --- Kotlin (kotlinc when present; the fixture is a two-file invocation) ---
if command -v kotlinc >/dev/null 2>&1 && command -v java >/dev/null 2>&1; then
  KOTLIN_JAR=$(mktemp -d)/cad-trace-kotlin.jar
  if kotlinc ../../core/kotlin/Governor.kt kotlin/CadenceTrace.kt \
      -include-runtime -d "$KOTLIN_JAR" >/tmp/pc3-err.log 2>&1; then
    emit_compare Kotlin java -jar "$KOTLIN_JAR" "$STEPS" "$SEED"
  else
    echo "-- Kotlin emitter --"
    echo "   kotlinc compile failed:" >&2
    cat /tmp/pc3-err.log >&2
    fail=1
  fi
else
  echo "-- Kotlin emitter -- SKIPPED (no kotlinc+java on this runner; android-packages CI covers)"
fi

# --- Swift (swiftc when present) ---
if command -v swiftc >/dev/null 2>&1; then
  SWIFT_BIN=$(mktemp -d)/cad-trace-swift
  if swiftc -O ../../core/swift/Governor.swift swift/CadenceTrace.swift \
      -o "$SWIFT_BIN" >/tmp/pc3-err.log 2>&1; then
    emit_compare Swift "$SWIFT_BIN" "$STEPS" "$SEED"
  else
    echo "-- Swift emitter --"
    echo "   swiftc compile failed:" >&2
    cat /tmp/pc3-err.log >&2
    fail=1
  fi
else
  echo "-- Swift emitter -- SKIPPED (no swiftc on this runner; apple-packages CI covers)"
fi

# --- Dart (dart when present; the emitter is a zero-dependency script) ---
if command -v dart >/dev/null 2>&1; then
  emit_compare Dart dart dart/CadenceTrace.dart "$STEPS" "$SEED"
else
  echo "-- Dart emitter -- SKIPPED (no dart on this runner; flutter-packages CI covers)"
fi

# Sanity: the trace must actually PRESENT in all three policies and
# exercise both coalescing and interpolation — otherwise the comparison
# proves nothing.
echo "-- policy coverage --"
COV=$($NODE -e "
const hex = require('fs').readFileSync('/tmp/pc3-ts.log', 'utf8').trim();
// 6 bytes per tick (3 policies x 2 bytes), hex-encoded = 12 chars/tick.
if (hex.length % 12 !== 0) { console.error('not 3 policies x 2 bytes per tick'); process.exit(1); }
const presented = [false, false, false];
let coalesced = 0, interp = 0;
for (let i = 0; i < hex.length; i += 12) {
  for (let p = 0; p < 3; p++) {
    const b1 = parseInt(hex.slice(i + p * 4, i + p * 4 + 2), 16);
    const b2 = parseInt(hex.slice(i + p * 4 + 2, i + p * 4 + 4), 16);
    if (b1 & 0x80) presented[p] = true;
    if (b1 & 0x40) interp++;
    coalesced += b2;
  }
}
const missing = [0, 1, 2].filter((p) => !presented[p]);
if (missing.length) { console.error('policies that never presented: ' + missing.join(',')); process.exit(1); }
console.log('all three policies presented; ticks=' + (hex.length / 12) +
  ' interpPacked=' + interp + ' coalescedPacked=' + coalesced);
") || fail=1
echo "   $COV"

if [ "$fail" -ne 0 ]; then
  echo "PC3: FAIL" >&2
  exit 1
fi
echo "PC3: PASS — identical cadence decision sequences (TS + $ran_vm VM emitters present)"
