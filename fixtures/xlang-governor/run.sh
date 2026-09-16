#!/usr/bin/env bash
# run.sh — G5 cross-language governor parity gate (RFC-0009).
#
# The SAME deterministic (behind, now_ms) trace (xorshift32-seeded, the
# repo's canonical 04-LITMUS §0.2 generator) is run through the governor in
# all three languages; each emits the packed action log (one byte per step:
# kind<<6 | min(skip_n, 63), hex-encoded). The three logs are byte-compared
# pairwise — any divergence in the ladder, the skip-n arithmetic, the
# counter-driven cooldown, or the rate-limited Reseed fallback is a hard
# failure.
#
# Requires: node 18+, gcc, cargo; ../../packages/core/dist built
#           (pnpm --filter @weft/core build) and ../../core/c/governor-test
#           + ../../core/rust/target/release/governor_xlang built.
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
echo "-- C emitter --"
$C_DUMP xlang-dump "$STEPS" "$SEED" > trace-c.log
echo "-- Rust emitter --"
$RUST_DUMP "$STEPS" "$SEED" > trace-rust.log

echo "-- byte-compare (TS vs C) --"
if cmp -s trace-ts.log trace-c.log; then
  echo "   identical ($(wc -c < trace-ts.log) bytes)"
else
  echo "   MISMATCH:" >&2
  cmp trace-ts.log trace-c.log >&2 || true
  fail=1
fi

echo "-- byte-compare (TS vs Rust) --"
if cmp -s trace-ts.log trace-rust.log; then
  echo "   identical ($(wc -c < trace-ts.log) bytes)"
else
  echo "   MISMATCH:" >&2
  cmp trace-ts.log trace-rust.log >&2 || true
  fail=1
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

rm -f trace-ts.log trace-c.log trace-rust.log

if [ "$fail" -ne 0 ]; then
  echo "G5: FAIL" >&2
  exit 1
fi
echo "G5: PASS — identical action sequences across TS/C/Rust"
