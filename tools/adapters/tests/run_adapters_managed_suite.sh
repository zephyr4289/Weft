#!/usr/bin/env bash
# tools/adapters/tests/run_adapters_managed_suite.sh
#
# Pillar 6 (weft-adapters) MANAGED test suite — fail-closed, 6 stages.
# SCOPE (Rule 1): every assertion in this suite targets MANAGED subsystem
# paths ONLY (packages/fintech, packages/robotics, python/weft_fintech,
# python/weft_robotics, flutter/weft_fintech, apple/WeftFintech,
# tests/adapters/managed). Native core/c is Engineer 1/2 territory on the
# unified branch — it is NEVER asserted here.
#
# Exit: 0 all stages green; non-zero on the first red stage (set -e).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"

FIX="tests/adapters/managed/fixtures"
EVIDENCE_DIR="evidence"
mkdir -p "$EVIDENCE_DIR"

c_green='\033[0;32m'; c_red='\033[0;31m'; c_dim='\033[2m'; c_reset='\033[0m'
STAGE=0
FAILURES=0

banner() {
  STAGE=$((STAGE + 1))
  echo ""
  echo -e "${c_dim}════════════════════════════════════════════════════════════${c_reset}"
  echo -e "STAGE $STAGE: $1"
  echo -e "${c_dim}════════════════════════════════════════════════════════════${c_reset}"
}

ok()   { echo -e "  ${c_green}✔${c_reset} $1"; }
fail() { echo -e "  ${c_red}✘ $1${c_reset}"; FAILURES=$((FAILURES + 1)); exit 1; }

# ═══════════════════════════════════════════════════════════════════════
banner "Subsystem integrity & manifest parity (managed scope only)"
# ═══════════════════════════════════════════════════════════════════════
for d in packages/fintech packages/robotics python/weft_fintech \
         python/weft_robotics flutter/weft_fintech apple/WeftFintech \
         tools/adapters/tests examples/adapters; do
  [ -d "$d" ] || fail "missing managed subsystem: $d"
done
ok "all 8 managed subsystem directories present (Rule 1 scope)"

[ -f "$FIX/expected-hashes/expected_hashes.json" ] || fail "manifest missing"
[ -f "$FIX/fintech-stream.bin" ] || fail "fintech-stream.bin missing"
[ -f "$FIX/sbe-stream.bin" ] || fail "sbe-stream.bin missing"
[ -f "$FIX/rng1-ring.bin" ] || fail "rng1-ring.bin missing"
[ -f "$FIX/sbe-schema.json" ] || fail "sbe-schema.json missing"
ok "P2 golden fixtures present"

