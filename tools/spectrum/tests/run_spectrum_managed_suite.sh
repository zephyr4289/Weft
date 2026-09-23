#!/usr/bin/env bash
# run_spectrum_managed_suite.sh — Pillar 5 (weft-spectrum) managed suite runner.
#
# 11 fail-closed stages. set -euo pipefail everywhere (silent-green contract).
#   1.  Kernel-core integrity — core/c untouched (Law 3: byte-frozen)
#   2.  SHP1 fixture determinism — golden fixtures byte-identical on re-run
#   3.  @weft/spectrum suite (TS: wire/detect/power/governor, 35 tests)
#   4.  TS zero-allocation probe — 1,000,000 telemetry cycles (< 64 KiB gate)
#       + negative control that MUST bite
#   5.  python/weft_spectrum suite (42 tests incl. tracemalloc 1M-cycle gate
#       + negative control)
#   6.  Dart static audit lane — android/weft_spectrum (no-SDK lane)
#   7.  Swift static audit lane — apple/WeftSpectrum (no-SDK lane)
#   8.  Cross-language parity — TS == Python == frozen manifests (+ audit pin)
#   9.  Multi-tier demo — 240 -> 120 -> 60 -> recovery, 0 dropped frames
#   10. Report integrity — D-53 present with laws map + scorecard
#   11. Patch hygiene — no merge-conflict markers, no stray artifacts
#
# Usage: bash tools/spectrum/tests/run_spectrum_managed_suite.sh

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO"

STAGES_RUN=0
STAGES_FAILED=0

stage() { STAGES_RUN=$((STAGES_RUN + 1)); echo; echo "[$1] $2"; }

fail() {
  STAGES_FAILED=$((STAGES_FAILED + 1))
  echo "    FAIL: $*" >&2
}

pass() { echo "    PASS: $*"; }

# ---------------------------------------------------------------------------
stage "1/11" "kernel-core integrity (Law 3: legacy core/c byte-frozen)"
BASE="$(git merge-base HEAD origin/main 2>/dev/null || git merge-base HEAD main 2>/dev/null || true)"
LEGACY_MODS=""
if [ -n "$BASE" ]; then
  LEGACY_MODS="$(git diff --name-only "$BASE" HEAD -- core/c/src/fanout* core/c/src/sha256* core/c/include/weft_tensor.h 2>/dev/null || true)"
fi
if [ -n "$LEGACY_MODS" ]; then
  fail "legacy core/c modified: $LEGACY_MODS"
else
  pass "legacy core/c/ untouched (byte-frozen)"
fi

# ---------------------------------------------------------------------------
stage "2/11" "SHP1 fixture determinism (double-run byte-identical)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
node tests/spectrum/managed/fixtures/generate.mjs --out "$TMP" >/dev/null
DET_FAIL=0
for f in hw-profile-flagship.bin hw-profile-mid.bin hw-profile-budget.bin \
         hw-profile-torn.bin expected_profile.json governor_vector.json tier_vector.json; do
  if ! cmp -s "tests/spectrum/managed/fixtures/$f" "$TMP/$f"; then
    echo "    determinism drift: $f" >&2
    DET_FAIL=1
  fi
done
if [ "$DET_FAIL" -ne 0 ]; then fail "fixtures not byte-identical"; else pass "7/7 artifacts byte-identical"; fi

