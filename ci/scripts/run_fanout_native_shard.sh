#!/usr/bin/env bash
# run_fanout_native_shard.sh — RFC-0004 native fan-out shard (C + Rust + xlang).
#
# Gates (any failure exits non-zero):
#   1. C F-series conformance, both ordering regimes (fenced acq/rel + seq_cst)
#   2. C torture: 1 writer x 4 readers, every fresh claim word-validated,
#      both regimes (200k frames — CI-friendly; the committed 1M-frame
#      evidence lives in litmus/evidence/fanout/)
#   3. Rust suite: F-series + cursor + torture + exhaustive Loom model
#      (embedded preemption bound 2 — terminates in ~25s)
#   4. Cross-language interop: TS producer -> C consumer, C producer -> TS
#      consumer, bit-exact payload validation (requires packages/core/dist —
#      built by the workflow's fanout-native leg or `pnpm --filter @weft/core build`)
#   5. RFC-0009 governor: G-series in C and Rust + G5 trace parity across
#      TS/C/Rust (fixtures/xlang-governor, same dist requirement as gate 4)
#
# Output: ci/run-artifacts/shard-fanout-native.log
#         ci/run-artifacts/shard-fanout-native-results.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-fanout-native.log
: > "$LOG"

fail=0
step() { echo "" | tee -a "$LOG"; echo "=== $1 ===" | tee -a "$LOG"; }

# --- 1+2: C gates, both ordering regimes ---
step "C build (fenced acq/rel + seq_cst)"
make -C core/c fanout-test fanout-test-seq fanout-runner fanout-runner-seq 2>&1 | tee -a "$LOG"

step "C F-series conformance (default regime)"
./core/c/fanout-test 2>&1 | tee -a "$LOG" || fail=1
step "C F-series conformance (all-seq_cst regime)"
./core/c/fanout-test-seq 2>&1 | tee -a "$LOG" || fail=1

step "C torture (default regime, 200k frames)"
./core/c/fanout-runner torture 200000 4 256 4 2>&1 | tee -a "$LOG" || fail=1
step "C torture (all-seq_cst regime, 200k frames)"
./core/c/fanout-runner-seq torture 200000 4 256 4 2>&1 | tee -a "$LOG" || fail=1

# --- 2b (issue #17-1): SIMD claim copy — FS-series gate, both ordering
# regimes + the legacy (seam-disabled) escape hatch + ASAN. TSAN runs in
# the turbo-native shard's TSAN leg (dispatcher self-pins scalar there).
step "FS-series SIMD claim-copy gate (default + seq_cst + legacy + asan)"
make -C core/c fanout-simd-test fanout-simd-test-seq fanout-simd-test-legacy fanout-simd-test-asan 2>&1 | tee -a "$LOG"
./core/c/fanout-simd-test torture_s=1.0 2>&1 | tee -a "$LOG" || fail=1
./core/c/fanout-simd-test-seq torture_s=1.0 2>&1 | tee -a "$LOG" || fail=1
./core/c/fanout-simd-test-legacy torture_s=1.0 2>&1 | tee -a "$LOG" || fail=1
./core/c/fanout-simd-test-asan torture_s=1.0 2>&1 | tee -a "$LOG" || fail=1

# --- 2c (issue #17-2): cache-aware slot layout — FL-series (split theory
# vs brute force, allocation guarantee, latency A/B, interop byte-layout
# equality) + the 32B-line no-regression leg.
step "FL-series cache-aware layout gate (64B + 32B-line legs)"
make -C core/c fanout-layout-test fanout-layout-test-smallline fanout-layout-test-asan 2>&1 | tee -a "$LOG"
./core/c/fanout-layout-test 2>&1 | tee -a "$LOG" || fail=1
./core/c/fanout-layout-test-smallline 2>&1 | tee -a "$LOG" || fail=1
./core/c/fanout-layout-test-asan 2>&1 | tee -a "$LOG" || fail=1

# --- 2d (issue #17-3): writer batching — FB-series (roundtrip, mixed-mode,
# dropped accounting, windowed-stamping torture, atomic refusal, commit-only
# API) x4 regimes.
step "FB-series writer-batching gate (default + seq_cst + asan)"
make -C core/c fanout-batch-test fanout-batch-test-seq fanout-batch-test-asan 2>&1 | tee -a "$LOG"
./core/c/fanout-batch-test torture_s=1.0 2>&1 | tee -a "$LOG" || fail=1
./core/c/fanout-batch-test-seq torture_s=1.0 2>&1 | tee -a "$LOG" || fail=1
./core/c/fanout-batch-test-asan torture_s=1.0 2>&1 | tee -a "$LOG" || fail=1

