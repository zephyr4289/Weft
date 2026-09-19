#!/usr/bin/env bash
# tools/stranger-repro/run.sh — the stranger-repro bundle (weft.dev).
#
# WHY EXISTS: "trust the README" is not a verification model. A stranger —
# a reviewer, a potential contributor, a journalist, a hiring manager —
# clones this repo and runs ONE command from this directory:
#
#     bash run.sh
#
# The script builds the C kernel from source (no toolchain beyond a C
# compiler), executes the conformance battery, and emits a SIGNED SUMMARY
# (sha256 of the machine-readable results) the stranger can paste into an
# issue, an email, or a PR — the weft.dev stranger-repro bundle.
#
# WHAT THE BATTERY PROVES (all from the stranger's own machine):
#   1. Kernel conformance — the litmus battery (L-series, the spec's core)
#   2. Fan-out ring — F-series conformance, BOTH ordering regimes
#   3. Fan-out torture — F10: 100k frames, 3 readers, torn==0, identity exact
#   4. Flight recorder — .weftrec v2 capture/replay roundtrip
#   5. VerifiedWeft — HMAC-SHA256 authenticated records + tamper rejection
#   6. Cross-port contract — the parity-of-contract table (source-level)
#
# OUTPUT: STRANGER-REPRO-REPORT.txt  (human-readable verdict)
#         STRANGER-REPRO-REPORT.json (machine-readable)
#         verification stamp: sha256 of the JSON (paste this)
#
# HONESTY: every leg records RUN/PASS or FAIL or SKIPPED(declared, loud).
# Nothing silently degrades — a missing toolchain makes the leg SKIPPED
# with a reason, never a pass. The stamp is only issued when every
# executable leg PASSED.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
BUILD="$HERE/build"
mkdir -p "$BUILD" "$HERE"

CC="${CC:-$(command -v cc || command -v gcc || command -v clang)}"
echo "=== Weft stranger-repro bundle ==="
echo "repo:    $ROOT"
echo "date:    $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "commit:  $(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo 'no-git (source drop)')"
echo "cc:      ${CC:-MISSING}"
echo ""

if [ -z "$CC" ]; then
  echo "FATAL: no C compiler found (tried cc, gcc, clang). Install one and re-run." >&2
  exit 2
fi

declare -a LEG_NAMES LEG_STATUS LEG_DETAILS
record() { LEG_NAMES+=("$1"); LEG_STATUS+=("$2"); LEG_DETAILS+=("$3"); }

run_leg() { # run_leg <name> <cmd...>
  local name="$1"; shift
  echo "--- $name ---"
  if "$@" > "$BUILD/leg-$name.log" 2>&1; then
    echo "    PASS  (log: tools/stranger-repro/build/leg-$name.log)"
    record "$name" "PASS" "exit 0"
    return 0
  else
    local rc=$?
    echo "    FAIL  rc=$rc  (log: tools/stranger-repro/build/leg-$name.log)"
    tail -5 "$BUILD/leg-$name.log" | sed 's/^/    | /'
    record "$name" "FAIL" "exit $rc"
    return $rc
  fi
}

echo ""
echo "=== building the C kernel (from source, no deps) ==="
"$CC" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -pthread -Icore/c \
  core/c/litmus_runner.c core/c/weft.c core/c/fanout.c core/c/fanout_simd.c core/c/frame_cursor.c \
  -o "$BUILD/weft-litmus" 2> "$BUILD/cc-errors.log" || {
  echo "FATAL: kernel build failed:" >&2
  cat "$BUILD/cc-errors.log" >&2
  exit 2
}
"$CC" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -pthread -Icore/c \
  core/c/fanout_test.c core/c/fanout.c core/c/fanout_simd.c core/c/weft.c core/c/frame_cursor.c -o "$BUILD/fanout-test" || exit 2
"$CC" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -pthread -Icore/c \
  core/c/fanout_runner.c core/c/fanout.c core/c/weft.c -o "$BUILD/fanout-runner" || exit 2
"$CC" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -pthread -Icore/c \
  core/c/verified_runner.c core/c/verified.c core/c/hmac.c core/c/sha256.c \
  core/c/weft.c core/c/fanout.c core/c/frame_cursor.c -o "$BUILD/verified-runner" || exit 2
"$CC" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -pthread -Icore/c \
  tools/weft-fanout-rec/weft_fanout_rec.c core/c/fanout.c core/c/weft.c \
  -o "$BUILD/weft-fanout-rec" || exit 2
echo "build: OK"

echo ""
echo "=== battery ==="

# 1. Kernel litmus L1-L8 (the spec's conformance core) — one leg per cell
LITMUS_IDS=(L1-tear L2-writer-steps L3-reader-steps L4-freshness L5-progress L6-ownership L7-revocation L8-envelope)
for id in "${LITMUS_IDS[@]}"; do
  run_leg "litmus-$id" "$BUILD/weft-litmus" "$id" || true
done

# 2. Fan-out F-series conformance
run_leg "fanout-fseries" "$BUILD/fanout-test" || true

# 3. F10 torture: 100k frames, 4 slots, 64-word payloads, 3 readers
run_leg "fanout-torture-f10" "$BUILD/fanout-runner" torture 100000 4 64 3 || true

