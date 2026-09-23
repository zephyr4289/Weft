#!/usr/bin/env bash
# run_heddle_canvas_shard.sh — the heddle-canvas pillar shard (Pillar 4 /
# RFC-0022). Six legs, every one named and gated:
#
#   1. UNIT     — the vitest battery (plane contract, dirty protocol,
#                 budget, engine-on-NullHAL, shaders drift, oracle, layout
#                 parity vs the native header).
#   2. LAW1     — instrument calibration (a planted allocator MUST be
#                 caught; a clean loop MUST read zero) then the 60,000-
#                 frame Law-1 bench: 0 B engine allocations + 0 frame-
#                 budget violations under sustained 100k samples/sec.
#   3. NATIVE   — the Vulkan probe: WHP1 plane as one SSBO, decimation
#                 bit-exact vs the C oracle (lavapipe ICD; exit 3 = no
#                 ICD = named skip, never a silent pass).
#   4. BROWSER  — real Chromium under COOP/COEP: crossOriginIsolated,
#                 tier ladder, WebGL2 TF + Canvas2D cross-tier ==-gate
#                 (bit-exact hashes), dirty-skip proof (SwiftShader-class
#                 rasterizers; WebGPU = named refusal when no adapter).
#   5. FREEZE   — core/c/weft.{c,h} byte-frozen (Law 3).
#
# Browser isolation follows the Series-7 browser-sab pattern: playwright
# installs into a scratch npm project under ci/browser-tmp (git-ignored);
# the workspace pnpm-lock.yaml is untouched.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG="$ROOT/ci/run-artifacts/shard-heddle-canvas.log"
: > "$LOG"
step() { echo "=== $1 ===" | tee -a "$LOG"; }
PKG="$ROOT/packages/heddle-canvas"

step "1/5 unit battery (vitest)"
cd "$PKG"
(pnpm test || npx vitest run || npm run test) 2>&1 | tee -a "$LOG" | tail -4
cd "$ROOT"

step "2/5 Law-1: instrument calibration + 60,000-frame gate"
cd "$PKG"
node --experimental-strip-types scripts/law1_calibrate.ts 2>&1 | tee -a "$LOG"
node --expose-gc --experimental-strip-types bench/frame_budget.ts 2>&1 | tee -a "$LOG" | grep -E "^(law1|frame-budget|engine-stats|tier3|\{)"
cd "$ROOT"

step "3/5 native Vulkan probe (WHP1 plane as SSBO, bit-exact)"
cd "$PKG/native"
if command -v cc >/dev/null 2>&1; then
  make -s clean >/dev/null 2>&1 || true
  make -s 2>&1 | tee -a "$LOG" || { echo "native build FAILED" | tee -a "$LOG"; exit 1; }
  set +e
  ./vk-heddle-probe 2>&1 | tee -a "$LOG"
  VK_EXIT=$?
  set -e
  if [ "$VK_EXIT" -eq 3 ]; then
    echo "vk-probe: no ICD present — NAMED SKIP (install mesa-vulkan-drivers / set VK_ICD_FILENAMES)" | tee -a "$LOG"
  elif [ "$VK_EXIT" -ne 0 ]; then
    echo "vk-probe FAILED (exit $VK_EXIT)" | tee -a "$LOG"
    exit 1
  fi
else
  echo "cc absent — native leg NAMED SKIP" | tee -a "$LOG"
fi
cd "$ROOT"

step "4/5 browser rig (Chromium + COOP/COEP + SwiftShader)"
mkdir -p ci/browser-tmp
cd ci/browser-tmp
if [ ! -f package.json ]; then npm init -y >/dev/null 2>&1; fi
npm install playwright-core@latest esbuild@latest --no-fund --no-audit 2>&1 | tail -1 | tee -a "$LOG" >/dev/null || true
npx playwright-core install chromium 2>&1 | tail -2 | tee -a "$LOG" >/dev/null || true
cd "$PKG"
if [ -f "$ROOT/ci/browser-tmp/node_modules/.bin/esbuild" ]; then
  ESBUILD="$ROOT/ci/browser-tmp/node_modules/.bin/esbuild"
elif [ -f "$PKG/node_modules/.bin/esbuild" ]; then
  ESBUILD="$PKG/node_modules/.bin/esbuild"
elif [ -f "$ROOT/node_modules/.bin/esbuild" ]; then
  ESBUILD="$ROOT/node_modules/.bin/esbuild"
elif command -v esbuild >/dev/null 2>&1; then
  ESBUILD="esbuild"
else
  ESBUILD="npx --yes --package=esbuild esbuild"
fi
$ESBUILD rig/heddle_rig_main.ts --bundle --format=esm \
  --outfile=rig/heddle_rig_bundle.mjs --platform=browser --external:node:inspector \
  --log-level=error 2>&1 | tee -a "$LOG" || { echo "rig bundle FAILED" | tee -a "$LOG"; exit 1; }
node rig/serve.mjs &
RIG_SERVER=$!
trap 'kill $RIG_SERVER 2>/dev/null || true' EXIT
sleep 1
cat > "$ROOT/ci/browser-tmp/run_heddle_rig.mjs" <<'RUNNER'
import { chromium } from 'playwright-core';
const browser = await chromium.launch({
  headless: true,
  args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--enable-features=Vulkan,WebGPU', '--ignore-gpu-blocklist'],
});
const page = await browser.newPage();
page.on('pageerror', (e) => console.error('PAGE-ERROR', String(e)));
await page.goto('http://127.0.0.1:8931/', { waitUntil: 'load' });
const line = await page.waitForEvent('console', {
  timeout: 120000,
  predicate: (m) => m.text().startsWith('HEDDLE-RIG-VERDICT'),
}).then((m) => m.text()).catch(() => { console.error('NO-VERDICT'); process.exit(1); });
console.log(line);
const verdict = JSON.parse(line.slice('HEDDLE-RIG-VERDICT '.length));
await browser.close();
process.exit(verdict.pass ? 0 : 1);
RUNNER
cd "$ROOT/ci/browser-tmp"
set +e
node run_heddle_rig.mjs 2>&1 | tee -a "$LOG" | grep -E "HEDDLE-RIG-VERDICT" | head -1
RIG_EXIT=$?
set -e
kill $RIG_SERVER 2>/dev/null || true
trap - EXIT
if [ "$RIG_EXIT" -ne 0 ]; then
  echo "browser rig FAILED" | tee -a "$LOG"
  exit 1
fi
cd "$ROOT"

step "5/5 kernel freeze (core/c/weft.{c,h} byte-frozen — Law 3)"
if git rev-parse --verify origin/main >/dev/null 2>&1; then
  if git diff --quiet origin/main -- core/c/weft.c core/c/weft.h; then
    echo "kernel freeze: 0 diffs vs origin/main — PASS" | tee -a "$LOG"
  else
    echo "kernel freeze: core/c/weft.{c,h} DIFFERS from origin/main — FAIL" | tee -a "$LOG"
    exit 1
  fi
else
  echo "kernel freeze: origin/main absent (local run) — SKIPPED, named" | tee -a "$LOG"
fi

echo "heddle-canvas shard: ALL LEGS GREEN" | tee -a "$LOG"
