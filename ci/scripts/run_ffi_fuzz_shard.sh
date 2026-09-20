#!/usr/bin/env bash
# run_ffi_fuzz_shard.sh — Issue #16 Tier 1 Task 4: FFI-boundary fuzzing.
#
# HONEST DECLARATIONS (never silent):
#   - No clang => no libFuzzer; no AFL++ in this environment. The harness is
#     a seeded, deterministic, structure-aware op-stream fuzzer whose oracle
#     is ASAN/UBSAN plus the protocol's own invariants. Coverage-guided
#     fuzzing is the declared follow-up for a clang-bearing CI runner.
#   - The issue's "24 hours with zero crashes" is a NIGHTLY-tier budget; the
#     cycle counts achieved here are committed as evidence (never inflated).
#   - The C<->Rust boundary leg requires cargo (absent here) — loud skip;
#     JVM/Dart/Swift boundary legs are CI-gated by their package workflows.
#
# Legs:
#   1. C kernel API fuzz, plain build (200K ops, catalog seed)
#   2. C kernel API fuzz, ASAN build (100K ops — the memory-safety oracle)
#   3. C<->TS ring boundary fuzz, BOTH directions, with RANDOM corruption:
#      random geometry/frames, 0..8 random bytes flipped anywhere in the
#      ring file (ctrl AND payload). Oracle: the consumer resolves
#      GRACEFULLY — exit 0 (clean pass) or exit 1 (honest verdict FAIL:
#      accounting or bit mismatch) are both acceptable outcomes of
#      corruption; exit >= 2 / an uncaught exception / a crash is a
#      boundary bug (RED).
#
# Output: ci/run-artifacts/shard-ffi-fuzz.log + -results.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-ffi-fuzz.log
RESULTS=ci/run-artifacts/shard-ffi-fuzz-results.json
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

step "Build: ffi-fuzz, ffi-fuzz-asan, fanout-runner"
make -C core/c ffi-fuzz ffi-fuzz-asan fanout-runner 2>&1 | tee -a "$LOG"

if [ ! -f packages/core/dist/index.js ]; then
  echo "ffi-fuzz: packages/core/dist missing (pnpm --filter @weft/core build) — boundary legs need it" | tee -a "$LOG"
  echo '{"shard":"ffi-fuzz","status":"FAILED","reason":"dist missing"}' > "$RESULTS"
  exit 1
fi

step "1. C kernel API fuzz (plain, 200K ops)"
if ./core/c/ffi-fuzz 200000 0x00C0FFEE 2>&1 | tee -a "$LOG"; then :; else fail=1; echo "leg 1 RED" | tee -a "$LOG"; fi

step "2. C kernel API fuzz (ASAN oracle, 100K ops)"
if ./core/c/ffi-fuzz-asan 100000 0x00C0FFEE 2>&1 | tee -a "$LOG"; then :; else fail=1; echo "leg 2 RED" | tee -a "$LOG"; fi

step "3. C<->TS boundary fuzz with random corruption (both directions)"
TMPD=$(mktemp -d)
ITERATIONS=${ITERATIONS:-30}
dir_c_ok=0; dir_c_graceful=0; dir_c_crash=0
dir_ts_ok=0; dir_ts_graceful=0; dir_ts_crash=0
for ((i = 1; i <= ITERATIONS; i++)); do
  # deterministic per-iteration parameters (seeded sweep — same discipline
  # as L11: same seed, same schedule)
  read -r FRAMES SLOTS WORDS NCORRUPT <<< "$(python3 - "$i" <<'PY'
import sys
i = int(sys.argv[1])
def mix(x):
    x ^= x >> 16; x = (x * 0x7FEB352D) & 0xFFFFFFFF
    x ^= x >> 15; x = (x * 0x846CA68B) & 0xFFFFFFFF
    x ^= x >> 16
    return x
s = mix(0x00C0FFEE ^ (i * 2654435761))
frames = 500 + s % 4500
slots = 2 + (s >> 8) % 7      # 2..8
words = 1 + (s >> 16) % 64    # 1..64
ncorrupt = (s >> 24) % 9      # 0..8 bytes
print(frames, slots, words, ncorrupt)
PY
)"
  RING="$TMPD/ring-$i.bin"; META="$TMPD/ring-$i.meta"

  # Direction 1: C producer -> corruption -> TS consumer
  ./core/c/fanout-runner dump-ring "$RING" "$META" "$FRAMES" "$SLOTS" "$WORDS" >/dev/null 2>&1
  if [ "$NCORRUPT" -gt 0 ]; then
    python3 - "$RING" "$NCORRUPT" "$i" <<'PY'
