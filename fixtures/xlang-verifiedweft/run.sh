#!/usr/bin/env bash
# run.sh — cross-language VerifiedWeft interop gate (RFC 0005 byte-compat contract).
#
# Direction 1: C producer (verified-runner gen)   -> TS consumer (reader.mjs)
# Direction 2: TS producer (writer.mjs)           -> C consumer (verified-runner validate)
# Direction 3 (negative): C gen -> single-bit tamper -> C validate MUST reject;
#             the tampered file also MUST be rejected by the TS reader.
#
# All directions validate: file header (magic VWV1, frame count, payload
# length), the 16-byte envelope v1 (seq = record index), every payload word
# against the shared 04-LITMUS §0.1 mix32 generator, and the 32-byte
# HMAC-SHA256 tag per record against the derived auth key. Any mismatch
# exits non-zero.
#
# Requires: node 18+, gcc; ../../core/c/verified-runner built
#           (make -C ../../core/c verified-runner).
#
# The secret is FIXED (deadbeefcafebabe...) so the three kernels' tags land
# in the shared fixture's domain; vectors for the primitive itself live in
# hmac-vectors.json (RFC 4231 + boundary cases, node:crypto cross-checked).
set -euo pipefail
cd "$(dirname "$0")"

NODE=${NODE:-node}
RUNNER=${RUNNER:-../../core/c/verified-runner}
FRAMES=${FRAMES:-5000}
PLEN=${PLEN:-64}
SECRET="deadbeefcafebabe"
fail=0

echo "== xlang VerifiedWeft interop: env $(uname -m)/$(uname -s), node $($NODE --version), frames=$FRAMES, payload=$PLEN =="

echo "-- direction 1: C producer -> TS consumer --"
$RUNNER gen vw-c.bin "$FRAMES" "$PLEN" "$SECRET"
$NODE reader.mjs vw-c.bin "$SECRET" || fail=1

echo
echo "-- direction 2: TS producer -> C consumer --"
$NODE writer.mjs vw-ts.bin "$FRAMES" "$PLEN" "$SECRET"
$RUNNER validate vw-ts.bin "$FRAMES" "$PLEN" "$SECRET" || fail=1

echo
echo "-- direction 3: tamper MUST be rejected by both kernels --"
$RUNNER tamper vw-c.bin vw-c-bad.bin 40
if $RUNNER validate vw-c-bad.bin "$FRAMES" "$PLEN" "$SECRET" 2>/dev/null; then
  echo "C kernel ACCEPTED tampered file — FAIL"
  fail=1
else
  echo "C kernel rejected tampered file — OK"
fi
if $NODE reader.mjs vw-c-bad.bin "$SECRET" 2>/dev/null; then
  echo "TS kernel ACCEPTED tampered file — FAIL"
  fail=1
else
  echo "TS kernel rejected tampered file — OK"
fi

echo
echo "verdict: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
rm -f vw-c.bin vw-c-bad.bin vw-ts.bin
exit $fail