# ---------------------------------------------------------------------------
stage "3/11" "@weft/spectrum suite (TypeScript)"
if node --test packages/spectrum-managed/test/*.test.mjs > "$TMP/ts-suite.log" 2>&1; then
  tail -4 "$TMP/ts-suite.log" | sed 's/^/    /'
  pass "TS suite"
else
  tail -20 "$TMP/ts-suite.log" >&2
  fail "TS suite"
fi

# ---------------------------------------------------------------------------
stage "4/11" "TS zero-allocation probe (--expose-gc, 1,000,000 cycles, <= 64 KiB)"
TEL_OUT="$(node --expose-gc packages/spectrum-managed/probes/alloc-probe.mjs telemetry)" || true
echo "    telemetry: $TEL_OUT"
if echo "$TEL_OUT" | grep -q '"ok":true'; then
  pass "telemetry probe within gate"
else
  fail "telemetry probe exceeded 64 KiB gate"
fi
CTRL_OUT="$(node --expose-gc packages/spectrum-managed/probes/alloc-probe.mjs control)" || CTRL_CODE=$?
if [ "${CTRL_CODE:-0}" -eq 2 ]; then
  echo "    control:   $CTRL_OUT (bit as required)"
  pass "negative control bites"
else
  fail "negative control did NOT bite (exit ${CTRL_CODE:-0}) — probe insensitive"
fi

# ---------------------------------------------------------------------------
stage "5/11" "python/weft_spectrum suite (incl. tracemalloc 1M-cycle gate)"
if ! python3 -c "import numpy" >/dev/null 2>&1; then
  python3 -m pip install --quiet numpy || true
fi
if (cd python/weft_spectrum && python3 -m unittest discover -s tests) > "$TMP/py-suite.log" 2>&1; then
  tail -3 "$TMP/py-suite.log" | sed 's/^/    /'
  pass "Python suite"
else
  tail -30 "$TMP/py-suite.log" >&2
  fail "Python suite"
fi

# ---------------------------------------------------------------------------
stage "6/11" "Dart static audit lane (no SDK required)"
if node android/weft_spectrum/audit/static_audit.mjs | tee "$TMP/dart-audit.log" | tail -2 | sed 's/^/    /'; then
  pass "Dart structural audit"
else
  fail "Dart structural audit"
fi

# ---------------------------------------------------------------------------
stage "7/11" "Swift static audit lane (no SDK required)"
if node apple/WeftSpectrum/audit/static_audit.mjs | tee "$TMP/swift-audit.log" | tail -2 | sed 's/^/    /'; then
  pass "Swift structural audit"
else
  fail "Swift structural audit"
fi

# ---------------------------------------------------------------------------
stage "8/11" "cross-language parity (TS == Python == frozen manifests)"
if node tests/spectrum/managed/parity.mjs | tee "$TMP/parity.log" | tail -3 | sed 's/^/    /'; then
  pass "cross-language parity"
else
  fail "cross-language parity"
fi

# ---------------------------------------------------------------------------
stage "9/11" "multi-tier adaptive demo (0 dropped frames mandate)"
if node tests/spectrum/managed/demo/tier-demo.mjs | tee "$TMP/demo.log" | tail -1 | sed 's/^/    /'; then
  pass "multi-tier demo"
else
  fail "multi-tier demo"
fi

# ---------------------------------------------------------------------------
stage "10/11" "report integrity (D-53 with laws map + scorecard)"
REPORT="reports/D-53-SPECTRUM-MANAGED.md"
if [ ! -f "$REPORT" ]; then
  fail "missing $REPORT"
else
  RI_OK=1
  grep -q "Law 1" "$REPORT" || { echo "    missing Law 1 mapping" >&2; RI_OK=0; }
  grep -q "Law 2" "$REPORT" || { echo "    missing Law 2 mapping" >&2; RI_OK=0; }
  grep -q "Law 3" "$REPORT" || { echo "    missing Law 3 mapping" >&2; RI_OK=0; }
  grep -q "Law 4" "$REPORT" || { echo "    missing Law 4 mapping" >&2; RI_OK=0; }
  grep -qi "scorecard" "$REPORT" || { echo "    missing scorecard" >&2; RI_OK=0; }
  grep -qi "zero-allocation" "$REPORT" || { echo "    missing zero-allocation proofs section" >&2; RI_OK=0; }
  grep -qi "re-render" "$REPORT" || { echo "    missing re-render audit section" >&2; RI_OK=0; }
  if [ "$RI_OK" -ne 0 ]; then pass "D-53 structure complete"; else fail "D-53 structure incomplete"; fi
fi

# ---------------------------------------------------------------------------
stage "11/11" "patch hygiene (conflict markers / stray artifacts)"
HYG_OK=1
if grep -rn "^<<<<<<< \|^>>>>>>> " packages/spectrum-managed/src python/weft_spectrum/weft_spectrum \
     android/weft_spectrum/lib apple/WeftSpectrum/Sources 2>/dev/null | grep -q .; then
  echo "    conflict markers found" >&2; HYG_OK=0
fi
if find packages/spectrum-managed python/weft_spectrum android/weft_spectrum apple/WeftSpectrum \
     -name "*.orig" -o -name "*.rej" 2>/dev/null | grep -q .; then
  echo "    stray .orig/.rej artifacts" >&2; HYG_OK=0
fi
if [ "$HYG_OK" -ne 0 ]; then pass "clean tree"; else fail "hygiene violations"; fi

# ---------------------------------------------------------------------------
echo
echo "==============================================="
if [ "$STAGES_FAILED" -ne 0 ]; then
  echo "SPECTRUM MANAGED SUITE: $STAGES_FAILED FAILED / $STAGES_RUN stages"
  exit 1
fi
echo "SPECTRUM MANAGED SUITE: ALL $STAGES_RUN STAGES GREEN"
echo "==============================================="
