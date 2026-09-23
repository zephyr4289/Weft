#!/usr/bin/env bash
# tools/verify/tests/run_verify_managed_suite.sh
#
# WEFT-DIRECTIVE-PILLAR-8-ENG3 — 7-stage fail-closed managed verification
# matrix for the weft-verify developer suite (@weft/verify).
#
#   Stage 1  subsystem integrity, file manifest, boundary law + charset audit
#   Stage 2  AST allocation linter cross-language precision & recall (100%)
#   Stage 3  zero-allocation runtime execution probe (1e6 iters, <= 64 KiB)
#   Stage 4  CLI command suite & flag parser verification
#   Stage 5  scorecard HTML/JSON determinism + schema + >= 1e7 state tally
#   Stage 6  git hook integration (poisoned commit rejected, clean passes,
#            hook latency < 50 ms)
#   Stage 7  package payload & zero-runtime-dependency audit (< 100 KiB)
#
# Fail-closed: any failed assertion aborts the suite with exit 1 and leaves
# the evidence trail under tests/verify/managed/evidence/.
set -uo pipefail

SUITEDIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SUITEDIR/../../.." && pwd)
MANAGED="$ROOT/tests/verify/managed"
EVID="$MANAGED/evidence"
VV="$ROOT/tools/verify/weft-verify"
PROBE_RUN="$MANAGED/probe_run.ts"
STAGE2_CHECK="$MANAGED/stage2_check.ts"
STAGE5_CHECK="$MANAGED/stage5_schema_check.ts"
EXPECT="$MANAGED/fixtures/expected/poisoned.expect.json"

mkdir -p "$EVID"
FAILURES=0
STAGE=0
SUITE_START=$(date +%s)

log()  { printf '%s\n' "$*" | tee -a "$EVID/suite-run.log" >&2; }
fail() {
  log "  [FAIL] $*"
  FAILURES=$((FAILURES + 1))
  exit 1
}
stage_banner() {
  STAGE=$1
  log ""
  log "────────────────────────────────────────────────────────────"
  log "[STAGE $STAGE/7] $2"
  log "────────────────────────────────────────────────────────────"
}
# expect_success <label> <cmd...>
expect_success() {
  local label="$1"; shift
  local out rc
  out=$("$@" 2>&1); rc=$?
  if [ $rc -ne 0 ]; then
    log "$out" | tail -20 >> "$EVID/suite-run.log"
    fail "$label (exit $rc)"
  fi
  LAST_OUT="$out"
  log "  [ok] $label"
}
# expect_exit <wanted-code> <label> <cmd...>
expect_exit() {
  local wanted="$1" label="$2"; shift 2
  local out rc
  out=$("$@" 2>&1); rc=$?
  if [ "$rc" -ne "$wanted" ]; then
    log "$out" | tail -20 >> "$EVID/suite-run.log"
    fail "$label (wanted exit $wanted, got $rc)"
  fi
  LAST_OUT="$out"
  log "  [ok] $label (exit $rc as required)"
}
# expect_grep <pattern> <label>  (checks LAST_OUT)
expect_grep() {
  if ! printf '%s' "$LAST_OUT" | grep -qE "$1"; then
    fail "$2 (pattern not found: $1)"
  fi
  log "  [ok] $2"
}
node_json() { # node_json <file> <js-expression over c>
  node -e "
    const fs = require('fs');
    const c = JSON.parse(fs.readFileSync(process.argv[1], 'utf8'));
    process.exit(($2) ? 0 : 1);
  " "$1"
}

: > "$EVID/suite-run.log"
log "weft-verify managed suite — start $(date -u +%Y-%m-%dT%H:%M:%SZ) host=$(uname -sm)"

# type-stripping flags for direct .ts execution (Node 22.6+ mandated)
if node --experimental-strip-types -e 'process.exit(0)' >/dev/null 2>&1; then
  NODE_TS_FLAGS="--experimental-strip-types --no-warnings"
else
  NODE_TS_FLAGS="--no-warnings"
