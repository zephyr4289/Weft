#!/usr/bin/env bash
# run_binding_parity.sh — hash guard over the deliberate mirror pairs.
#
# WHY EXISTS: RFC-0002 (single-source packaging). Weft keeps ONE canonical
# implementation per language and deliberate build-mirror copies for the
# platform build systems that cannot reach across the tree:
#
#   core/kotlin/*.kt   <-> android/weft-core/src/main/kotlin/dev/weft/*.kt
#   core/dart/*.dart   <-> packages/flutter_weft/lib/src/reference/*.dart
#   core/ts/weft.ts    <-> packages/core/src/index.ts
#
# Every drift incident in the tree's history (the heddles/ forks that kept a
# bug their packaged twin had already fixed) started as an unnoticed edit to
# ONE side of a pair. Byte-identity is the contract; this guard makes any
# unilateral edit RED in CI instead of silent.
#
# Remediation rule: edit the CANONICAL side (core/*), then copy to the mirror.
# The pairs are byte-identical by design — no normalization, no excludes.
#
# Usage: run_binding_parity.sh
# Output: ci/run-artifacts/shard-binding-parity.log + binding-parity-results.json
# Exit:   0 all pairs identical; 1 any drift/missing file.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-binding-parity.log
: > "$LOG"

# pair table: canonical|mirror  (paths relative to ROOT)
PAIRS=(
  "core/kotlin/FrameCursor.kt|android/weft-core/src/main/kotlin/dev/weft/FrameCursor.kt"
  "core/kotlin/Fanout.kt|android/weft-core/src/main/kotlin/dev/weft/Fanout.kt"
  "core/kotlin/Steward.kt|android/weft-core/src/main/kotlin/dev/weft/Steward.kt"
  "core/kotlin/TriadNative.kt|android/weft-core/src/main/kotlin/dev/weft/TriadNative.kt"
  "core/kotlin/Weft.kt|android/weft-core/src/main/kotlin/dev/weft/Weft.kt"
  "core/dart/frame_cursor.dart|packages/flutter_weft/lib/src/reference/frame_cursor.dart"
  "core/dart/fanout.dart|packages/flutter_weft/lib/src/reference/fanout.dart"
  "core/dart/heddle.dart|packages/flutter_weft/lib/src/reference/heddle.dart"
  "core/dart/steward.dart|packages/flutter_weft/lib/src/reference/steward.dart"
  "core/dart/weft.dart|packages/flutter_weft/lib/src/reference/weft.dart"
  "core/ts/weft.ts|packages/core/src/weft.ts"
  "core/ts/fanout.ts|packages/core/src/fanout.ts"
)

n_pass=0
n_fail=0
cells_json=""

for pair in "${PAIRS[@]}"; do
  canon="${pair%%|*}"
  mirror="${pair##*|}"
  status="PASS"; detail="identical"
  if [ ! -f "$canon" ]; then
    status="FAIL"; detail="canonical missing: $canon"
  elif [ ! -f "$mirror" ]; then
    status="FAIL"; detail="mirror missing: $mirror"
  else
    h_canon=$(sha256sum "$canon" | cut -d' ' -f1)
    h_mirror=$(sha256sum "$mirror" | cut -d' ' -f1)
    if [ "$h_canon" != "$h_mirror" ]; then
      status="FAIL"; detail="hash drift ($h_canon != $h_mirror)"
      {
        echo "---- diff $canon <-> $mirror (first 40 lines) ----"
        diff -u "$mirror" "$canon" | head -40 || true
      } >> "$LOG"
    fi
  fi
  {
    echo "[$status] $canon  <->  $mirror"
    [ "$status" = "PASS" ] || echo "        reason: $detail"
  } | tee -a "$LOG"

  cells_json+="\"$canon\":{\"status\":\"$status\",\"detail\":\"$detail\"},"
  if [ "$status" = "PASS" ]; then n_pass=$((n_pass+1)); else n_fail=$((n_fail+1)); fi
done

cells_json="${cells_json%,}"
json="{\"shard\":\"binding-parity\",\"passed\":$n_pass,\"failed\":$n_fail,\"total\":${#PAIRS[@]},\"cells\":{$cells_json}}"
echo "$json" | tee ci/run-artifacts/binding-parity-results.json

if [ "$n_fail" -gt 0 ]; then
  echo ""
  echo "❌ BINDING PARITY DRIFT: $n_fail of ${#PAIRS[@]} pairs diverged." | tee -a "$LOG"
  echo "   Rule: edit the canonical (core/*) side, then copy byte-identical to the mirror." | tee -a "$LOG"
  exit 1
fi
echo "✅ Binding parity: ${#PAIRS[@]}/${#PAIRS[@]} pairs byte-identical" | tee -a "$LOG"