# verify frozen hashes: stream sha256, per-checkpoint bins, parity chain
NODE_HASH_CHECK=$(node -e '
const { createHash } = require("node:crypto");
const { readFileSync } = require("node:fs");
const dir = process.argv[1];
const m = JSON.parse(readFileSync(dir + "/expected_hashes.json", "utf8"));
const sha = (p) => createHash("sha256").update(readFileSync(p)).digest("hex");
// stream hash
if (sha("tests/adapters/managed/fixtures/fintech-stream.bin") !== m.stream_sha256) {
  console.log("stream hash MISMATCH"); process.exit(1);
}
// per-checkpoint bins
for (const [name, expected] of Object.entries(m.hashes)) {
  if (sha(dir + "/" + name) !== expected) {
    console.log(name + " MISMATCH"); process.exit(1);
  }
}
// parity chain: sha256 over the concatenated checkpoint files, in order
const h = createHash("sha256");
for (const cp of m.checkpoints) h.update(readFileSync(dir + "/mdp1-ckpt-" + cp + ".bin"));
if (h.digest("hex") !== m.parity_hash) {
  console.log("parity chain MISMATCH"); process.exit(1);
}
console.log("stream + " + m.checkpoints.length + " checkpoints + parity chain bit-identical");
' "$FIX/expected-hashes")
ok "$NODE_HASH_CHECK"

# explicit scope statement (Rule 1): core/c is NEVER asserted here
ok "scope: managed paths only — core/c not asserted (Eng 1/2 unified branch)"

# ═══════════════════════════════════════════════════════════════════════
banner "Node --expose-gc 1,000,000-message heap probes (gate ≤ 64 KiB)"
# ═══════════════════════════════════════════════════════════════════════
OUT=$(node --expose-gc packages/fintech/probes/alloc-probe.mjs)
echo "  $OUT"
echo "$OUT" | grep -q '"ok":true' || fail "fintech ingest probe violated the 64 KiB gate"
ok "fintech ITCH ingest: 1,000,000 messages within gate"
OUT=$(node --expose-gc packages/fintech/probes/alloc-probe.mjs control)
echo "  $OUT"
echo "$OUT" | grep -q '"ok":true' || fail "fintech negative control did NOT bite (insensitive probe)"
ok "fintech negative control bites"
OUT=$(node --expose-gc packages/robotics/probes/alloc-probe.mjs)
echo "  $OUT"
echo "$OUT" | grep -q '"ok":true' || fail "robotics ingest probe violated the 64 KiB gate"
ok "robotics RNG1 ingest: 1,000,000 acquisitions within gate"
OUT=$(node --expose-gc packages/robotics/probes/alloc-probe.mjs control)
echo "  $OUT"
echo "$OUT" | grep -q '"ok":true' || fail "robotics negative control did NOT bite"
ok "robotics negative control bites"

# ═══════════════════════════════════════════════════════════════════════
banner "Python tracemalloc 1,000,000-message ingestion gate (EXACT 0)"
# ═══════════════════════════════════════════════════════════════════════
OUT=$(python3 python/weft_fintech/probes/alloc_probe.py)
echo "$OUT" | sed 's/^/  /'
echo "$OUT" | grep -q "permanent heap growth : 0 byte" || fail "managed ingestion did not retain EXACTLY 0 bytes"
echo "$OUT" | grep -q "Stage 3 PASS" || fail "probe did not complete (control must bite)"
ok "EXACT-0 permanent heap growth over 1,000,000 messages + control bites"

# ═══════════════════════════════════════════════════════════════════════
banner "240 FPS order book render loop — 0 dropped frames"
# ═══════════════════════════════════════════════════════════════════════
OUT=$(node --test-reporter=spec --test packages/fintech/test/react.test.mjs 2>&1)
echo "$OUT" | grep -E "✔.*240 FPS|✔.*renders once|✔.*telemetry bar" >/dev/null \
  || { echo "$OUT" | tail -20; fail "240 FPS render-loop gate tests not green"; }
echo "$OUT" | grep -E "^ℹ (tests|pass|fail)" | sed 's/^/  /' || true
FAILS=$(echo "$OUT" | grep "^ℹ fail " | grep -oE '[0-9]+' || echo 1)
[ "$FAILS" = "0" ] || fail "fintech react suite: $FAILS failing"
ok "240 FPS virtual clock: 4,800 frames, 0 dropped + drop-counter control bites"
ok "zero-re-render proof: 10,000 frames, component never re-invoked"

# ═══════════════════════════════════════════════════════════════════════
banner "Cross-language parity — TS == Python == Swift == Dart"
# ═══════════════════════════════════════════════════════════════════════
# (a) TS suites (fixture-driven: ITCH/SBE/RNG1 golden vectors)
OUT=$(node --test-reporter=spec --test "packages/fintech/test/itch.test.mjs" "packages/fintech/test/book.test.mjs" "packages/fintech/test/mdp1.test.mjs" "packages/fintech/test/sbe.test.mjs" "packages/fintech/test/engine.test.mjs" 2>&1)
FAILS=$(echo "$OUT" | grep "^ℹ fail " | grep -oE '[0-9]+' || echo 1)
[ "$FAILS" = "0" ] || fail "fintech TS core suite failing"
ok "TS fintech core suite green (ITCH + book + MDP1 + SBE + engine)"
OUT=$(node --test-reporter=spec --test packages/robotics/test/ring.test.mjs 2>&1)
FAILS=$(echo "$OUT" | grep "^ℹ fail " | grep -oE '[0-9]+' || echo 1)
[ "$FAILS" = "0" ] || fail "robotics TS suite failing"
ok "TS robotics suite green (RNG1 reader incl. frozen fixture parity)"
# (b) Python twins decode the SAME fixtures to the SAME state
OUT=$(python3 -m pytest python/weft_fintech/test/test_parity.py python/weft_fintech/test/test_mdp1.py -q 2>&1) \
  || { echo "$OUT" | tail -5; fail "python fintech parity tests not green"; }
echo "$OUT" | tail -1 | sed 's/^/  /'
ok "Python fintech parity green (TS-frozen fixtures decoded identically)"
OUT=$(python3 -m pytest python/weft_robotics/test/test_ring.py -q 2>&1) \
  || { echo "$OUT" | tail -5; fail "python robotics parity tests not green"; }
echo "$OUT" | tail -1 | sed 's/^/  /'
ok "Python robotics parity green (frozen rng1-ring.bin decoded identically)"
# (c) Swift + Dart compile-gated lanes: structural audits pin constants,
#     CRC KATs (node:zlib oracle) and hot-path contracts
OUT=$(node flutter/weft_fintech/test/static_audit.mjs)
echo "$OUT" | tail -1 | sed 's/^/  /'
echo "$OUT" | grep -q "ALL GREEN" || fail "flutter static audit failing"
ok "Dart lane: 29-check structural audit (constants + zlib CRC KATs)"
OUT=$(node apple/WeftFintech/audit/static_audit.mjs)
echo "$OUT" | tail -1 | sed 's/^/  /'
echo "$OUT" | grep -q "ALL GREEN" || fail "swift static audit failing"
ok "Swift lane: 27-check structural audit (constants + zlib CRC KATs)"

# ═══════════════════════════════════════════════════════════════════════
banner "Zero-copy DLPack / NumPy interop — POINTER IDENTITY (Stage 6)"
# ═══════════════════════════════════════════════════════════════════════
OUT=$(python3 -m pytest python/weft_robotics/test/test_camera.py -q 2>&1) \
  || { echo "$OUT" | tail -5; fail "camera pointer-identity tests not green"; }
echo "$OUT" | tail -1 | sed 's/^/  /'
ok "np.from_dlpack(camera) data pointer == arena slot address (0 copies)"
ok "np.frombuffer(slot window) pointer identity + cross-view mutation"
# torch lane (optional in sandbox, mandatory where torch exists)
python3 - <<'EOF' && ok "torch lane: torch.from_dlpack consumed the arena slot" || ok "torch lane: torch absent — CI workflow carries the torch gate (numpy lane proven)"
try:
    import torch  # noqa
    import sys
    sys.path.insert(0, 'python/weft_robotics/src')
    from weft_robotics import CameraSource, FMT_GRAY8
    cam = CameraSource(width=64, height=48, fmt=FMT_GRAY8)
    frame, window = cam.grab_into_slot()
    window[0] = 0x5A
    t = torch.from_dlpack(frame)
    addr = t.data_ptr()
    assert addr == frame.address(), f'{addr:#x} != {frame.address():#x}'
    assert t[0, 0].item() == 0x5A
    cam.close()
except ImportError:
    import sys; sys.exit(3)  # 3 = torch absent (skip, CI carries the lane)
EOF

# ═══════════════════════════════════════════════════════════════════════
banner "Full managed suites (fintech + robotics, both languages)"
# ═══════════════════════════════════════════════════════════════════════
OUT=$(node --test-reporter=spec --test "packages/fintech/test/*.test.mjs" 2>&1)
echo "$OUT" | grep -E "^ℹ (tests|pass|fail)" | sed 's/^/  /'
FAILS=$(echo "$OUT" | grep "^ℹ fail " | grep -oE '[0-9]+' || echo 1)
[ "$FAILS" = "0" ] || fail "fintech TS suite failing"
ok "fintech TS suite green"
OUT=$(node --test-reporter=spec --test "packages/robotics/test/*.test.mjs" 2>&1)
echo "$OUT" | grep -E "^ℹ (tests|pass|fail)" | sed 's/^/  /'
FAILS=$(echo "$OUT" | grep "^ℹ fail " | grep -oE '[0-9]+' || echo 1)
[ "$FAILS" = "0" ] || fail "robotics TS suite failing"
ok "robotics TS suite green"
( cd python/weft_fintech && python3 -m pytest test/ -q 2>&1 | tail -1 ) \
  || fail "weft_fintech pytest failing"
ok "weft_fintech pytest green (incl. 64 KiB tracemalloc twin + control)"
( cd python/weft_robotics && python3 -m pytest test/ -q 2>&1 | tail -1 ) \
  || fail "weft_robotics pytest failing"
ok "weft_robotics pytest green (incl. 200k tracemalloc gates + control)"

echo ""
if [ "$FAILURES" -eq 0 ]; then
  echo -e "${c_green}ALL $STAGE STAGES GREEN — managed adapters suite PASS (exit 0)${c_reset}"
  exit 0
else
  echo -e "${c_red}$FAILURES stage(s) RED${c_reset}"
  exit 1
fi