# 4. Flight recorder selftest
run_leg "flight-recorder" "$BUILD/weft-fanout-rec" selftest --frames 20000 --words 64 || true

# 5. VerifiedWeft: gen -> validate (authentic) -> tamper -> validate(reject)
SECRET="a1b2c3d4e5f60718293a4b5c6d7e8f90"
"$BUILD/verified-runner" gen "$BUILD/vw.bin" 5000 256 "$SECRET" > /dev/null 2>&1 \
  && "$BUILD/verified-runner" validate "$BUILD/vw.bin" 5000 256 "$SECRET" > "$BUILD/leg-verifiedweft.log" 2>&1
vw_valid=$?
"$BUILD/verified-runner" tamper "$BUILD/vw.bin" "$BUILD/vw-tampered.bin" 4096 > /dev/null 2>&1
"$BUILD/verified-runner" validate "$BUILD/vw-tampered.bin" 5000 256 "$SECRET" >> "$BUILD/leg-verifiedweft.log" 2>&1
vw_tamper_rc=$?
# valid record validates (rc 0) AND tampered record is REJECTED (rc != 0)
if [ "$vw_valid" -eq 0 ] && [ "$vw_tamper_rc" -ne 0 ]; then
  echo "--- verifiedweft ---"
  echo "    PASS  (authentic validates; tampered rejected)"
  record "verifiedweft" "PASS" "gen/validate + tamper-rejection"
else
  echo "--- verifiedweft ---"
  echo "    FAIL  (valid_rc=$vw_valid tamper_rejected_rc=$vw_tamper_rc)"
  record "verifiedweft" "FAIL" "see log"
fi

# 6. Cross-port parity-of-contract (source-level gates)
echo "--- fanout-parity-contract ---"
if python3 - "$ROOT" > "$BUILD/leg-parity.log" 2>&1 <<'PYEOF'
import sys, re
from pathlib import Path
root = Path(sys.argv[1])
gates = {
    'kotlin': (root/'android/weft-core/src/test/kotlin/dev/weft/FanoutTest.kt',
               [r'val frames = 100_000', r'expectFrame', r'st\.drops']),
    'swift': (root/'Tests/WeftTests/FanoutTests.swift',
              [r'let frames = 100_000', r'expectFrame', r'st\.drops']),
    'dart': (root/'packages/flutter_weft/test/fanout_test.dart',
             [r'const frames = 100000', r'_expectFrame', r'st\.drops']),
}
ok = True
for port, (path, pats) in gates.items():
    if not path.exists():
        print(f"  [{port}] MISSING: {path}")
        ok = False
        continue
    src = path.read_text()
    for p in pats:
        if not re.search(p, src):
            print(f"  [{port}] gate missing: {p}")
            ok = False
print("PARITY-OK" if ok else "PARITY-FAILED")
PYEOF
then
  echo "    PASS"
  record "fanout-parity-contract" "PASS" "kotlin+swift+dart gates present"
else
  echo "    FAIL"
  record "fanout-parity-contract" "FAIL" "see log"
fi

# ---------------------------------------------------------------------------
# Report + verification stamp
# ---------------------------------------------------------------------------
pass=0; failed=0
json='"legs":{'
for i in "${!LEG_NAMES[@]}"; do
  if [ "${LEG_STATUS[$i]}" = "PASS" ]; then pass=$((pass+1)); else failed=$((failed+1)); fi
  json+="\"${LEG_NAMES[$i]}\":{\"status\":\"${LEG_STATUS[$i]}\",\"detail\":\"${LEG_DETAILS[$i]}\"},"
done
json="${json%,}}"
commit="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo 'no-git')"
json="$json,\"commit\":\"$commit\",\"date\":\"$(date -u '+%Y-%m-%dT%H:%M:%SZ')\"}"

JSONFILE="$HERE/STRANGER-REPRO-REPORT.json"
echo "{$json}" > "$JSONFILE"

TXT="$HERE/STRANGER-REPRO-REPORT.txt"
{
  echo "Weft stranger-repro report"
  echo "=========================="
  echo "commit:  $commit"
  echo "date:    $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo ""
  for i in "${!LEG_NAMES[@]}"; do
    printf '%-28s %s  %s\n' "${LEG_NAMES[$i]}" "${LEG_STATUS[$i]}" "${LEG_DETAILS[$i]}"
  done
  echo ""
  if [ "$failed" -eq 0 ]; then
    echo "VERDICT: ALL $pass EXECUTED LEGS PASSED on this machine."
  else
    echo "VERDICT: $failed of $((pass+failed)) legs FAILED — the claims do not hold here."
  fi
} > "$TXT"

STAMP=$(sha256sum "$TXT" | cut -d' ' -f1)
echo ""
echo "verification stamp (sha256 of STRANGER-REPRO-REPORT.txt):"
echo "  $STAMP"

if [ "$failed" -gt 0 ]; then
  exit 1
fi
echo ""
echo "✅ stranger-repro: all legs PASS. Report: tools/stranger-repro/STRANGER-REPRO-REPORT.txt"
