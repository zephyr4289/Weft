#!/usr/bin/env bash
# run_weft_tensor_shard.sh — Pillar 2 (weft-tensor) CI shard.
#
# 8 fail-closed stages. set -euo pipefail everywhere (silent-green contract).
#   1. Fixture determinism   — make_ring_fixture.py double-run byte parity
#   2. TypeScript suite      — node --test (layout/ring/fixture/ingest/render/runtime)
#   3. Python suite          — pytest (layout/ring/fixture/dlpack/alloc/ingest)
#   4. TS producer -> Py consumer (cross-language wire parity)
#   5. Py producer -> TS consumer (cross-language wire parity, reverse)
#   6. DLPack alias proof over the TS-produced ring (zero-copy, pointer-checked)
#   7. Zero-allocation probes (node --expose-gc, 5 modes x 100k ops)
#   8. Latency gates (commit p99 < 50us video+audio — the directive's number)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "[1/8] fixture determinism"
python3 scripts/make_ring_fixture.py --verify

echo "[2/8] TypeScript suite"
node --test packages/weft-tensor/test/*.test.mjs

echo "[3/8] Python suite"
python3 -m pytest python/tests -q

echo "[4/8] TS producer -> Python consumer"
node packages/weft-tensor/test/crosslang_produce.mjs "$TMP/ts_ring.bin"
python3 python/tests/crosslang_consume.py "$TMP/ts_ring.bin"

echo "[5/8] Python producer -> TS consumer"
python3 python/tests/crosslang_produce.py "$TMP/py_ring.bin"
node packages/weft-tensor/test/crosslang_consume.mjs "$TMP/py_ring.bin"

echo "[6/8] DLPack alias proof (consumed the TS-produced ring, pointer-checked)"
python3 - "$TMP/ts_ring.bin" <<'PY'
import sys, numpy as np
sys.path.insert(0, "python")
from weft_tensor import WeftRing
data = open(sys.argv[1], "rb").read()   # ONE bytes object — attach AND compare
ring = WeftRing.attach(data)
v = ring.acquire_latest()
t = np.from_dlpack(v)
assert np.shares_memory(t, v.as_numpy()), "DLPack tensor does not alias ring memory"
buf = np.frombuffer(data, dtype=np.uint8)
t_addr = t.__array_interface__["data"][0]
b_addr = buf.__array_interface__["data"][0]
assert 0 <= t_addr - b_addr < len(buf), "tensor pointer outside ring buffer"
print(f"    dlpack alias OK (slot ptr {t_addr:#x} inside ring {b_addr:#x}..{b_addr + len(buf):#x})")
PY

echo "[7/8] zero-allocation probes (100k ops each, post-warmup, --expose-gc)"
for mode in ring ingest audio render overlay; do
  line="$(node --expose-gc packages/weft-tensor/test/helpers/alloc_probe.mjs --mode "$mode" --iters 100000)"
  echo "    $line"
  delta="$(node -e 'process.stdout.write(String(JSON.parse(process.argv[1]).deltaBytes))' "$line")"
  if [ "$delta" -ge 65536 ]; then
    echo "LAW 1 VIOLATION: mode $mode grew heap by ${delta}B (limit 65536B)" >&2
    exit 1
  fi
done

echo "[8/8] latency gates (directive: commit < 50us)"
line="$(node packages/weft-tensor/bench/commit.bench.mjs)"
echo "    $line"
ok="$(node -e '
const r = JSON.parse(process.argv[1]);
process.stdout.write(String(r.video.commit_under_50us && r.audio.commit_under_50us));
' "$line")"
if [ "$ok" != "true" ]; then
  echo "LATENCY GATE FAILED: p99 commit >= 50us" >&2
  exit 1
fi

echo "weft-tensor shard: ALL 8 STAGES GREEN"