import sys, random
path, n, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
data = bytearray(open(path, 'rb').read())
rng = random.Random(seed)
for _ in range(n):
    off = rng.randrange(len(data))
    data[off] ^= 1 << rng.randrange(8)
open(path, 'wb').write(bytes(data))
PY
  fi
  set +e
  node fixtures/xlang-fanout/reader.mjs "$RING" "$META" >/dev/null 2>&1
  rc=$?
  set -e
  if [ $rc -le 1 ]; then
    [ $rc -eq 0 ] && dir_c_ok=$((dir_c_ok + 1)) || dir_c_graceful=$((dir_c_graceful + 1))
  else
    dir_c_crash=$((dir_c_crash + 1))
    echo "  C->TS iteration $i: exit $rc (NOT graceful) — RED" | tee -a "$LOG"
  fi

  # Direction 2: TS producer -> corruption -> C consumer (fixed writer
  # geometry: writer.mjs emits 4 slots / 64 floats; corruption sweeps)
  RING2="$TMPD/ring2-$i.bin"; META2="$TMPD/ring2-$i.meta"
  node fixtures/xlang-fanout/writer.mjs "$RING2" "$META2" "$FRAMES" >/dev/null 2>&1
  if [ "$NCORRUPT" -gt 0 ]; then
    python3 - "$RING2" "$NCORRUPT" "$((i + 7919))" <<'PY'
import sys, random
path, n, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
data = bytearray(open(path, 'rb').read())
rng = random.Random(seed)
for _ in range(n):
    off = rng.randrange(len(data))
    data[off] ^= 1 << rng.randrange(8)
open(path, 'wb').write(bytes(data))
PY
  fi
  set +e
  ./core/c/fanout-runner validate-ring "$RING2" "$META2" >/dev/null 2>&1
  rc=$?
  set -e
  if [ $rc -le 1 ]; then
    [ $rc -eq 0 ] && dir_ts_ok=$((dir_ts_ok + 1)) || dir_ts_graceful=$((dir_ts_graceful + 1))
  else
    dir_ts_crash=$((dir_ts_crash + 1))
    echo "  TS->C iteration $i: exit $rc (NOT graceful) — RED" | tee -a "$LOG"
  fi
done
rm -rf "$TMPD"
echo "boundary fuzz: C->TS ok=$dir_c_ok graceful_fail=$dir_c_graceful crash=$dir_c_crash | TS->C ok=$dir_ts_ok graceful_fail=$dir_ts_graceful crash=$dir_ts_crash (iterations=$ITERATIONS)" | tee -a "$LOG"
if [ "$dir_c_crash" -ne 0 ] || [ "$dir_ts_crash" -ne 0 ]; then fail=1; fi

step "Declarations (LAW 4 — never silent)"
command -v cargo >/dev/null 2>&1 || echo "C<->Rust boundary leg: cargo absent — SKIPPED (declared; CI rust legs cover where the toolchain exists)" | tee -a "$LOG"
command -v clang >/dev/null 2>&1 || echo "libFuzzer/AFL++ coverage-guided tier: no clang — STRUCTURED fuzzer shipped instead (declared follow-up for a clang runner)" | tee -a "$LOG"
echo "24h zero-crash soak: nightly-tier budget — this shard's committed cycle counts are the evidence (never inflated)" | tee -a "$LOG"

if [ "$fail" -eq 0 ]; then
  echo "{\"shard\":\"ffi-fuzz\",\"status\":\"PASSED\",\"gates\":\"C-API fuzz 200K + ASAN 100K + boundary fuzz $ITERATIONS iterations x2 directions (zero non-graceful exits)\"}" > "$RESULTS"
  echo "ffi-fuzz shard: PASS" | tee -a "$LOG"
else
  echo '{"shard":"ffi-fuzz","status":"FAILED"}' > "$RESULTS"
  echo "ffi-fuzz shard: RED" | tee -a "$LOG"
  exit 1
fi
