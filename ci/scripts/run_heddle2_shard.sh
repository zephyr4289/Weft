#!/usr/bin/env bash
# run_heddle2_shard.sh — Pillar 4 (heddle-2.0) CI shard.
#
# 8 fail-closed stages. set -euo pipefail everywhere (silent-green contract).
#   1. Kernel-core integrity — core/c/weft.{c,h} + core/c/tensor untouched
#   2. HPL1 fixture determinism — golden fixtures byte-identical on re-run
#   3. heddle-core suite (layout/plane/producer/scheduler/fixtures, 47 tests)
#   4. react-heddle suite (hooks/canvas/visualizers/hud, 28 tests incl.
#      100k-frame zero-setState mandate probe)
#   5. Native-source structural audits — flutter-heddle (35 checks) +
#      swift-heddle (22 checks): constants parity, Endian.little / .littleEndian
#      scans, hot-path purity, wiring contracts
#   6. Zero-allocation probes — producer/consumer/scheduler x 100k ops
#      (< 32 KiB gate) + negative control MUST bite
#   7. Flight demo — 240 Hz frame lock + 100k ticks/sec + GC mandate +
#      0-re-render probe (demos/trading-terminal-240fps)
#   8. Report integrity — D-43 present with laws map + scorecard
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

echo "[1/8] kernel-core integrity (Law 3: core/c byte-frozen)"
if git diff --stat HEAD -- core/c/ | grep -q .; then
  echo "LAW 3 VIOLATION: core/c modified in this branch" >&2
  exit 1
fi
BASE="$(git merge-base HEAD origin/main 2>/dev/null || git merge-base HEAD main 2>/dev/null || true)"
if [ -n "$BASE" ] && git diff --stat "$BASE" HEAD -- core/c/ | grep -q .; then
  echo "LAW 3 VIOLATION: core/c modified since main" >&2
  exit 1
fi
echo "    core/c/ untouched"

echo "[2/8] HPL1 fixture determinism (double-run byte-identical)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
node fixtures/heddle2/generate.mjs --out "$TMP" >/dev/null
for f in hpl1-basic-4x8 hpl1-edge-1x2; do
  if ! cmp -s "fixtures/heddle2/$f.bin" "$TMP/$f.bin"; then
    echo "DETERMINISM FAILURE: $f.bin differs from committed golden fixture" >&2
    exit 1
  fi
done
echo "    fixtures byte-identical"

echo "[3/8] heddle-core suite"
node --test packages/heddle-core/test/*.test.mjs 2>&1 | tail -4

echo "[4/8] react-heddle suite (zero-re-render binding layer)"
node --test packages/react-heddle/test/*.test.mjs 2>&1 | tail -4

echo "[5/8] native-source structural audits (Dart + Swift, no-SDK lanes)"
node packages/flutter-heddle/test/static_audit.mjs | tail -2
node packages/swift-heddle/audit/static_audit.mjs | tail -2

echo "[6/8] zero-allocation probes (--expose-gc, 100k ops, < 32 KiB gate)"
for mode in producer consumer scheduler; do
  OUT="$(node --expose-gc packages/heddle-core/probes/alloc-probe.mjs "$mode")"
  echo "    $OUT"
  echo "$OUT" | grep -q '"pass":true'
done
CTRL_OUT="$(node --expose-gc packages/heddle-core/probes/alloc-probe.mjs control)"
echo "    $CTRL_OUT"
echo "$CTRL_OUT" | grep -q '"pass":true' # control MUST bite (probe validity)

echo "[7/8] flight demo — trading-terminal-240fps mandates"
node demos/trading-terminal-240fps/run.mjs | tee /tmp/heddle2-flight.log | grep -E "^\[|PASS|FAIL|NOTE" || true
grep -q "FLIGHT PASS" /tmp/heddle2-flight.log || { echo "FLIGHT DEMO FAILED" >&2; exit 1; }

echo "[8/8] report integrity"
[ -f reports/D-43-HEDDLE2-MANAGED.md ] || { echo "missing reports/D-43-HEDDLE2-MANAGED.md" >&2; exit 1; }
grep -q "Law 1" reports/D-43-HEDDLE2-MANAGED.md && \
grep -q "Law 2" reports/D-43-HEDDLE2-MANAGED.md && \
grep -q "Law 3" reports/D-43-HEDDLE2-MANAGED.md && \
grep -q "Law 4" reports/D-43-HEDDLE2-MANAGED.md || { echo "D-43 missing laws map" >&2; exit 1; }
grep -q "Scorecard" reports/D-43-HEDDLE2-MANAGED.md || { echo "D-43 missing scorecard" >&2; exit 1; }
echo "    D-43 report + laws map + scorecard present"

echo ""
echo "heddle2 shard: ALL 8 STAGES GREEN"