# --- 3: Rust suite ---
step "Rust suite (F-series + cursor + torture + Loom model, bound 2)"
if command -v cargo >/dev/null 2>&1; then
  (cd core/rust && WEFT_FANOUT_FRAMES=200000 cargo test --release) 2>&1 | tee -a "$LOG" || fail=1
else
  echo "cargo not found — SKIPPED (declared)" | tee -a "$LOG"
fi

# --- 4: cross-language interop ---
step "xlang interop (TS <-> C, bit-exact)"
if [ -f packages/core/dist/index.js ]; then
  FRAMES=2000 bash fixtures/xlang-fanout/run.sh 2>&1 | tee -a "$LOG" || fail=1
else
  echo "packages/core/dist not built — interop SKIPPED (build with: pnpm --filter @weft/core build)" | tee -a "$LOG"
  if [ -n "${CI:-}" ]; then fail=1; fi # in CI the workflow builds it; missing dist is a failure
fi

# --- 5: JNI bridge (host JVM over the exact Android C sources) ---
# gcc + a JDK are preinstalled on ubuntu-latest runners; the harness
# compiles weft_jni.c + weft.c + fanout.c itself and runs a 1W x 3R
# threaded torture on real JVM threads.
step "JNI fan-out harness (host JVM, 200k frames x 3 readers)"
if command -v javac >/dev/null 2>&1 && command -v gcc >/dev/null 2>&1; then
  EVIDENCE=0 bash fixtures/jni-fanout/run.sh 2>&1 | tee -a "$LOG" || fail=1
else
  echo "javac or gcc not found — JNI harness SKIPPED (declared)" | tee -a "$LOG"
  if [ -n "${CI:-}" ]; then fail=1; fi # CI runners have both; missing toolchain is a failure
fi

# --- 5: RFC-0009 governor (G-series + G5 trace parity) ---
step "governor G-series (C)"
make -C core/c governor-test 2>&1 | tee -a "$LOG" || fail=1
./core/c/governor-test 2>&1 | tee -a "$LOG" || fail=1

step "governor G-series (Rust) + G5 trace parity (TS/C/Rust + VM emitters)"
if command -v cargo >/dev/null 2>&1; then
  (cd core/rust && cargo test --release --test governor_test) 2>&1 | tee -a "$LOG" || fail=1
  (cd core/rust && cargo build --release --bin governor_xlang) 2>&1 | tee -a "$LOG" || fail=1
  if [ -f packages/core/dist/index.js ]; then
    bash fixtures/xlang-governor/run.sh 2>&1 | tee -a "$LOG" || fail=1
  else
    echo "packages/core/dist not built — G5 SKIPPED (build with: pnpm --filter @weft/core build)" | tee -a "$LOG"
    if [ -n "${CI:-}" ]; then fail=1; fi
  fi
else
  echo "cargo not found — governor rust gates SKIPPED (declared)" | tee -a "$LOG"
  if [ -f packages/core/dist/index.js ] && [ -x core/c/governor-test ]; then
    # cargo absent but the C ladder + TS/VM emitters are runnable: the G5
    # gate best-efforts the rust leg as a declared skip.
    bash fixtures/xlang-governor/run.sh 2>&1 | tee -a "$LOG" || fail=1
  fi
fi

# Gate 5b (Series 7): RFC-0009 §cadence PC3 trace parity — the TS
# reference plus every VM emitter whose toolchain this runner has
# (kotlinc on android-packages legs; dart/swiftc on their workflows).
step "cadence PC3 trace parity (TS + VM emitters present)"
if [ -f packages/core/dist/index.js ]; then
  bash fixtures/xlang-cadence/run.sh 2>&1 | tee -a "$LOG" || fail=1
else
  echo "packages/core/dist not built — PC3 SKIPPED (build with: pnpm --filter @weft/core build)" | tee -a "$LOG"
  if [ -n "${CI:-}" ]; then fail=1; fi
fi

# --- 6: fan-out flight recorder (.weftrec v2, FORMATS.md §1.5) ---
step "flight-recorder selftest (capture -> validate(mixer) -> replay -> recapture -> compare)"
make -C tools/weft-fanout-rec weft-fanout-rec weft-fanout-rec-seq 2>&1 | tee -a "$LOG" || fail=1
./tools/weft-fanout-rec/weft-fanout-rec selftest --frames 20000 --words 64 2>&1 | tee -a "$LOG" || fail=1
./tools/weft-fanout-rec/weft-fanout-rec-seq selftest --frames 20000 --words 64 2>&1 | tee -a "$LOG" || fail=1

# Gate 6 (Series 6): RFC-0010 v3 compression e2e — the capture -> validate
# -> replay -> recapture loop under --compress, wave + mixer families,
# version gates, measured ratio.
step "flight-recorder e2e under v3 compression (RFC-0010)"
bash tools/weft-fanout-rec/e2e.sh 2>&1 | tee -a "$LOG" || fail=1

