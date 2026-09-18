#!/usr/bin/env bash
# run.sh — the xlang-blend cross-language parity gate (Series 8).
#
# The C kernel (core/c/blend_q12.c) is the golden source: blend_test
# --digests replays its deterministic (size, alpha) grid and digests every
# output buffer with FNV-1a 64 (golden-vectors.csv, committed). Every
# language port replays the SAME buffers (same xorshift32 pair-fill, same
# seed) through ITS blend implementation and must reproduce the same
# digests byte-for-byte — any divergence in channel order, shift depth,
# rounding, or endianness is a hard failure.
#
# Toolchain policy (the repo's per-port honesty pattern, cf.
# xlang-cadence/run.sh): the C + TS legs are REQUIRED where a C compiler
# exists (the sandbox and CI both have one); each VM verifier runs when
# its toolchain is present and is a DECLARED skip otherwise (in CI,
# android-packages runs the Kotlin leg, flutter-packages the Dart leg,
# apple-packages compiles the Swift port and its XCTest).
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-gcc}
NODE=${NODE:-node}
fail=0
ran=0
declared_skips=0

echo "== xlang-blend golden parity: env $(uname -m)/$(uname -s) =="

# --- 1. C golden selftest (the reference kernel gates itself) -------------
if command -v "$CC" >/dev/null 2>&1; then
  echo "-- C kernel (golden source) --"
  (cd ../../core/c && make -s blend-test) || { echo "   C build failed" >&2; exit 1; }
  if (cd ../../core/c && ./blend-test > /dev/null); then
    echo "   blend-test gates GREEN"
    ran=$((ran + 1))
  else
    echo "   blend-test RED" >&2
    fail=1
  fi
  # The committed golden-vectors.csv must match the kernel's current output
  # (guards against editing one side of the contract). The CSV path is
  # resolved from core/c's perspective (the subshell lives there — an
  # earlier draft used a bare relative path and cmp silently compared
  # against nothing; the drift guard caught its own author).
  if (cd ../../core/c && ./blend-test --digests | grep -E '^[0-9]+,' | cmp -s - ../../fixtures/xlang-blend/golden-vectors.csv); then
    echo "   golden-vectors.csv matches the kernel"
  else
    echo "   GOLDEN DRIFT: committed CSV != kernel output (edit core/c, regen, and update all ports — never the CSV alone)" >&2
    fail=1
  fi
else
  echo "-- C compiler absent: DECLARED skip (golden source unverified here) --"
  declared_skips=$((declared_skips + 1))
fi

# --- 2. TS verifier (reference port) --------------------------------------
if [ ! -f ../../packages/core/dist/index.js ]; then
  echo "packages/core/dist not built — run: (cd packages/core && npx tsup)" >&2
  exit 1
fi
echo "-- TS verifier (reference port) --"
if $NODE blend_verify.mjs golden-vectors.csv; then
  ran=$((ran + 1))
else
  fail=1
fi

# --- 3. Kotlin verifier (kotlinc when present) ----------------------------
if command -v kotlinc >/dev/null 2>&1; then
  echo "-- Kotlin verifier --"
  if kotlinc ../../core/kotlin/BlendQ12.kt kotlin/BlendVerify.kt -include-runtime -d /tmp/blend-verify.jar >/dev/null 2>&1 \
     && java -jar /tmp/blend-verify.jar golden-vectors.csv; then
    ran=$((ran + 1))
  else
    echo "   Kotlin verifier FAILED" >&2
    fail=1
  fi
else
  echo "-- Kotlin verifier: DECLARED skip (no kotlinc; android-packages leg owns it) --"
  declared_skips=$((declared_skips + 1))
fi

# --- 4. Dart verifier (dart when present) ---------------------------------
if command -v dart >/dev/null 2>&1; then
  echo "-- Dart verifier --"
  if dart run dart/BlendVerify.dart golden-vectors.csv; then
    ran=$((ran + 1))
  else
    echo "   Dart verifier FAILED" >&2
    fail=1
  fi
else
  echo "-- Dart verifier: DECLARED skip (no dart; flutter-packages leg owns it) --"
  declared_skips=$((declared_skips + 1))
fi

# --- 5. Swift port: declared skip in the Linux sandbox --------------------
echo "-- Swift port: DECLARED skip (no swiftc; apple-packages compiles + golden-gates it) --"
declared_skips=$((declared_skips + 1))

echo ""
echo "xlang-blend: ran=$ran declared_skips=$declared_skips"
if [ "$fail" -ne 0 ]; then
  echo "❌ XLANG-BLEND PARITY: a port diverged from the C golden digests." >&2
  exit 1
fi
echo "✅ xlang-blend parity: every executed port byte-identical to the C kernel"