fi

trap 'log ""; log "SUITE ABORTED at stage $STAGE"; exit 1' ERR

# ════════════════════ STAGE 1: integrity + boundary ════════════════════
stage_banner 1 "subsystem integrity, file manifest, boundary law"
log "  node $(node --version) | git $(git --version | cut -d' ' -f3) | flags: $NODE_TS_FLAGS"

MISSING=""
for f in \
  packages/verify/package.json \
  packages/verify/tsconfig.json \
  packages/verify/README.md \
  packages/verify/bin/weft-verify.mjs \
  packages/verify/src/cli.ts \
  packages/verify/src/lint/mask.ts \
  packages/verify/src/lint/hot.ts \
  packages/verify/src/lint/rules.ts \
  packages/verify/src/lint/scan.ts \
  packages/verify/src/formal/index.ts \
  packages/verify/src/formal/small.ts \
  packages/verify/src/formal/wide.ts \
  packages/verify/src/formal/types.ts \
  packages/verify/src/chaos/index.ts \
  packages/verify/src/chaos/prng.ts \
  packages/verify/src/ring/triad.ts \
  packages/verify/src/runtime/probe.ts \
  packages/verify/src/scorecard/schema.ts \
  packages/verify/src/scorecard/html.ts \
  packages/verify/formal/TriadBuffer.tla \
  tools/verify/weft-verify \
  tools/verify/hooks/pre-commit-alloc-lint.sh \
  tools/verify/hooks/pre-push-alloc-lint.sh \
  tools/verify/hooks/install.sh \
  tests/verify/managed/probe_run.ts \
  tests/verify/managed/stage2_check.ts \
  tests/verify/managed/stage5_schema_check.ts \
  tests/verify/managed/fixtures/expected/poisoned.expect.json \
  tests/verify/managed/fixtures/poisoned/poison.ts \
  tests/verify/managed/fixtures/poisoned/poison.c \
  tests/verify/managed/fixtures/poisoned/poison.cpp \
  tests/verify/managed/fixtures/poisoned/poison.rs \
  tests/verify/managed/fixtures/poisoned/poison.swift \
  tests/verify/managed/fixtures/poisoned/poison.dart \
  tests/verify/managed/fixtures/clean/clean.ts \
  tests/verify/managed/fixtures/clean/clean.c \
  tests/verify/managed/fixtures/clean/clean.cpp \
  tests/verify/managed/fixtures/clean/clean.rs \
  tests/verify/managed/fixtures/clean/clean.swift \
  tests/verify/managed/fixtures/clean/clean.dart
do
  [ -f "$ROOT/$f" ] || MISSING="$MISSING $f"
done
[ -z "$MISSING" ] || fail "manifest missing:$MISSING"
log "  [ok] manifest: 41/41 required files present"

# Boundary law: the verify payload may never reference or write core/c/
BOUNDARY_HITS=$( { grep -rn "core/c/" "$ROOT/packages/verify/src" "$ROOT/packages/verify/bin" \
  "$ROOT/tools/verify/weft-verify" "$ROOT/tools/verify/hooks" 2>/dev/null || true; } | wc -l )
[ "$BOUNDARY_HITS" -eq 0 ] || { grep -rn "core/c/" "$ROOT/packages/verify" "$ROOT/tools/verify" | head -10 >> "$EVID/suite-run.log"; fail "boundary law: verify code references core/c/ ($BOUNDARY_HITS hits)"; }
CORE_WRITES=$(find "$ROOT/packages/verify" "$ROOT/tools/verify" "$ROOT/tests/verify" -type f | grep -c "^$ROOT/core/" || true)
[ "$CORE_WRITES" -eq 0 ] || fail "boundary law: payload contains files under core/ ($CORE_WRITES)"
log "  [ok] boundary law: zero core/c/ references, zero core/ payload paths"

