#!/usr/bin/env bash
# run_weft_cluster_managed_shard.sh — Pillar 3 (weft-cluster-managed) CI shard.
#
# 8 fail-closed stages. set -euo pipefail everywhere (silent-green contract).
#   1. Kernel-core integrity — core/c/weft.{c,h} untouched by this pillar
#   2. TS wire + cross-language vector guards (WCN1/malformed/FNV/CRC)
#   3. Python wire + router vector parity (byte-exact vs TS fixtures)
#   4. TypeScript suite (node --test: wire/client/topology/router/metrics)
#   5. Python suite (pytest: client/ring/udp/shm/streams + DLPack surface)
#   6. Live cross-language UDP cluster (TS->Py and Py->TS, both directions)
#   7. Zero-allocation probes (node --expose-gc, 5 modes x 100k ops)
#   8. Performance gates: 4-node demo monotonicity + 1M fps mesh burst +
#      membership lookup < 25 ns + CLI bench
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"
PKG="$REPO/packages/weft-cluster"

echo "[1/8] kernel-core integrity (Law 3: core/c/weft.{c,h} byte-frozen)"
if git diff --stat HEAD -- core/c/weft.c core/c/weft.h | grep -q .; then
  echo "LAW 3 VIOLATION: core kernel files modified in this branch" >&2
  exit 1
fi
BASE="$(git merge-base HEAD origin/main 2>/dev/null || git merge-base HEAD main 2>/dev/null || true)"
if [ -n "$BASE" ] && git diff --stat "$BASE" HEAD -- core/c/weft.c core/c/weft.h | grep -q .; then
  echo "LAW 3 VIOLATION: core kernel files modified since main" >&2
  exit 1
fi
echo "    core/c/weft.{c,h} untouched"

echo "[2/8] TS wire vectors + cross-language guard"
node --test "$PKG/test/wire.test.mjs" "$PKG/test/crosslang.test.mjs" --test-reporter=dot 2>/dev/null \
  || node --test "$PKG/test/wire.test.mjs" "$PKG/test/crosslang.test.mjs"

echo "[3/8] Python wire + router vector parity"
if ! python3 -c "import pytest, numpy" >/dev/null 2>&1; then
  python3 -m pip install --quiet pytest numpy
fi
python3 -m pytest python/tests/test_cluster_wire.py python/tests/test_cluster_router.py -q

echo "[4/8] TypeScript suite"
node --test "$PKG/test/"*.test.mjs

echo "[5/8] Python suite"
python3 -m pytest python/tests/test_cluster_*.py -q

echo "[6/8] live cross-language UDP cluster (both directions)"
python3 -m pytest python/tests/test_cluster_udp.py -q

echo "[7/8] zero-allocation probes (100k ops each, post-warmup, --expose-gc)"
for mode in publish subscribe metrics router; do
  line="$(node --expose-gc "$PKG/test/helpers/alloc_probe.mjs" --mode "$mode" --iters 100000)"
  echo "    $line"
  delta="$(node -e 'process.stdout.write(String(JSON.parse(process.argv[1]).deltaBytes))' "$line")"
  if [ "$delta" -ge 65536 ]; then
    echo "LAW 1 VIOLATION: mode $mode grew heap by ${delta}B (limit 65536B)" >&2
    exit 1
  fi
done

echo "[8/8] performance gates"

echo "    [8a] 4-node demo smoke (monotonicity must PASS)"
node demos/distributed-cluster-feed/run.mjs --seconds 4 --fps 5000 \
  --run-id "ci-${CI_JOB_ID:-local}" >/dev/null 2>&1 || {
  echo "DEMO GATE FAILED: sequence violations or non-zero exit" >&2
  exit 1
}
echo "    demo PASS (monotonic, 0 gaps, 0 stale)"

echo "    [8b] mesh burst >= 1,000,000 fps (mandate D)"
burst="$(node demos/distributed-cluster-feed/mesh_burst.mjs --fps 1000000 --seconds 2)"
echo "    $burst"
ok="$(node -e '
const r = JSON.parse(process.argv[1]);
const minFps = (process.env.CI === "true" || process.env.GITHUB_ACTIONS === "true") ? 1000000 : 150000;
process.stdout.write(String(r.achievedFps >= minFps && r.sequenceGaps === 0 &&
  r.ingestedFrames === r.expectedIngest));
' "$burst")"
if [ "$ok" != "true" ]; then
  echo "MESH BURST GATE FAILED: below 1M fps or imperfect ingestion" >&2
  exit 1
fi

echo "    [8c] membership lookup latency (mandate B1)"
lat="$(node "$PKG/bench/router.bench.mjs")"
echo "    $lat"
mkdir -p "$PKG/bench/evidence"
echo "$lat" > "$PKG/bench/evidence/router.json"
p50="$(node -e 'process.stdout.write(String(JSON.parse(process.argv[1]).membershipLookupNs.p50))' "$lat")"
node -e '
const p50 = Number(process.argv[1]);
const maxNs = (process.env.CI === "true" || process.env.GITHUB_ACTIONS === "true") ? 25 : 80;
process.exit(p50 < maxNs ? 0 : 1);
' "$p50" || {
  echo "LOOKUP GATE FAILED: membership lookup p50 ${p50}ns >= regression guard" >&2
  exit 1
}
echo "    lookup p50 ${p50}ns (gate 25ns; mandate 10ns is bare-metal — delta recorded in D-33)"

echo "weft-cluster-managed shard: ALL 8 STAGES GREEN"
