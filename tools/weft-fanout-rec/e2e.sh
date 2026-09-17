#!/usr/bin/env bash
# e2e.sh — RFC-0010 end-to-end demo gate: the capture -> validate -> replay
# -> recapture loop the lead's roadmap named, under .weftrec v3 compression.
#
# Road (every step is one of the tool's own modes — no external producer):
#   1. produce   wave-family frames into shm ring A (the REAL-signal shape
#                compression targets) and mixer-family frames into ring C
#                (the pseudorandom shape the codec honestly declines)
#   2. capture --compress  drains ring A -> file A (v3, dzv records)
#   3. validate  file A: CRC + accounting + telescoping + wave bit-exact
#                (decompression transparent: payloads are wire-identical)
#   4. replay    file A -> shm ring B (fresh, O_EXCL)
#   5. capture --compress  drains ring B -> file B (the recapture leg)
#   6. validate  file B + compare payloads A vs B (content-faithful loop)
#   7. mixer leg: capture ring C --compress -> stored fallback measured
#   8. version gates: v2 tooling shape (flags check) rejects v3 headers;
#                     v2 files still validate (backward read compat)
#
# Any failure exits non-zero (fail-fast: set -e).

set -euo pipefail
cd "$(dirname "$0")"

TOOL=./weft-fanout-rec
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

PAYLOAD=256
SLOTS=4
FRAMES=20000
HZ=3000   # throttled so capture keeps up (a real concurrent session)
RING_A=weft_e2e_ring_a
RING_B=weft_e2e_ring_b
RING_C=weft_e2e_ring_c

echo "== 1+2. concurrent session: produce wave -> ring A while capture --compress runs =="
# The honest e2e shape: capture attaches BEFORE the producer finishes (the
# ring holds only $SLOTS slots — a post-hoc capture would see 1 claim and
# a 19999-drop accounting, which is honest but not the session under test).
"$TOOL" produce --shm "$RING_A" --payload "$PAYLOAD" --slots "$SLOTS" \
    --frames "$FRAMES" --hz "$HZ" --family wave &
PROD_A_PID=$!
sleep 0.3
"$TOOL" capture "$OUT/a.weftrec" --shm "$RING_A" --payload "$PAYLOAD" \
    --slots "$SLOTS" --max-secs 30 --idle-ms 1500 --compress
wait "$PROD_A_PID"

echo "== 3. validate: file A (wave bit-exact through decompression) =="
"$TOOL" validate "$OUT/a.weftrec" --expect-wave

echo "== 4+5. replay (throttled) -> ring B while capture --compress recaptures =="
# Concurrent, like the capture leg: an unthrottled replay finishes before
# the recapture attaches, and the ring holds only $SLOTS slots — the honest
# capture would then record 1 claim + (K-1) drops (late arrival, not the
# loop under test). --hz matches the producer pace.
"$TOOL" replay "$OUT/a.weftrec" --shm "$RING_B" --payload "$PAYLOAD" \
    --slots "$SLOTS" --hz "$HZ" &
REPLAY_PID=$!
sleep 0.3
"$TOOL" capture "$OUT/b.weftrec" --shm "$RING_B" --payload "$PAYLOAD" \
    --slots "$SLOTS" --max-secs 30 --idle-ms 1500 --compress
wait "$REPLAY_PID"

echo "== 6. validate + compare: file B structural + payload-exact vs file A =="
# NOTE (declared boundary, same as the v2 selftest): replay is content-
# faithful, NOT seq-faithful — the ring renumbers frames 1..K, so the
# recaptured file's wave words do not match wave_word(new_seq). The content
# check is the payload-exact comparison below (the selftest's discipline).
"$TOOL" validate "$OUT/b.weftrec"
# Payload-exact comparison via cmp on a normalized dump: replay is
# content-faithful, so record payloads must match pairwise; compare via
# the tool's own validate output + python record walk (no new C surface).
python3 - "$OUT/a.weftrec" "$OUT/b.weftrec" << 'PY'
import struct, sys, zlib

