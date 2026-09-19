#!/usr/bin/env bash
# verify-wasm-demo-ci.sh — the browser leg of the wasm-port workflow.
# Serves demos/wasm-canvas, opens it in headless chromium, requires:
#   - zero page errors
#   - advancing counters (the fan-out loop is live)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/demos/wasm-canvas"

python3 -m http.server 8788 --bind 127.0.0.1 >/tmp/wasm-demo-http.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1

agent-browser open http://127.0.0.1:8788/index.html
sleep 4

ERRORS=$(agent-browser errors | grep -c . || true)
if [ "$ERRORS" != "0" ]; then
  agent-browser errors
  echo "FAIL: page errors in the wasm demo" >&2
  exit 1
fi

C1=$(agent-browser eval "document.getElementById('seq').textContent")
sleep 3
C2=$(agent-browser eval "document.getElementById('seq').textContent")
agent-browser close

echo "seq counter: $C1 -> $C2"
if [ "$C1" = "$C2" ] || [ "$C1" = "0" ]; then
  echo "FAIL: counters not advancing (fan-out loop dead?)" >&2
  exit 1
fi
echo "✅ wasm demo verified in headless chromium (live fan-out, zero errors)"
