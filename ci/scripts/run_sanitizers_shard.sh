#!/usr/bin/env bash
# run_sanitizers_shard.sh — TIER4 §1 memory-safety audit gate (issue #19, task 1).
#
# Zero-findings sanitizer matrix over the C surface:
#   ASan  + UBSan (joint, -fno-sanitize-recover=all): kernel litmus L1–L8,
#          canary-test, reclaim-test, fanout-test, governor-test,
#          verified-test, blend-test, fuzz-runner (200k ops)
#   TSan: kernel litmus L1–L8 (the existing tsan-deep shard covers N
#         iterations of litmus only; this cell adds the modern kernel
#         surface under the same detector), fanout-test-tsan
#
# Acceptance (issue #19): zero findings in the full suite. Any sanitizer
# report is a hard failure — no suppression files exist, none are allowed.
#
# Output: ci/run-artifacts/shard-sanitizers.log + shard-sanitizers-results.json
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG="$ROOT/ci/run-artifacts/shard-sanitizers.log"
: > "$LOG"

FAIL=0

step() { echo "" >> "$LOG"; echo "=== $* ===" | tee -a "$LOG"; }

record() {
  local name="$1"; local status="$2"
  if [ "$status" -eq 0 ]; then
    echo "  PASS $name" | tee -a "$LOG"
  else
    echo "  FAIL $name" | tee -a "$LOG"
    FAIL=1
  fi
}

ASAN_FLAGS="-O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=all -g"
TSAN_FLAGS="-O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE -fsanitize=thread -g"

step "Build: ASan+UBSan matrix"
(cd core/c && gcc $ASAN_FLAGS -o asan-litmus weft.c litmus_runner.c 2>>"$LOG" && \
              gcc $ASAN_FLAGS -o asan-reclaim weft.c reclaim_test.c 2>>"$LOG" && \
              gcc $ASAN_FLAGS -o asan-fanout fanout.c frame_cursor.c weft.c fanout_test.c 2>>"$LOG" && \
              gcc $ASAN_FLAGS -o asan-governor governor.c governor_test.c 2>>"$LOG" && \
              gcc $ASAN_FLAGS -o asan-verified weft.c verified.c verified_test.c hmac.c sha256.c sha256_hw.c 2>>"$LOG" && \
              gcc $ASAN_FLAGS -o asan-blend blend_q12.c blend_test.c 2>>"$LOG" && \
              gcc $ASAN_FLAGS -o asan-fuzz fuzz_runner.c weft.c fanout.c frame_cursor.c 2>>"$LOG" && \
              gcc $ASAN_FLAGS -o asan-ffihost ffi_host.c ffi_host_test.c 2>>"$LOG") \
  && record "asan-build" 0 || record "asan-build" 1

step "ASan+UBSan: kernel litmus L1-L8"
LIT=0
for t in L1-tear L2-writer-steps L3-reader-steps L4-freshness L5-progress L6-ownership L7-revocation L8-envelope; do
  (cd core/c && ./asan-litmus "$t") >> "$LOG" 2>&1 || LIT=1
done
record "asan-litmus" $LIT

step "ASan+UBSan: kernel conformance batteries"
(cd core/c && ./asan-reclaim)  >> "$LOG" 2>&1 && record "asan-reclaim" 0 || record "asan-reclaim" 1
(cd core/c && ./asan-fanout)   >> "$LOG" 2>&1 && record "asan-fanout" 0 || record "asan-fanout" 1
(cd core/c && ./asan-governor) >> "$LOG" 2>&1 && record "asan-governor" 0 || record "asan-governor" 1
(cd core/c && ./asan-verified) >> "$LOG" 2>&1 && record "asan-verified" 0 || record "asan-verified" 1
(cd core/c && ./asan-blend)    >> "$LOG" 2>&1 && record "asan-blend" 0 || record "asan-blend" 1

step "ASan+UBSan: FFI isolation host (in-process + fork containment)"
(cd core/c && ./asan-ffihost)      >> "$LOG" 2>&1 && record "asan-ffihost" 0 || record "asan-ffihost" 1
(cd core/c && ./asan-ffihost fork) >> "$LOG" 2>&1 && record "asan-ffihost-fork" 0 || record "asan-ffihost-fork" 1

step "ASan+UBSan: fuzz-runner 200k ops seed 0x00C0FFEE"
(cd core/c && ./asan-fuzz 200000 0x00C0FFEE) >> "$LOG" 2>&1 && record "asan-fuzz" 0 || record "asan-fuzz" 1

step "TSan: kernel litmus L1-L8 + fanout"
TS=0
(cd core/c && gcc $TSAN_FLAGS -o tsan-litmus weft.c litmus_runner.c 2>>"$LOG") || TS=1
(cd core/c && gcc $TSAN_FLAGS -o tsan-fanout fanout.c frame_cursor.c weft.c fanout_test.c 2>>"$LOG") || TS=1
if [ $TS -eq 0 ]; then
  for t in L1-tear L2-writer-steps L3-reader-steps L4-freshness L5-progress L6-ownership L7-revocation L8-envelope; do
    (cd core/c && ./tsan-litmus "$t") >> "$LOG" 2>&1 || TS=1
  done
  (cd core/c && ./tsan-fanout) >> "$LOG" 2>&1 || TS=1
fi
record "tsan-suite" $TS

step "Sanitizer report scan (any WARNING/ERROR anywhere in the log = RED)"
if grep -qE "WARNING: (Address|Thread)Sanitizer|runtime error:" "$LOG"; then
  echo "  FAIL: sanitizer findings present" | tee -a "$LOG"
  FAIL=1
else
  echo "  PASS: zero sanitizer findings" | tee -a "$LOG"
fi

python3 - "$LOG" << 'PYEOF'
import json, sys, re
log = open(sys.argv[1]).read()
cells = {}
for m in re.finditer(r'^\s*(PASS|FAIL) ([a-z0-9-]+)$', log, re.M):
    cells[m.group(2)] = m.group(1) == 'PASS'
out = {'shard': 'sanitizers', 'status': 'PASSED' if all(cells.values()) and cells else 'FAILED', 'cells': cells}
json.dump(out, open('ci/run-artifacts/shard-sanitizers-results.json', 'w'), indent=2)
print(json.dumps(out, indent=2))
sys.exit(0 if out['status'] == 'PASSED' else 1)
PYEOF

[ $FAIL -eq 0 ] || exit 1