# --- 6b: inter-process SHM ring sessions (RFC-0011 draft, Series 7) ---
# S-series conformance + multi-process torture (both roads) + the
# zero-syscall strace gate (data path is pure shared-memory atomics —
# proof that between marker writes NO syscall appears) + the recorder's
# cross-process produce->daemon flow over the session protocol.
step "IPC shm sessions: S-series conformance (C)"
make -C core/c shm-test 2>&1 | tee -a "$LOG" || fail=1
./core/c/shm-test 2>&1 | tee -a "$LOG" || fail=1

step "IPC shm sessions: multi-process torture (named + anonymous roads)"
make -C core/c shm-runner 2>&1 | tee -a "$LOG" || fail=1
./core/c/shm-runner torture 64 4 100000 2>&1 | tee -a "$LOG" || fail=1
./core/c/shm-runner torture 64 4 100000 --fork 2>&1 | tee -a "$LOG" || fail=1

step "IPC shm sessions: Rust S-series (fork torture included)"
if command -v cargo >/dev/null 2>&1; then
  (cd core/rust && cargo test --release --test shm_test) 2>&1 | tee -a "$LOG" || fail=1
else
  echo "cargo not found — shm rust gates SKIPPED (declared)" | tee -a "$LOG"
  if [ -n "${CI:-}" ]; then fail=1; fi
fi

step "IPC shm sessions: zero-syscall data path (strace gate)"
if command -v strace >/dev/null 2>&1; then
  STRACE_LOG=$(mktemp)
  strace -f -o "$STRACE_LOG" ./core/c/shm-runner strace-proof weft-ci-strace 50000 --marker 2>/dev/null | tee -a "$LOG" || fail=1
  BETWEEN=$(awk '/MARKER-PUBLISH-START/,/MARKER-PUBLISH-END/' "$STRACE_LOG" | grep -vc 'MARKER\|^$' || true)
  echo "non-marker syscalls between publish markers: $BETWEEN (must be 0)" | tee -a "$LOG"
  [ "$BETWEEN" = "0" ] || fail=1
  rm -f "$STRACE_LOG"
else
  echo "strace not found — zero-syscall gate SKIPPED (declared)" | tee -a "$LOG"
  if [ -n "${CI:-}" ]; then fail=1; fi # ubuntu-latest has strace
fi

step "IPC shm sessions: cross-process produce -> daemon capture (session protocol)"
SESS=weft-ci-session-$$
(./tools/weft-fanout-rec/weft-fanout-rec produce --shm "$SESS" --payload 64 --slots 4 --frames 500 --hz 2000 >/dev/null 2>&1 &)
sleep 1
timeout 15 ./tools/weft-fanout-rec/weft-fanout-rec daemon /tmp/weft-ci-daemon-$$.weftrec --shm "$SESS" --payload 64 --slots 4 --idle-ms 300 2>&1 | tee -a "$LOG" || fail=1
./tools/weft-fanout-rec/weft-fanout-rec validate /tmp/weft-ci-daemon-$$.weftrec 2>&1 | tee -a "$LOG" || fail=1
rm -f /tmp/weft-ci-daemon-$$.weftrec
python3 -c "import ctypes; ctypes.CDLL(None).shm_unlink(b'/$SESS')" 2>/dev/null || true

# --- 7: F10 100k torture PARITY across ports (C baseline + JVM + parity-
#         of-contract over the Kotlin/Swift/Dart F10 sources) ---
step "F10 100k torture parity (C baseline x2 regimes + JVM + source contract)"
bash ci/scripts/run_f10_parity.sh 2>&1 | tee -a "$LOG" || fail=1

if [ "$fail" -ne 0 ]; then
  echo '{"shard":"fanout-native","status":"FAILED"}' > ci/run-artifacts/shard-fanout-native-results.json
  exit 1
fi
echo '{"shard":"fanout-native","status":"PASSED","gates":"C F-series+torture x2 regimes + FS-series SIMD claim-copy x4 regimes + FL-series layout x3 builds + FB-series batching x3 regimes + rust suite + loom(bound 2) + xlang TS<->C + jni-harness JVM + flight-rec selftest x2 regimes + governor G-series C/Rust + G5 trace parity (TS/C/Rust + VM) + cadence PC3 trace parity (TS + VM) + v3-compression e2e + shm S-series + shm torture x2 roads + shm rust + shm zero-syscall strace + shm produce->daemon + F10 cross-port parity"}' > ci/run-artifacts/shard-fanout-native-results.json
echo "✅ fanout-native shard PASSED" | tee -a "$LOG"
