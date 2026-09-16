#!/usr/bin/env bash
# run_browser_sab_shard.sh — the real-browser SAB/COOP-COEP leg (Series 7).
#
# PROVES, in a stock Chromium (Playwright), what node's vitest cannot:
#   1. crossOriginIsolated === true under COOP same-origin + COEP require-corp
#      (WHITEPAPER §8.2's SAB requirement, verified in the browser, not assumed)
#   2. the REAL @weft/core dist build runs a kernel publish/claim roundtrip
#      over SAB-backed buffers in-page
#   3. a 5000-frame 1W/2R fan-out stream with pat() validation, torn-accepted
#      == 0, telescoping identity intact — all inside the isolated context
#
# ISOLATION OF THE BROWSER DEPENDENCY: playwright installs into a scratch
# npm project under ci/browser-tmp (git-ignored) — the workspace's
# pnpm-lock.yaml stays untouched (the frozen-install lesson from the
# verifiedweft shard).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG=ci/run-artifacts/shard-browser-sab.log
: > "$LOG"
step() { echo "=== $1 ===" | tee -a "$LOG"; }

# --- 1. Build @weft/core if the dist is missing (the shard is standalone) ---
step "ensure @weft/core dist"
if [ ! -f packages/core/dist/index.mjs ]; then
  pnpm --filter @weft/core build 2>&1 | tee -a "$LOG"
fi
[ -f packages/core/dist/index.mjs ] || { echo "dist missing after build" | tee -a "$LOG"; exit 1; }

# --- 2. Playwright in a scratch project (workspace untouched) ---
step "provision playwright (scratch project, git-ignored)"
mkdir -p ci/browser-tmp
cd ci/browser-tmp
if [ ! -f package.json ]; then
  npm init -y >/dev/null 2>&1
fi
npm install playwright-core@latest --no-fund --no-audit 2>&1 | tee -a "$LOG"
# Chromium binary for playwright-core (core does not bundle browsers).
npx playwright@latest install chromium --with-deps 2>&1 | tail -5 | tee -a "$LOG" || \
  npx playwright@latest install chromium 2>&1 | tail -5 | tee -a "$LOG"
cd "$ROOT"

# --- 3. Serve with COOP/COEP and probe in the browser ---
step "harness server (COOP/COEP) + chromium probe"
set +e
node tools/browser-harness/server.mjs packages/core/dist 8123 2>&1 | tee -a "$LOG" &
SERVER_PID=$!
for i in $(seq 1 50); do
  if curl -s -o /dev/null http://localhost:8123/; then break; fi
  sleep 0.2
done
node tools/browser-harness/probe.mjs 8123 2>&1 | tee -a "$LOG"
PROBE_RC=${PIPESTATUS[0]}
wait "$SERVER_PID"
SERVER_RC=$?
set -e

if [ "$PROBE_RC" -ne 0 ] || [ "$SERVER_RC" -ne 0 ]; then
  echo '{"shard":"browser-sab","status":"FAILED","probe":'"$PROBE_RC"',"server":'"$SERVER_RC"'}' \
    > ci/run-artifacts/shard-browser-sab-results.json
  echo "❌ browser-sab shard FAILED" | tee -a "$LOG"
  exit 1
fi

echo '{"shard":"browser-sab","status":"PASSED","proven":["crossOriginIsolated","SAB kernel roundtrip","5000-frame 1W/2R fanout torn=0"]}' \
  > ci/run-artifacts/shard-browser-sab-results.json
echo "✅ browser-sab shard PASSED" | tee -a "$LOG"
