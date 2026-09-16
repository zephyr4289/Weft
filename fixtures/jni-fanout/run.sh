#!/usr/bin/env bash
# run.sh — build the host .so and run the JVM torture harness.
#
# Compiles the EXACT sources the Android build compiles (weft_jni.c +
# core/c/weft.c + core/c/fanout.c) against the host JDK's jni.h, then runs
# FanoutJniHarness on the host JVM. Needs: gcc, a JDK (javac+java) with
# include/jni.h. On ubuntu-latest CI runners both are preinstalled.
#
# Usage: bash fixtures/jni-fanout/run.sh    (from the repo root)
# Output: per-check lines + PASS/FAIL summary; evidence log written to
#         litmus/evidence/fanout/jni-harness.log when EVIDENCE=1 (default 1).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
HERE="fixtures/jni-fanout"
EVIDENCE="${EVIDENCE:-1}"
LOG="litmus/evidence/fanout/jni-harness.log"

# Locate a JDK with include/jni.h (JAVA_HOME, then the PATH).
JDK_HOME="${JAVA_HOME:-}"
if [ -z "$JDK_HOME" ] || [ ! -f "$JDK_HOME/include/jni.h" ]; then
  JAVAC_BIN="$(command -v javac || true)"
  if [ -n "$JAVAC_BIN" ]; then
    JDK_HOME="$(cd "$(dirname "$JAVAC_BIN")/.." && pwd)"
  fi
fi
if [ ! -f "$JDK_HOME/include/jni.h" ]; then
  echo "jni-fanout: no JDK with include/jni.h found (set JAVA_HOME) — SKIPPING (loud)." >&2
  exit 1
fi
# Compiler from the JDK; runner from the JDK if present, else the PATH java
# (some distributions split the two across packages — same version classfiles).
JAVAC="$JDK_HOME/bin/javac"
JAVA="$JDK_HOME/bin/java"
if [ ! -x "$JAVA" ]; then JAVA="$(command -v java)"; fi
# Some JDK distributions need their own lib dir on the loader path.
export LD_LIBRARY_PATH="$JDK_HOME/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

mkdir -p "$HERE/build"
SO="$HERE/build/libweft_core.so"

gcc -shared -fPIC -O2 -std=c11 -Wall -D_GNU_SOURCE \
  -I"$ROOT/android/weft-core/src/main/cpp" -I"$ROOT/core/c" \
  -I"$JDK_HOME/include" -I"$JDK_HOME/include/$(uname -s | tr 'A-Z' 'a-z')" \
  "$ROOT/android/weft-core/src/main/cpp/weft_jni.c" \
  "$ROOT/core/c/weft.c" \
  "$ROOT/core/c/fanout.c" \
  -o "$SO"

"$JAVAC" -d "$HERE/build" "$HERE/FanoutJniHarness.java"

if [ "$EVIDENCE" = "1" ]; then
  mkdir -p "$(dirname "$LOG")"
  {
    echo "=== Weft fan-out JNI harness ($(date -u +%Y-%m-%dT%H:%M:%SZ)) ==="
    echo "env: $(uname -sr), gcc $(gcc -dumpversion), $($JAVA -version 2>&1 | head -1)"
    echo "sources: weft_jni.c + core/c/weft.c + core/c/fanout.c (host .so, -O2)"
    echo
    "$JAVA" -Dweft.lib="$SO" -cp "$HERE/build" dev.weft.FanoutJniHarness
    echo
    echo "exit: $?"
  } | tee "$LOG"
  # Re-run exit code through the tee pipeline (pipefail covers gcc/java above).
else
  "$JAVA" -Dweft.lib="$SO" -cp "$HERE/build" dev.weft.FanoutJniHarness
fi
