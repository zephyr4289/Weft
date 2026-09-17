#!/usr/bin/env bash
# run.sh — G5 cross-language governor parity gate (RFC-0009).
#
# The SAME deterministic (behind, now_ms) trace (xorshift32-seeded, the
# repo's canonical 04-LITMUS §0.2 generator) is run through the governor in
# every language; each emits the packed action log (one byte per step:
# kind<<6 | min(skip_n, 63), hex-encoded). The logs are byte-compared
# pairwise — any divergence in the ladder, the skip-n arithmetic, the
# counter-driven cooldown, or the rate-limited Reseed fallback is a hard
# failure.
#
# G5-VM: the three VM emitters (vm/kotlin, vm/swift, vm/dart — Series 7)
# join the comparison best-effort per toolchain (the repo's per-port
# honesty pattern): each runs when its compiler is present and is a
# DECLARED skip otherwise — the workflow that owns the toolchain runs the
# same gate with it present (android-packages: kotlinc; flutter-packages:
# dart; apple-packages: swiftc). The C/Rust legs run when their binaries
# were built (the fanout-native shard builds them before calling).
#
# Requires: node 18+ and ../../packages/core/dist built
#           (pnpm --filter @weft/core build).
set -euo pipefail
cd "$(dirname "$0")"

NODE=${NODE:-node}
C_DUMP=${C_DUMP:-../../core/c/governor-test}
RUST_DUMP=${RUST_DUMP:-../../core/rust/target/release/governor_xlang}
STEPS=${STEPS:-10000}
SEED=${SEED:-0x00C0FFEE}
fail=0

echo "== G5 governor trace parity: env $(uname -m)/$(uname -s), node $($NODE --version), steps=$STEPS seed=$SEED =="

if [ ! -f ../../packages/core/dist/index.js ]; then
  echo "packages/core/dist not built — run: pnpm --filter @weft/core build" >&2
  exit 1
fi

echo "-- TS emitter --"
$NODE gov_trace.mjs "$STEPS" "$SEED" > trace-ts.log

if [ -x "$C_DUMP" ] || command -v "$C_DUMP" >/dev/null 2>&1; then
  echo "-- C emitter --"
  "$C_DUMP" xlang-dump "$STEPS" "$SEED" > trace-c.log
  echo "-- byte-compare (TS vs C) --"
  if cmp -s trace-ts.log trace-c.log; then
    echo "   identical ($(wc -c < trace-ts.log) bytes)"
  else
    echo "   MISMATCH:" >&2
    cmp trace-ts.log trace-c.log >&2 || true
    fail=1
  fi
else
  echo "-- C emitter -- SKIPPED (binary not built: make -C core/c governor-test; the fanout-native shard builds it)"
fi

if [ -x "$RUST_DUMP" ]; then
  echo "-- Rust emitter --"
  "$RUST_DUMP" "$STEPS" "$SEED" > trace-rust.log
  echo "-- byte-compare (TS vs Rust) --"
  if cmp -s trace-ts.log trace-rust.log; then
    echo "   identical ($(wc -c < trace-ts.log) bytes)"
  else
    echo "   MISMATCH:" >&2
    cmp trace-ts.log trace-rust.log >&2 || true
    fail=1
  fi
else
  echo "-- Rust emitter -- SKIPPED (binary not built: cargo build --release --bin governor_xlang; the fanout-native shard builds it)"
fi

# --- G5-VM: the VM ladder emitters, best-effort per toolchain ---
emit_compare_vm() { # $1 = label, $2.. = command words
  local label="$1"; shift
  echo "-- byte-compare (TS vs $label) --"
  if "$@" > trace-vm.log 2>/dev/null; then
    if cmp -s trace-ts.log trace-vm.log; then
      echo "   identical ($(wc -c < trace-ts.log) bytes)"
    else
      echo "   MISMATCH:" >&2
      cmp trace-ts.log trace-vm.log >&2 || true
      fail=1
    fi
  else
    echo "   emitter failed" >&2
    fail=1
  fi
}

echo "-- VM ladder emitters (G5-VM) --"
if command -v kotlinc >/dev/null 2>&1 && command -v java >/dev/null 2>&1; then
  KOTLIN_JAR=$(mktemp -d)/gov-trace-kotlin.jar
  if kotlinc ../../core/kotlin/Governor.kt vm/kotlin/GovernorTrace.kt \
      -include-runtime -d "$KOTLIN_JAR" >/dev/null 2>&1; then
    emit_compare_vm Kotlin java -jar "$KOTLIN_JAR" "$STEPS" "$SEED"
  else
    echo "   Kotlin: compile FAILED" >&2
    fail=1
  fi
else
  echo "   Kotlin: SKIPPED (no kotlinc+java on this runner; android-packages CI covers)"
fi
if command -v swiftc >/dev/null 2>&1; then
  SWIFT_BIN=$(mktemp -d)/gov-trace-swift
  if swiftc -O ../../core/swift/Governor.swift vm/swift/GovernorTrace.swift \
      -o "$SWIFT_BIN" >/dev/null 2>&1; then
    emit_compare_vm Swift "$SWIFT_BIN" "$STEPS" "$SEED"
  else
    echo "   Swift: compile FAILED" >&2
    fail=1
  fi
else
  echo "   Swift: SKIPPED (no swiftc on this runner; apple-packages CI covers)"
fi
if command -v dart >/dev/null 2>&1; then
  emit_compare_vm Dart dart vm/dart/GovernorTrace.dart "$STEPS" "$SEED"
else
  echo "   Dart: SKIPPED (no dart on this runner; flutter-packages CI covers)"
fi

# Sanity: the trace must actually exercise every action class (FastPath=0,
# Skip=1, Snapshot=2, Reseed=3) — otherwise the comparison proves nothing.
echo "-- action-class coverage --"
HEX=$(head -c 200000 trace-ts.log)
COV=$(node -e "
const hex = process.argv[1];
const seen = new Set();
for (let i = 0; i + 1 < hex.length; i += 2) {
  const b = parseInt(hex.slice(i, i + 2), 16);
  seen.add(b >> 6);
}
const missing = [0, 1, 2, 3].filter((k) => !seen.has(k));
if (missing.length) { console.error('missing classes: ' + missing.join(',')); process.exit(1); }
console.log('all four action classes exercised');
" "$HEX") || fail=1
echo "   $COV"

rm -f trace-ts.log trace-c.log trace-rust.log trace-vm.log

if [ "$fail" -ne 0 ]; then
  echo "G5: FAIL" >&2
  exit 1
fi
echo "G5: PASS — identical action sequences across every emitter present (TS/C/Rust + VM)"
