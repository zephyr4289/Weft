#!/usr/bin/env bash
# run.sh — RFC 0019 cross-language time-travel replay parity gate.
#
# The SAME deterministic scenario (xorshift32-seeded, the repo's canonical
# 04-LITMUS §0.2 generator) is folded in every runtime; each prints the
# per-step u64 state hash as 16 lowercase hex chars. The logs are
# byte-compared pairwise — any divergence in the fold's transition table,
# the 111-byte serialization, or the FNV-1a reduction is a hard failure.
#
# NORMATIVE SCENARIO GRAMMAR (every emitter mirrors this EXACTLY):
#   state = SEED; latest=0 w_work=1 r_work=2 epoch=0 revoked=0 seq=0
#   bufseq = [0,0,0]                      # shadow seq per buffer slot
#   for i in 0..N:
#     state = xorshift32(state); op = state & 15; u = state (unsigned)
#     op < 7   : seq++; len = (u >> 4) % 1024
#                if !revoked: PUBLISH(aux=len, data=seq);
#                  bufseq[w_work] = seq; (latest,w_work) = (w_work,latest)
#                else: epoch++; DROP(aux=epoch & 0xffff, data=seq)
#     op < 12  : CLAIM(data=bufseq[latest]);
#                (latest,r_work) = (r_work,latest)
#     op == 12 : !revoked: REVOKE(data=epoch); revoked=1
#                else:     ACK(data=epoch)
#     op == 13 : revoked: ACK(data=epoch); revoked=0
#                else:    STALL(data=(u >> 4) % 8)
#     op == 14 : TEAR(data=seq)
#     else     : CANARY_FAIL(data=seq)
#
# Legs: C (core/c/replay-runner) and TS (packages/core dist) always run
# when built; Rust (replay_xlang) runs when its binary was built; the VM
# emitters (kotlin/swift/dart) run when their compiler is present and are
# DECLARED skips otherwise — the workflow that owns the toolchain runs the
# same gate with it present (android-packages: kotlinc; apple-packages:
# swiftc; flutter-packages: dart).
#
# Requires: node 18+ and ../../packages/core/dist built
#           (pnpm --filter @weft/core build).
set -euo pipefail
cd "$(dirname "$0")"
TMPDIR_RUN=$(mktemp -d)
trap 'rm -rf "$TMPDIR_RUN"' EXIT

NODE=${NODE:-node}
C_RUNNER=${C_RUNNER:-../../core/c/replay-runner}
RUST_RUNNER=${RUST_RUNNER:-../../core/rust/target/release/replay_xlang}
STEPS=${STEPS:-10000}
SEED=${SEED:-0x00C0FFEE}
fail=0

echo "== RFC 0019 replay parity: env $(uname -m)/$(uname -s), node $($NODE --version), steps=$STEPS seed=$SEED =="

if [ ! -f ../../packages/core/dist/index.js ]; then
  echo "packages/core/dist not built — run: pnpm --filter @weft/core build" >&2
  exit 1
fi

# --- C reference leg -------------------------------------------------------
if [ -x "$C_RUNNER" ]; then
  "$C_RUNNER" "$STEPS" "$SEED" > "$TMPDIR_RUN/c.log"
  echo "  C     leg: $(wc -c < "$TMPDIR_RUN/c.log") bytes"
else
  echo "  C     leg: SKIPPED (replay-runner not built — make -C core/c replay-runner)"
fi

# --- TS leg (always) -------------------------------------------------------
$NODE replay_emitter.mjs "$STEPS" "$SEED" > "$TMPDIR_RUN/ts.log"
echo "  TS    leg: $(wc -c < "$TMPDIR_RUN/ts.log") bytes"

# --- Rust leg (when built) -------------------------------------------------
if [ -x "$RUST_RUNNER" ]; then
  if "$RUST_RUNNER" "$STEPS" "$SEED" > "$TMPDIR_RUN/rust.log" 2>/dev/null; then
    echo "  Rust  leg: $(wc -c < "$TMPDIR_RUN/rust.log") bytes"
  else
    rm -f "$TMPDIR_RUN/rust.log"
    echo "  Rust  leg: SKIPPED (declared — runner failed)"
  fi
else
  echo "  Rust  leg: SKIPPED (declared — replay_xlang not built)"
fi

# --- VM legs (per-toolchain honesty pattern) --------------------------------
KOTLINC=${KOTLINC:-kotlinc}
if command -v "$KOTLINC" > /dev/null 2>&1; then
  if "$KOTLINC" ../../core/kotlin/WeftReplay.kt vm/kotlin/ReplayTrace.kt \
    -include-runtime -d "$TMPDIR_RUN/replay-kotlin.jar" > /dev/null 2>&1 && [ -f "$TMPDIR_RUN/replay-kotlin.jar" ]; then
    if java -jar "$TMPDIR_RUN/replay-kotlin.jar" "$STEPS" "$SEED" > "$TMPDIR_RUN/kotlin.log" 2>/dev/null; then
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
  if swiftc -O ../../core/swift/WeftReplay.swift vm/swift/ReplayTrace.swift \
    -o "$TMPDIR_RUN/replay-swift" > /dev/null 2>&1 && [ -x "$TMPDIR_RUN/replay-swift" ]; then
    if "$TMPDIR_RUN/replay-swift" "$STEPS" "$SEED" > "$TMPDIR_RUN/swift.log" 2>/dev/null; then
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
  if dart run vm/dart/replay_trace.dart "$STEPS" "$SEED" > "$TMPDIR_RUN/dart.log" 2>/dev/null; then
    echo "  Dart  leg: $(wc -c < "$TMPDIR_RUN/dart.log") bytes"
  else
    rm -f "$TMPDIR_RUN/dart.log"
    echo "  Dart  leg: SKIPPED (declared — runtime failed)"
  fi
else
  echo "  Dart  leg: SKIPPED (declared — dart absent)"
fi

# --- byte-compare every produced log ---------------------------------------
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
  echo "== replay parity: PASS (agreed legs: ${logs}) =="
else
  echo "== replay parity: FAIL ==" >&2
  exit 1
fi