# Charset integrity: the sandbox mangler eats the open-bracket+lowercase-h
# byte pair; the payload must be free of it so both fs views converge.
BADPAT=$(printf '\133h')
BAD_HITS=$( { grep -rF -- "$BADPAT" "$ROOT/packages/verify" "$ROOT/tools/verify" "$ROOT/tests/verify/managed" 2>/dev/null || true; } | grep -cv Binary || true )
[ "$BAD_HITS" -eq 0 ] || fail "charset integrity: $BAD_HITS corrupted marker(s) present"
log "  [ok] charset integrity: payload free of the filtered byte pair"

expect_success "CLI runs with zero warnings" sh -c "cd $ROOT && NODE_OPTIONS=--no-warnings node packages/verify/bin/weft-verify.mjs --version 2>&1 | tee /dev/stderr | grep -q 'weft-verify 1.0.0'"
expect_success "hook scripts are executable" test -x "$ROOT/tools/verify/hooks/pre-commit-alloc-lint.sh"
log "[STAGE 1] PASS"

# ════════════════ STAGE 2: linter precision & recall ═══════════════════
stage_banner 2 "AST allocation linter — cross-language precision & recall"
FINDINGS_JSON="$EVID/stage2-findings.json"
expect_exit 1 "lint flags the poisoned fixture set (exit 1 required)" \
  sh -c "$VV --lint-alloc '$MANAGED/fixtures/poisoned' --format json > '$FINDINGS_JSON'"
expect_success "lint passes the clean fixture set" \
  "$VV" --lint-alloc "$MANAGED/fixtures/clean"
node $NODE_TS_FLAGS "$STAGE2_CHECK" "$FINDINGS_JSON" "$EXPECT" > "$EVID/stage2-verdict.json" \
  || fail "stage2 comparator failed"
node_json "$EVID/stage2-verdict.json" 'c.ok && c.precision === 1 && c.recall === 1' \
  || fail "precision/recall not 100% (see $EVID/stage2-verdict.json)"
log "  [ok] precision 1.0 / recall 1.0 — $(node -e 'const c=require("'"$EVID"'/stage2-verdict.json"); console.log(c.flagged + " flagged == " + c.poisonedFixtures + " expected, " + c.cleanFindings + " clean findings")')"
log "[STAGE 2] PASS"

# ══════════════ STAGE 3: zero-alloc runtime execution probe ════════════
stage_banner 3 "zero-allocation runtime execution probe (1,000,000 iterations)"
expect_success "probe: 1e6 hot-loop iterations, heap growth <= 64 KiB" \
  node $NODE_TS_FLAGS --expose-gc "$PROBE_RUN" 1000000 3
node $NODE_TS_FLAGS --expose-gc "$PROBE_RUN" 1000000 3 > "$EVID/stage3-probe.json" \
  || fail "probe evidence write failed"
node_json "$EVID/stage3-probe.json" 'c.pass && c.iterations === 1000000 && c.growthKiB <= 64' \
  || fail "probe gate not satisfied"
GROWTH=$(node -e 'console.log(require("'"$EVID"'/stage3-probe.json").growthKiB)')
log "  [ok] heap growth $GROWTH KiB <= 64 KiB gate (min-of-3 GC rounds)"
expect_exit 1 "negative control: allocating loop blows the gate (exit 1)" \
  node $NODE_TS_FLAGS --expose-gc "$PROBE_RUN" 100000 1 bite
expect_success "dogfood: the engine's own @hot probe source lints clean" \
  "$VV" --lint-alloc "$ROOT/packages/verify/src"
log "[STAGE 3] PASS"

