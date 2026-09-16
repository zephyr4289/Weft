#!/usr/bin/env bash
# run.sh — cross-language ring interop gate (RFC-0004 byte-compat contract).
#
# Direction 1: TS producer (@weft/core dist)  -> C consumer  (validate-ring)
# Direction 2: C producer (dump-ring)          -> TS consumer (WeftFanoutReader)
#
# Both directions validate: geometry, handoff accounting (fresh final frame,
# dropped = frames-1), and every payload word's bit pattern against the
# shared 04-LITMUS §0.1-mixer generator. Any mismatch exits non-zero.
#
# Requires: node 18+, gcc; ../../packages/core/dist built
#           (pnpm --filter @weft/core build) and ../../core/c/fanout-runner
#           built (make -C ../../core/c fanout-runner).
set -euo pipefail
cd "$(dirname "$0")"

NODE=${NODE:-node}
RUNNER=${RUNNER:-../../core/c/fanout-runner}
FRAMES=${FRAMES:-5000}
fail=0

echo "== xlang fan-out interop: env $(uname -m)/$(uname -s), node $($NODE --version), frames=$FRAMES =="

echo "-- direction 1: TS producer -> C consumer --"
$NODE writer.mjs ring-ts.bin ring-ts.meta "$FRAMES"
$RUNNER validate-ring ring-ts.bin ring-ts.meta || fail=1

echo
echo "-- direction 2: C producer -> TS consumer --"
$RUNNER dump-ring ring-c.bin ring-c.meta "$FRAMES" 4 64
$NODE reader.mjs ring-c.bin ring-c.meta || fail=1

echo
echo "verdict: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
rm -f ring-ts.bin ring-ts.meta ring-c.bin ring-c.meta
exit $fail
