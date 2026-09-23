#!/usr/bin/env bash
###############################################################################
# Weft Studio — managed verification suite (Pillar 7, fail-closed, 7 stages)
#
#   Stage 1  integrity + webapp embed parity + boundary law (core/c untouched)
#   Stage 2  1,000,000-message heap probe (node --expose-gc, ≤ 64 KiB) + control
#   Stage 3  zero-re-render proof (React shim, 10,000-frame burst, spy == 1)
#   Stage 4  240 FPS render loop gate (4,800 frames, 0 drops, p99 < 4.166 ms)
#   Stage 5  time-travel determinism + SREC1/SBURST integrity
#   Stage 6  codegen parity (7 languages, golden hashes) + layout truth
#   Stage 7  UI discipline audit + payload budget + desktop bundle budget
#
# Exit code 0 only when ALL stages pass. Any failure aborts (fail-closed).
###############################################################################
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
STAGE_DIR="$REPO/tests/studio/managed"
EVIDENCE="$REPO/evidence/pillar7"
LOG="$REPO/evidence/pillar7-shard-run.log"
WEBAPP_ROOT="${STUDIO_WEBAPP_ROOT:-$REPO/..}"
BUILD_DIR="$(mktemp -d)"
trap 'rm -f "$STAGE_DIR"/*.gen.mjs; rm -rf "$BUILD_DIR"' EXIT

export STUDIO_UNIFIED="${STUDIO_UNIFIED:-1}"

mkdir -p "$EVIDENCE"
: > "$LOG"

# Locate runner runtime
ESBUILD_BIN="$(find "$REPO/node_modules" -name esbuild -type f -perm /111 2>/dev/null | head -n 1 || true)"

run_js_ts() {
  local script="$1"
  local extra_flags="${2:-}"
  if command -v bun >/dev/null 2>&1; then
    bun $extra_flags "$script"
  elif [ -n "$ESBUILD_BIN" ]; then
    local basename
    basename="$(basename "$script" | sed 's/\.[^.]*$//')"
    local out="$STAGE_DIR/${basename}.gen.mjs"
    "$ESBUILD_BIN" "$script" --bundle --platform=node --format=esm --outfile="$out" 2>>"$LOG"
    node $extra_flags "$out"
    rm -f "$out"
  else
    echo "E_SEAM: neither bun nor esbuild found" >&2
    return 1
  fi
}

banner() {
  local msg="$1"
  echo "" | tee -a "$LOG"
  echo "======================================================================" | tee -a "$LOG"
  echo "  $msg" | tee -a "$LOG"
  echo "======================================================================" | tee -a "$LOG"
}

run_stage() {
  local n="$1" desc="$2" cmd="$3"
  banner "STAGE $n — $desc"
  local start
  start=$(date +%s)
  if eval "$cmd" 2>&1 | tee -a "$LOG"; then
    local dur=$(( $(date +%s) - start ))
    echo "[stage $n] PASS (${dur}s)" | tee -a "$LOG"
    return 0
  else
    local rc=$?
    echo "[stage $n] FAIL (exit $rc)" | tee -a "$LOG"
    return "$rc"
  fi
}

banner "WEFT STUDIO MANAGED SUITE — Pillar 7 (branch $(git -C "$REPO" rev-parse --abbrev-ref HEAD), $(git -C "$REPO" rev-parse --short HEAD))"
echo "started: $(date -u +%FT%TZ)" | tee -a "$LOG"

FAILED=0

run_stage 1 "integrity + embed parity + boundary law" \
  "run_js_ts '$STAGE_DIR/s1_integrity.mjs'" || FAILED=1

run_stage 2 "1,000,000-message heap probe (node --expose-gc)" \
  "run_js_ts '$STAGE_DIR/s2_probe_entry.ts' '--expose-gc'" || FAILED=1

run_stage 3 "zero-re-render proof (10,000-frame burst)" \
  "run_js_ts '$STAGE_DIR/s3_zero_rerender.mjs'" || FAILED=1

run_stage 4 "240 FPS render loop gate (4,800 frames)" \
  "run_js_ts '$STAGE_DIR/s4_frame_gate.mjs'" || FAILED=1

run_stage 5 "time-travel determinism + stream integrity" \
  "run_js_ts '$STAGE_DIR/s5_timetravel.ts'" || FAILED=1

run_stage 6 "codegen parity + layout truth" \
  "run_js_ts '$STAGE_DIR/s6_codegen_parity.ts'" || FAILED=1

run_stage 7 "UI discipline audit + bundle budgets" \
  "run_js_ts '$STAGE_DIR/s7_ui_audit.ts'" || FAILED=1

banner "VERDICT"
if [ "$FAILED" -eq 0 ]; then
  echo "ALL 7 STAGES GREEN — weftc-pillar7-managed is releasable." | tee -a "$LOG"
  echo "finished: $(date -u +%FT%TZ)" | tee -a "$LOG"
  exit 0
else
  echo "SUITE FAILED — release blocked (fail-closed)." | tee -a "$LOG"
  echo "finished: $(date -u +%FT%TZ)" | tee -a "$LOG"
  exit 2
fi