# ══════════════════ STAGE 4: CLI command suite ═════════════════════════
stage_banner 4 "CLI command suite & flag parser verification"
expect_success "--help prints usage" sh -c "$VV --help | grep -q 'weft verify'"
LAST_OUT=""
LAST_OUT=$("$VV" --version 2>&1)
expect_grep '^weft-verify 1\.0\.0' "--version format"
expect_exit 2 "unknown flag rejected with exit 2" "$VV" --definitely-not-a-flag
expect_exit 2 "missing path rejected with exit 2" "$VV" --lint-alloc /nonexistent/nope.ts
expect_exit 2 "invalid chaos profile rejected with exit 2" "$VV" --chaos warp-drive
expect_exit 2 "missing command rejected with exit 2" "$VV"
expect_exit 1 "poisoned file lint exits 1" "$VV" --lint-alloc "$MANAGED/fixtures/poisoned/poison.c"
LAST_OUT=$("$VV" --lint-alloc "$MANAGED/fixtures/poisoned/poison.c" 2>&1 || true)
expect_grep 'WV-C-001' "diagnostics carry the rule id"
expect_success "clean file lint exits 0" "$VV" --lint-alloc "$MANAGED/fixtures/clean/clean.c"
expect_success "--chaos thermal --json shape" sh -c "$VV --chaos thermal --json | grep -q '\"profile\": \"thermal\"'"
expect_success "--chaos network --json shape" sh -c "$VV --chaos network --json | grep -q 'resilienceScore'"
expect_success "--formal --json (bounded 50k) verdict PASS" sh -c "$VV --formal --states 50000 --json | grep -q '\"verdict\": \"PASS\"'"
TMP4=$(mktemp -d)
expect_success "--report json from formal artifact" sh -c "$VV --formal --states 50000 --json > $TMP4/formal.json && $VV --report json --input $TMP4/formal.json --out $TMP4/card.json"
[ -f "$TMP4/card.json" ] || fail "--report did not write the scorecard"
node_json "$TMP4/card.json" 'c.verdict === "PASS" && c.formal.theorems.length >= 9' || fail "report card malformed"
log "  [ok] report pipeline: formal artifact -> scorecard.json (PASS, 9+ theorems)"
TMP4A=$(mktemp -d)
expect_success "--all (bounded 200k) end-to-end" sh -c "$VV --all --states 200000 --out $TMP4A >/dev/null"
for art in scorecard.json scorecard.html formal.json; do
  [ -f "$TMP4A/$art" ] || fail "--all did not write $art"
done
log "  [ok] --all wrote scorecard.json + scorecard.html + formal.json"
rm -rf "$TMP4" "$TMP4A"
log "[STAGE 4] PASS"

# ═══════════ STAGE 5: determinism, schema and the 1e7 state tally ══════
stage_banner 5 "scorecard determinism + schema + >= 1e7 state-space tally"
T5=$(mktemp -d)
log "  running full formal pass (default target 10,000,000 states)..."
expect_success "full formal pass" sh -c "$VV --formal --json > $T5/formal.json"
node_json "$T5/formal.json" 'c.verdict === "PASS" && c.wide.explored >= 10000000 && c.wide.deadlineHit === false' \
  || fail "formal verdict/tally gate failed"
EXPLORED=$(node -e 'console.log(require("'"$T5"'/formal.json").wide.explored)')
WIDESEC=$(node -e 'console.log(require("'"$T5"'/formal.json").wide.seconds)')
log "  [ok] state-space tally: $EXPLORED distinct states (>= 1e7) in ${WIDESEC}s"

expect_success "report json (run 1)" sh -c "$VV --report json --input $T5/formal.json --out $T5/r1.json >/dev/null"
expect_success "report json (run 2)" sh -c "$VV --report json --input $T5/formal.json --out $T5/r2.json >/dev/null"
expect_success "report html (run 1)" sh -c "$VV --report html --input $T5/formal.json --out $T5/r1.html >/dev/null"
expect_success "report html (run 2)" sh -c "$VV --report html --input $T5/formal.json --out $T5/r2.html >/dev/null"
J1=$(sha256sum "$T5/r1.json" | cut -d' ' -f1); J2=$(sha256sum "$T5/r2.json" | cut -d' ' -f1)
H1=$(sha256sum "$T5/r1.html" | cut -d' ' -f1); H2=$(sha256sum "$T5/r2.html" | cut -d' ' -f1)
[ "$J1" = "$J2" ] || fail "scorecard JSON not byte-deterministic ($J1 vs $J2)"
[ "$H1" = "$H2" ] || fail "scorecard HTML not byte-deterministic ($H1 vs $H2)"
log "  [ok] determinism: JSON + HTML byte-identical across runs (sha256 $J1)"

