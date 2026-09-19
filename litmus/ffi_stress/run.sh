#!/usr/bin/env bash
# litmus/ffi_stress/run.sh — L11-ffi-stress (Issue #16 Tier 1): cross-language
# ring boundary stress, script-orchestrated because the boundary itself lives
# across processes (node <-> C), not inside one runner CLI.
#
# Adversary (catalog v3): N iterations of swept geometry and frame counts over
# BOTH directions of the RFC-0004 byte-compat boundary:
#   Direction 1: TS producer (@weft/core dist broadcaster) -> C consumer
#                (fanout-runner validate-ring)
#   Direction 2: C producer (fanout-runner dump-ring, swept slots/words)
#                -> TS consumer (WeftFanoutReader)
# Every iteration's ring must validate: geometry, handoff accounting (fresh
# final frame, dropped = frames-1), and every payload word against the shared
# 04-LITMUS §0.1 mixer. A single torn accept anywhere exits non-zero.
#
# Honest boundary declarations (never silent):
#   - The Rust leg (C <-> Rust as_bytes/attach_raw) requires cargo; when the
#     toolchain is absent it is SKIPPED with a loud line here (the rust litmus
#     shard + CI legs own that surface where the toolchain exists).
#   - JVM/Dart/Swift boundary legs are CI-gated (kotlinc/dart/swift toolchains
#     absent in this sandbox) — declared, covered by their package CI legs.
#   - The boundary under test is the RING (shared memory over file), the same
#     interop contract the xlang-fanout fixture gates; process-boundary faults
#     (signal crashes) are the fuzz harness's surface, not this test's.
#
# Output: exactly ONE JSON verdict line on stdout (last line); diagnostics to
#         stderr. Exit 0 pass / 1 fail / 2 environment error.
#
# Requires: node 18+, gcc, ../../packages/core/dist built, fanout-runner built.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$HERE"

NODE=${NODE:-node}
RUNNER="$ROOT/core/c/fanout-runner"
ITERATIONS=${ITERATIONS:-25}
FRAMES_MIN=${FRAMES_MIN:-1000}
FRAMES_MAX=${FRAMES_MAX:-50000}

if ! command -v "$NODE" >/dev/null 2>&1; then
  echo "L11: node not found — environment error" >&2
  exit 2
fi
if [ ! -x "$RUNNER" ]; then
  ( cd "$ROOT/core/c" && make fanout-runner ) >&2 || { echo "L11: fanout-runner build failed" >&2; exit 2; }
fi
if [ ! -f "$ROOT/packages/core/dist/index.js" ]; then
  echo "L11: packages/core/dist missing — build with: pnpm --filter @weft/core build" >&2
  exit 2
fi

# Deterministic sweep schedule (seeded xorshift32 over the iteration axis —
# the same discipline the catalog's L6 uses; same seed, same schedule).
sched_frames() {  # sched_frames <i> -> frames in [FRAMES_MIN, FRAMES_MAX]
  python3 - "$1" "$FRAMES_MIN" "$FRAMES_MAX" <<'PY'
import sys
i, lo, hi = (int(x) for x in sys.argv[1:4])
s = 0x00C0FFEE ^ (i * 2654435761) & 0xFFFFFFFF
s ^= s >> 16; s = (s * 0x7FEB352D) & 0xFFFFFFFF; s ^= s >> 15
s = (s * 0x846CA68B) & 0xFFFFFFFF; s ^= s >> 16
print(lo + (s % (hi - lo + 1)))
PY
}

dir1_ok=0; dir1_fail=0
dir2_ok=0; dir2_fail=0
total_frames=0

for ((i = 1; i <= ITERATIONS; i++)); do
  FR=$(sched_frames "$i")
  total_frames=$((total_frames + FR))

  # Direction 1: TS producer -> C consumer (writer.mjs geometry is fixed at
  # 4 slots / 64 floats; the frame axis sweeps here).
  if "$NODE" "$ROOT/fixtures/xlang-fanout/writer.mjs" ring-l11-ts.bin ring-l11-ts.meta "$FR" >&2 2>&1 \
     && "$RUNNER" validate-ring ring-l11-ts.bin ring-l11-ts.meta >&2 2>&1; then
    dir1_ok=$((dir1_ok + 1))
  else
    dir1_fail=$((dir1_fail + 1))
    echo "L11: direction 1 FAILED at iteration $i (frames=$FR)" >&2
  fi
  rm -f ring-l11-ts.bin ring-l11-ts.meta

  # Direction 2: C producer -> TS consumer, swept geometry
  # (slots x words cycle deterministically over the legal space).
  SLOTS=$(( 2 + (i % 4) * 2 ))            # 2, 4, 6, 8
  WORDS=$(( 1 + (i % 3) * 31 ))           # 1, 32, 63
  if "$RUNNER" dump-ring ring-l11-c.bin ring-l11-c.meta "$FR" "$SLOTS" "$WORDS" >&2 2>&1 \
     && "$NODE" "$ROOT/fixtures/xlang-fanout/reader.mjs" ring-l11-c.bin ring-l11-c.meta >&2 2>&1; then
    dir2_ok=$((dir2_ok + 1))
  else
    dir2_fail=$((dir2_fail + 1))
    echo "L11: direction 2 FAILED at iteration $i (frames=$FR slots=$SLOTS words=$WORDS)" >&2
  fi
  rm -f ring-l11-c.bin ring-l11-c.meta
done

# Rust leg (C <-> Rust as_bytes/attach_raw): loud skip when cargo is absent.
rust_leg="skipped-declared"
if command -v cargo >/dev/null 2>&1; then
  rust_leg="available-not-wired"   # tracked follow-up: rust litmus bin leg
  echo "L11: cargo present — the C<->Rust boundary leg is a declared follow-up (rust litmus shard owns it)" >&2
else
  echo "L11: cargo not found — C<->Rust boundary leg SKIPPED (declared; CI rust legs cover where the toolchain exists)" >&2
fi
echo "L11: JVM/Dart/Swift boundary legs are CI-gated (toolchains absent here — declared, not silent)" >&2

pass=0
if [ "$dir1_fail" -eq 0 ] && [ "$dir2_fail" -eq 0 ]; then pass=1; fi
echo "L11 iterations=$ITERATIONS dir1_ok=$dir1_ok dir1_fail=$dir1_fail dir2_ok=$dir2_ok dir2_fail=$dir2_fail total_frames=$total_frames rust_leg=$rust_leg pass=$pass" >&2

printf '{"test":"L11-ffi-stress","lang":"c","pass":%s,"metrics":{"iterations":%d,"dir1_ok":%d,"dir1_fail":%d,"dir2_ok":%d,"dir2_fail":%d,"total_frames":%d,"rust_leg":"%s"}}\n' \
  "$([ $pass -eq 1 ] && echo true || echo false)" "$ITERATIONS" "$dir1_ok" "$dir1_fail" "$dir2_ok" "$dir2_fail" "$total_frames" "$rust_leg"

exit $([ $pass -eq 1 ] && echo 0 || echo 1)
