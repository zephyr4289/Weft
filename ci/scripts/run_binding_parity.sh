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
  "core/kotlin/FanoutCompat.kt|android/weft-core/src/main/kotlin/dev/weft/FanoutCompat.kt"
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
  "core/ts/verified.ts|packages/core/src/verified.ts"
  "core/kotlin/Verified.kt|android/weft-core/src/main/kotlin/dev/weft/Verified.kt"
  "core/dart/verified.dart|packages/flutter_weft/lib/src/reference/verified.dart"
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
    h_canon=$(tr -d '\r' < "$canon" | sha256sum | cut -d' ' -f1)
    h_mirror=$(tr -d '\r' < "$mirror" | sha256sum | cut -d' ' -f1)
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

# ---------------------------------------------------------------------------
# HEDDLES <-> PACKAGES parity (Series 7 — the previously unguarded gap).
#
# heddles/* are compatibility SHIMS: each re-exports the canonical surface
# from a packages/* implementation (per RFC-0002 single-source packaging).
# The historic drift class — "the heddles/ fork kept a bug the packaged twin
# had already fixed" — applies to shims differently than to byte mirrors:
# a shim drifts when it re-exports a symbol the canonical package no longer
# exports (or stops re-exporting something it now does). Byte-hash is
# impossible across that boundary; the honest guard is SURFACE parity:
# every export name in the shim must exist in the canonical package's
# source surface, and the shim must name the right canonical package.
# ---------------------------------------------------------------------------

echo "" | tee -a "$LOG"
echo "=== heddles <-> packages surface parity (shim re-exports) ===" | tee -a "$LOG"

# shim|package-dir pairs: the shim's `export {...}` names must all appear
# as exported identifiers in the package's src surface.
SHIM_PAIRS=(
  "heddles/react/WeftCanvas.tsx|packages/react/src/index.ts"
  "heddles/vue/useWeft.ts|packages/vue/src/index.ts"
  "heddles/svelte/weft-action.ts|packages/svelte/src/index.ts"
  "heddles/react-native/weft-rn.ts|packages/react-native/src/index.ts"
)

shim_pass=0
shim_fail=0
shim_cells_json=""

PYTHON="$(command -v python3 || command -v python || echo python3)"

for pair in "${SHIM_PAIRS[@]}"; do
  shim="${pair%%|*}"
  canon="${pair##*|}"
  status="PASS"; detail="surface parity"
  if [ ! -f "$shim" ]; then
    status="FAIL"; detail="shim missing: $shim"
  elif [ ! -f "$canon" ]; then
    status="FAIL"; detail="canonical package surface missing: $canon"
  else
    missing=$("$PYTHON" -c "
import sys, re
shim_src = open(sys.argv[1], encoding='utf-8', errors='ignore').read()
canon_src = open(sys.argv[2], encoding='utf-8', errors='ignore').read()
names = set()
for m in re.finditer(r'export\s+(?:type\s+)?\{([^}]*)\}', shim_src):
    for part in m.group(1).split(','):
        part = part.strip().replace('type ', '')
        if not part:
            continue
        name = part.split(' as ')[-1].strip()
        if name and name not in ('default',):
            names.add(name)
missing = [n for n in sorted(names) if not re.search(r'\b' + re.escape(n) + r'\b', canon_src)]
print(','.join(missing))
" "$shim" "$canon")

    if [ -n "$missing" ]; then
      status="FAIL"
      detail="shim re-exports symbols absent from the canonical surface: $missing"
      {
        echo "---- missing from $canon: $missing ----"
      } >> "$LOG"
    fi
  fi
  {
    echo "[$status] $shim  ->  $canon"
    [ "$status" = "PASS" ] || echo "        reason: $detail"
  } | tee -a "$LOG"

  shim_cells_json+="\"$shim\":{\"status\":\"$status\",\"detail\":\"$detail\"},"
  if [ "$status" = "PASS" ]; then shim_pass=$((shim_pass+1)); else shim_fail=$((shim_fail+1)); fi
done

shim_cells_json="${shim_cells_json%,}"
shim_json="{\"passed\":$shim_pass,\"failed\":$shim_fail,\"total\":${#SHIM_PAIRS[@]},\"cells\":{$shim_cells_json}}"

if [ "$shim_fail" -gt 0 ]; then
  echo ""
  echo "❌ HEDDLES SURFACE PARITY: $shim_fail of ${#SHIM_PAIRS[@]} shims diverged." | tee -a "$LOG"
  echo "   Rule: implement in packages/*, re-export from heddles/* — never fork." | tee -a "$LOG"
  echo "{\"shard\":\"binding-parity\",\"byte_pairs\":{\"passed\":$n_pass,\"failed\":$n_fail,\"total\":${#PAIRS[@]}},\"shim_pairs\":$shim_json}" | tee ci/run-artifacts/binding-parity-results.json
  exit 1
fi
echo "✅ Heddles surface parity: ${#SHIM_PAIRS[@]}/${#SHIM_PAIRS[@]} shims aligned" | tee -a "$LOG"
echo "{\"shard\":\"binding-parity\",\"byte_pairs\":{\"passed\":$n_pass,\"failed\":$n_fail,\"total\":${#PAIRS[@]}},\"shim_pairs\":$shim_json}" | tee ci/run-artifacts/binding-parity-results.json
