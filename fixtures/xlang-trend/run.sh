#!/usr/bin/env bash
# run.sh — RFC 0020 cross-language trend-verdict parity gate.
#
# The SAME deterministic behind trace (behind = xorshift32(state) % 64,
# seed 0x00C0FFEE) drives the trend estimator in every runtime; each
# prints the packed verdict stream (verdict << 6 | min(skip_n, 63), hex,
# one line). The logs are byte-compared pairwise — any divergence in the
# Q16 arithmetic, the verdict ordering, or the slope guards is a hard
# failure.
#
# Legs follow the per-toolchain honesty pattern (see
# fixtures/xlang-replay/run.sh): C and TS always run when built; Rust and
# the VM emitters (kotlin/swift/dart) run when their toolchain is present
# and are DECLARED skips otherwise.
#
# Requires: node 18+ and ../../packages/core/dist built.
set -euo pipefail
cd "$(dirname "$0")"
TMPDIR_RUN=$(mktemp -d)
trap 'rm -rf "$TMPDIR_RUN"' EXIT

NODE=${NODE:-node}
C_RUNNER=${C_RUNNER:-../../core/c/trend-runner}
RUST_RUNNER=${RUST_RUNNER:-../../core/rust/target/release/trend_xlang}
STEPS=${STEPS:-10000}
SEED=${SEED:-0x00C0FFEE}
fail=0

echo "== RFC 0020 trend parity: env $(uname -m)/$(uname -s), node $($NODE --version), steps=$STEPS seed=$SEED =="

if [ ! -f ../../packages/core/dist/index.js ]; then
  echo "packages/core/dist not built — run: pnpm --filter @weft/core build" >&2
  exit 1
fi

if [ -x "$C_RUNNER" ]; then
  "$C_RUNNER" "$STEPS" "$SEED" > "$TMPDIR_RUN/c.log"
  echo "  C     leg: $(wc -c < "$TMPDIR_RUN/c.log") bytes"
else
  echo "  C     leg: SKIPPED (trend-runner not built — make -C core/c trend-runner)"
fi

$NODE trend_emitter.mjs "$STEPS" "$SEED" > "$TMPDIR_RUN/ts.log"
echo "  TS    leg: $(wc -c < "$TMPDIR_RUN/ts.log") bytes"

if [ -x "$RUST_RUNNER" ]; then
  if "$RUST_RUNNER" "$STEPS" "$SEED" > "$TMPDIR_RUN/rust.log" 2>/dev/null; then
    echo "  Rust  leg: $(wc -c < "$TMPDIR_RUN/rust.log") bytes"
  else
    rm -f "$TMPDIR_RUN/rust.log"
    echo "  Rust  leg: SKIPPED (declared — runner failed)"
  fi
else
  echo "  Rust  leg: SKIPPED (declared — trend_xlang not built)"
fi

KOTLINC=${KOTLINC:-kotlinc}
if command -v "$KOTLINC" > /dev/null 2>&1; then
  if "$KOTLINC" ../../core/kotlin/WeftTrend.kt vm/kotlin/TrendTrace.kt \
    -include-runtime -d "$TMPDIR_RUN/trend-kotlin.jar" > /dev/null 2>&1 && [ -f "$TMPDIR_RUN/trend-kotlin.jar" ]; then
    if java -jar "$TMPDIR_RUN/trend-kotlin.jar" "$STEPS" "$SEED" > "$TMPDIR_RUN/kotlin.log" 2>/dev/null; then
      echo "  Kotlin leg: $(wc -c < "$TMPDIR_RUN/kotlin.log") bytes"
    else
      rm -f "$TMPDIR_RUN/kotlin.log"
      echo "  Kotlin leg: SKIPPED (declared — runtime failed)"
    fi
  else
    echo "  Kotlin leg: SKIPPED (declared — kotlinc build failed)"
  fi
else
  echo "  Kotlin leg: SKIPPED (declared — kotlinc absent)"
fi

if command -v swiftc > /dev/null 2>&1; then
  if swiftc -O ../../core/swift/WeftTrend.swift vm/swift/TrendTrace.swift \
    -o "$TMPDIR_RUN/trend-swift" > /dev/null 2>&1 && [ -x "$TMPDIR_RUN/trend-swift" ]; then
    if "$TMPDIR_RUN/trend-swift" "$STEPS" "$SEED" > "$TMPDIR_RUN/swift.log" 2>/dev/null; then
      echo "  Swift leg: $(wc -c < "$TMPDIR_RUN/swift.log") bytes"
    else
      rm -f "$TMPDIR_RUN/swift.log"
      echo "  Swift leg: SKIPPED (declared — runtime failed)"
    fi
  else
    echo "  Swift leg: SKIPPED (declared — swiftc build failed)"
  fi
else
  echo "  Swift leg: SKIPPED (declared — swiftc absent)"
fi

if command -v dart > /dev/null 2>&1; then
  if dart run vm/dart/trend_trace.dart "$STEPS" "$SEED" > "$TMPDIR_RUN/dart.log" 2>/dev/null; then
    echo "  Dart  leg: $(wc -c < "$TMPDIR_RUN/dart.log") bytes"
  else
    rm -f "$TMPDIR_RUN/dart.log"
    echo "  Dart  leg: SKIPPED (declared — runtime failed)"
  fi
else
  echo "  Dart  leg: SKIPPED (declared — dart absent)"
fi

first=""
for leg in c ts rust kotlin swift dart; do
  f="$TMPDIR_RUN/$leg.log"
  [ -f "$f" ] || continue
  if [ -z "$first" ]; then first="$f"; continue; fi
  if ! cmp -s "$first" "$f"; then
    echo "  DIVERGENCE: $leg disagrees with the reference log"
    fail=1
  fi
done

if [ "$fail" -eq 0 ]; then
  logs=$(ls "$TMPDIR_RUN" | grep '\.log$' | tr '\n' ' ')
  echo "== trend parity: PASS (agreed legs: ${logs}) =="
else
  echo "== trend parity: FAIL ==" >&2
  exit 1
fi
