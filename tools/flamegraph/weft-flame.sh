#!/usr/bin/env bash
# weft-flame.sh — TIER5 §3 flame-graph capture for publish/claim (issue #20, task 3).
#
# Linux (perf):   ./weft-flame.sh linux [SECONDS]  — record + fold + SVG
# macOS (sample): ./weft-flame.sh macos [SECONDS]  — sample + text flame
#
# The workload is the W-suite C backend (B1 short-burst publish/claim) —
# the same cells the perf-regression gate pins, so flame shapes map 1:1 to
# gate numbers. Requires perf + (optional) FlameGraph tools for SVG:
#   https://github.com/brendangregg/FlameGraph  (stackcollapse-perf.pl, flamegraph.pl)
# Without the perl tools the raw perf-script output is kept in
# bench/flame/perf-folded.txt — importable into speedscope / FlameScope.

set -euo pipefail
cd "$(dirname "$0")/../.."
MODE="${1:-linux}"
DUR="${2:-10}"
OUT=bench/flame
mkdir -p "$OUT"

echo "→ Building the perf-friendly kernel + bench (-O2 -g -fno-omit-frame-pointer)"
gcc -O2 -g -fno-omit-frame-pointer -std=c11 -Wall -pthread -D_GNU_SOURCE \
    -o core/c/perf-bench core/c/bench_runner.c core/c/weft.c

case "$MODE" in
  linux)
    command -v perf >/dev/null 2>&1 || { echo "perf not installed (apt install linux-tools-common linux-tools-$(uname -r))" >&2; exit 1; }
    # Frame pointer warnings are noise: the build above already uses -fno-omit-frame-pointer.
    # B1 = the short-burst publish/claim bench (bench_runner's canonical cell)
    perf record -F 997 -g -o "$OUT/perf.data" -- core/c/perf-bench B1 measure_s="$DUR" 2>/dev/null || true
    perf script -i "$OUT/perf.data" > "$OUT/perf-script.txt"
    if [ -x FlameGraph/stackcollapse-perf.pl ] && [ -x FlameGraph/flamegraph.pl ]; then
      perf script -i "$OUT/perf.data" | FlameGraph/stackcollapse-perf.pl > "$OUT/perf-folded.txt"
      FlameGraph/flamegraph.pl --title "Weft publish/claim" --countname samples \
        < "$OUT/perf-folded.txt" > "$OUT/weft-flame.svg"
      echo "→ $OUT/weft-flame.svg"
    else
      echo "→ FlameGraph perl tools absent — folded stacks at $OUT/perf-script.txt"
      echo "  (import into https://www.speedscope.app or install FlameGraph for SVG)"
    fi
    ;;
  macos)
    command -v sample >/dev/null 2>&1 || { echo "sample not found (macOS only)" >&2; exit 1; }
    core/c/perf-bench B1 measure_s="$DUR" &
    BENCH_PID=$!
    sleep 1
    sample "$BENCH_PID" "$((DUR - 1))" -file "$OUT/sample.txt" >/dev/null 2>&1 || true
    wait $BENCH_PID 2>/dev/null || true
    echo "→ $OUT/sample.txt (open in Instruments: File > Import, or read the call tree directly)"
    ;;
  *)
    echo "usage: weft-flame.sh [linux|macos] [SECONDS]" >&2
    exit 1
    ;;
esac