expect_success "network chaos determinism (seeded)" sh -c "$VV --chaos network --json > $T5/n1.json && $VV --chaos network --json > $T5/n2.json && sha256sum $T5/n1.json $T5/n2.json | uniq -w64 | wc -l | grep -q '^1$'"
expect_success "scorecard schema + tally validation" node $NODE_TS_FLAGS "$STAGE5_CHECK" "$T5/r1.json" "$T5/formal.json"
cp "$T5/r1.json" "$EVID/stage5-scorecard.json"; cp "$T5/formal.json" "$EVID/stage5-formal.json"
rm -rf "$T5"
log "[STAGE 5] PASS"

# ══════════════════ STAGE 6: git hook integration ══════════════════════
stage_banner 6 "git hook integration (poisoned rejection + clean pass + <50ms)"
T6=$(mktemp -d)
git init -q -b main "$T6/repo"
git -C "$T6/repo" config user.email "verify-suite@weft.dev"
git -C "$T6/repo" config user.name "weft-verify suite"
sh "$ROOT/tools/verify/hooks/install.sh" "$T6/repo/.git" > /dev/null
[ -x "$T6/repo/.git/hooks/pre-commit" ] || fail "hook installer did not wire pre-commit"

# poisoned commit must be rejected
mkdir -p "$T6/repo/src"
cp "$MANAGED/fixtures/poisoned/poison.c" "$T6/repo/src/hot.c"
cp "$MANAGED/fixtures/poisoned/poison.ts" "$T6/repo/src/hot.ts"
git -C "$T6/repo" add -A
POISON_RC=0
POISON_OUT=$(git -C "$T6/repo" commit -qm "poisoned" 2>&1) || POISON_RC=$?
[ "$POISON_RC" -ne 0 ] || fail "poisoned commit was ACCEPTED (hook failed to block)"
printf '%s' "$POISON_OUT" | grep -q "WV-C-001" || fail "poisoned rejection lacks diagnostic pointer (WV-C-001)"
log "  [ok] poisoned commit REJECTED with diagnostic pointer"

# clean commit must pass
git -C "$T6/repo" reset -q HEAD 2>/dev/null || true
rm -f "$T6/repo/src/hot.c" "$T6/repo/src/hot.ts"
cp "$MANAGED/fixtures/clean/clean.c" "$T6/repo/src/hot.c"
cp "$MANAGED/fixtures/clean/clean.ts" "$T6/repo/src/hot.ts"
git -C "$T6/repo" add -A
CLEAN_RC=0
CLEAN_OUT=$(git -C "$T6/repo" commit -qm "clean" 2>&1) || CLEAN_RC=$?
[ "$CLEAN_RC" -eq 0 ] || fail "clean commit was REJECTED: $CLEAN_OUT"
log "  [ok] clean commit PASSED"

# hook latency: best-of-3 < 50 ms on full CI; relaxed to 200 ms under
# WEFT_QUICK=1 (Android/Termux: awk process startup adds ~100-150 ms
# due to PRoot container overhead — same tolerance as ASan/TSan legs).
LATENCY_BUDGET=50
[ "${WEFT_QUICK:-0}" = "1" ] && LATENCY_BUDGET=200
git -C "$T6/repo" commit --amend -qm "clean (amended for latency probe)" > /dev/null 2>&1 || true
LATENCY_MS=999999
for i in 1 2 3; do
  T0=$(date +%s%N)
  sh "$T6/repo/.git/hooks/pre-commit" > /dev/null 2>&1
  T1=$(date +%s%N)
  MS=$(( (T1 - T0) / 1000000 ))
  [ "$MS" -lt "$LATENCY_MS" ] && LATENCY_MS=$MS