def records(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'WREC' and struct.unpack_from('<H', b, 4)[0] == 3
    n = struct.unpack_from('<I', b, 16)[0]
    off = 32
    out = []
    def u32(p): return struct.unpack_from('<I', b, p)[0]
    while off + 36 <= len(b) and len(out) < (n or 1 << 30):
        rec_len = u32(off)
        if rec_len < 36 or off + rec_len > len(b): break
        codec, = struct.unpack_from('<H', b, off + 6)
        seq, dropped = struct.unpack_from('<QQ', b, off + 8)
        plen, slen = struct.unpack_from('<II', b, off + 24)
        stream = b[off + 32: off + 32 + slen]
        assert zlib.crc32(b[off + 4: off + 32 + slen]) == u32(off + rec_len - 4), 'crc'
        if codec == 1:
            words = []
            prev = 0
            i = 0
            while i < len(stream):
                z = 0; shift = 0
                while True:
                    byte = stream[i]; i += 1
                    z |= (byte & 0x7f) << shift
                    if not byte & 0x80: break
                    shift += 7
                d = (z >> 1) ^ -(z & 1)
                prev = (prev + d) & 0xffffffff
                words.append(prev)
            payload = struct.pack('<%dI' % len(words), *words)
        else:
            payload = stream
        assert len(payload) == plen, 'payload length'
        out.append((seq, dropped, payload))
        off += rec_len
    return out

a = records(sys.argv[1])
b = records(sys.argv[2])
# The recapture is ANOTHER honest consumer: it may have dropped frames the
# replay published (its own --idle/pace window). Content-faithfulness means
# B's claimed payloads are an ordered SUBSEQUENCE of A's (replay preserves
# order; recapture skips whole frames, never reorders or alters them).
ai = 0
matched = 0
for bi, (sb, db, pb) in enumerate(b):
    while ai < len(a) and a[ai][2] != pb:
        ai += 1
    if ai >= len(a):
        raise AssertionError(f'record {bi}: payload not found in order in A')
    matched += 1
    ai += 1
print(f'compare: {matched}/{len(b)} recaptured records are an ordered '
      f'payload-exact subsequence of {len(a)} (content-faithful loop closed; '
)

# Compression accounting (evidence for RFC-0010's falsifiable claim):
raw = sum(32 + len(p) + 4 for _, _, p in a)
import os
v3 = os.path.getsize(sys.argv[1])
v2_equiv = raw
print(f'sizes: v3 file {v3} B vs v2-equivalent {v2_equiv} B '
      f'-> ratio {v2_equiv / v3:.2f}x (wave family, dzv)')
PY

echo "== 7. mixer leg: concurrent capture; the codec honestly declines =="
"$TOOL" produce --shm "$RING_C" --payload "$PAYLOAD" --slots "$SLOTS" \
    --frames "$FRAMES" --hz "$HZ" --family mixer &
PROD_C_PID=$!
sleep 0.3
"$TOOL" capture "$OUT/c.weftrec" --shm "$RING_C" --payload "$PAYLOAD" \
    --slots "$SLOTS" --max-secs 30 --idle-ms 1500 --compress
wait "$PROD_C_PID"
"$TOOL" validate "$OUT/c.weftrec" --expect-mixer
python3 - "$OUT/c.weftrec" << 'PY'
import struct, sys, os
b = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('<I', b, 16)[0]
off = 32
dzv = stored = 0
def u32(p): return struct.unpack_from('<I', b, p)[0]
while off + 36 <= len(b):
    rec_len = u32(off)
    if rec_len < 36 or off + rec_len > len(b): break
    codec, = struct.unpack_from('<H', b, off + 6)
    if codec == 1: dzv += 1
    else: stored += 1
    off += rec_len
print(f'mixer capture under --compress: {dzv} dzv records, {stored} stored '
      f'(self-limiting: pseudorandom payloads are emitted verbatim)')
PY

echo "== 8. version gates =="
# v3 header rejected by the v2 flags contract (what v2-only tooling does):
# a v3 header carries FANOUT|COMPRESSED; the v2 rule is flags == FANOUT.
python3 - "$OUT/a.weftrec" << 'PY'
import struct, sys, zlib
b = bytearray(open(sys.argv[1], 'rb').read())
struct.pack_into('<H', b, 4, 2)  # claim v2 while keeping v3 flags
# The header CRC covers bytes 0..20 (including the version field), so a
# faithful forgery patches it too — otherwise the tool dies at the CRC
# check and never reaches the flags contract under test.
struct.pack_into('<I', b, 20, zlib.crc32(bytes(b[:20])))
open('/tmp/weft_e2e_v2shaped.weftrec', 'wb').write(bytes(b))
print('forged v2-shaped header (version 2 + compressed flags + patched CRC)')
PY
v2gate_out="$("$TOOL" validate /tmp/weft_e2e_v2shaped.weftrec 2>&1 || true)"
if echo "$v2gate_out" | grep -q "v2 flags must be exactly FANOUT"; then
    echo "v2 flags contract rejects v3-shaped flags — OK"
else
    echo "FAIL: v2 flags contract did not reject: $v2gate_out" >&2
    exit 1
fi
rm -f /tmp/weft_e2e_v2shaped.weftrec

# v2 files still validate (backward read compat: the selftest is the full
# v2 e2e; here one plain v2 capture round-trips):
"$TOOL" produce --shm "$RING_C" --payload "$PAYLOAD" --slots "$SLOTS" \
    --frames 2000 --hz 2000 --family mixer >/dev/null &
PROD_D_PID=$!
sleep 0.3
"$TOOL" capture "$OUT/plain.weftrec" --shm "$RING_C" --payload "$PAYLOAD" \
    --slots "$SLOTS" --max-secs 15 --idle-ms 1500
wait "$PROD_D_PID"
"$TOOL" validate "$OUT/plain.weftrec" --expect-mixer

# cleanup rings
python3 - << 'PY'
import ctypes, ctypes.util
libc = ctypes.CDLL(ctypes.util.find_library('c') or 'libc.so.6')
for name in ('weft_e2e_ring_a', 'weft_e2e_ring_b', 'weft_e2e_ring_c'):
    libc.shm_unlink(name.encode())
print('rings unlinked')
PY

echo ""
echo "e2e: PASS — capture -> validate(wave, v3) -> replay -> recapture -> compare,"
echo "      mixer stored-fallback measured, version gates enforced"