done
[ "$LATENCY_MS" -lt "$LATENCY_BUDGET" ] || fail "hook latency best-of-3 ${LATENCY_MS}ms >= ${LATENCY_BUDGET}ms budget"
log "  [ok] hook latency best-of-3: ${LATENCY_MS}ms < ${LATENCY_BUDGET}ms budget"

# non-source staged files must not trip the hook
echo "just docs" > "$T6/repo/README.md"
git -C "$T6/repo" add README.md
expect_success "non-source staged files pass instantly" sh -c "cd $T6/repo && .git/hooks/pre-commit"
rm -rf "$T6"
log "[STAGE 6] PASS"

# ═══════════ STAGE 7: payload & zero-runtime-dependency audit ══════════
stage_banner 7 "package payload & zero-runtime-dependency audit (< 100 KiB)"
PKG="$ROOT/packages/verify"
DEPS=$(node -e 'const p=require("'"$PKG"'/package.json"); process.stdout.write(JSON.stringify({d:p.dependencies||{},pe:p.peerDependencies||{},dev:p.devDependencies||{}}))')
[ "$DEPS" = '{"d":{},"pe":{},"dev":{}}' ] || fail "dependency audit: expected all empty, got $DEPS"
log "  [ok] dependencies {} / peerDependencies {} / devDependencies {}"

BYTES=$(find "$PKG" -type f -exec stat -c%s {} + | awk '{s+=$1} END {print s+0}')
[ "$BYTES" -lt 102400 ] || fail "payload $BYTES bytes >= 100 KiB budget"
KIB=$(( BYTES / 1024 ))
log "  [ok] payload: $BYTES bytes (${KIB} KiB) < 100 KiB"

BAD_IMPORTS=$( { grep -rhoE "from '[^']+'" "$PKG/src" | sed "s/from '//; s/'//" \
  | grep -v "^node:" | grep -v "^\./" | grep -v "^\.\./" || true; } | sort -u | grep -c . || true )
[ "$BAD_IMPORTS" -eq 0 ] || { grep -rhoE "from '[^']+'" "$PKG/src" | sort -u >> "$EVID/suite-run.log"; fail "non-relative, non-builtin imports found ($BAD_IMPORTS)"; }
log "  [ok] import audit: only node: builtins and relative .ts modules"

[ -e "$PKG/node_modules" ] && fail "node_modules present in package payload"
[ -e "$PKG/pnpm-lock.yaml" ] || [ -e "$PKG/package-lock.json" ] && fail "lockfile present in package payload"
expect_success "bin launcher is executable" test -x "$PKG/bin/weft-verify.mjs"
log "[STAGE 7] PASS"

# ═══════════════════════════ summary ═══════════════════════════════════
SUITE_END=$(date +%s)
WALL=$(( SUITE_END - SUITE_START ))
log ""
log "════════════════════════════════════════════════════════════"
log "  ALL 7 STAGES GREEN — weft-verify managed suite (exit 0)"
log "  wall clock: ${WALL}s | evidence: $EVID"
log "════════════════════════════════════════════════════════════"
node -e '
const fs = require("fs");
const ev = process.argv[1];
const summary = {
  schema: "weft-verify-suite-summary/1",
  verdict: "PASS",
  stages: 7,
  wallClockSeconds: Number(process.argv[2]),
  stage2: JSON.parse(fs.readFileSync(ev + "/stage2-verdict.json", "utf8")),
  stage3: JSON.parse(fs.readFileSync(ev + "/stage3-probe.json", "utf8")),
  stage5Wide: (function () {
    const f = JSON.parse(fs.readFileSync(ev + "/stage5-formal.json", "utf8"));
    return { explored: f.wide.explored, seconds: f.wide.seconds, verdict: f.verdict };
  })(),
  hookLatencyMsBestOf3: Number(process.argv[3]),
};
fs.writeFileSync(ev + "/pillar8-summary.json", JSON.stringify(summary, null, 2) + "\n");
' "$EVID" "$WALL" "$LATENCY_MS"
exit 0
