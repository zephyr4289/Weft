#!/usr/bin/env bash
# run_integration.sh — weftc Pillar 1 full verification pipeline.
#
# Every step is pipefail-guarded (silent-green = sabotage per repo policy).
# Pipeline:
#   1. IR loader + names tests            (node --test)
#   2. TS backend suite                   (node --test, --expose-gc for Law 1)
#   3. Python backend suite               (unittest)
#   4. Swift + Dart static audits         (node --test)
#   5. Cross-backend parity audit         (12-row matrix, exit 1 on drift)
#   6. Codegen determinism                (weftc --check vs committed goldens)
#   7. C kernel harness                   (build + golden verify + emit)
#   8. E2E integration legs               (TS + Python read the C-produced buffer)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "=== [1/8] IR loader + codegen lib ==="
node --test tools/weftc/codegen/lib/test/ir.test.mjs

echo "=== [2/8] TS backend suite (Law 1 heap probe via --expose-gc) ==="
node --expose-gc --test tools/weftc/codegen/ts/test/views.test.mjs

echo "=== [3/8] Python backend suite ==="
python3 tools/weftc/codegen/python/test/views_test.py

echo "=== [4/8] Swift + Dart static audits ==="
node --test tools/weftc/codegen/swift/test/swift_audit.test.mjs
node --test tools/weftc/codegen/dart/test/dart_audit.test.mjs

echo "=== [5/8] Cross-backend parity audit (3 fixtures x 4 backends) ==="
node tools/weftc/audit/parity_audit.mjs

echo "=== [6/8] Codegen determinism gate (--check) ==="
node tools/weftc/weftc.mjs --ir tools/weftc/schema/fixtures --target all --out tools/weftc/codegen --check

echo "=== [7/8] C kernel harness (frozen kernel linked, never modified) ==="
CC_BIN="${CC:-cc}"
"$CC_BIN" -std=c11 -I core/c -o "$TMP/weftc_verify" \
  tools/weftc/tests/harness/verify_golden.c core/c/weft.c
"$TMP/weftc_verify" tools/weftc/tests/golden
"$CC_BIN" -std=c11 -I core/c -o "$TMP/weftc_emit" \
  tools/weftc/tests/harness/emit_frame_stack.c core/c/weft.c
export WEFTC_EMIT_BIN="$TMP/weftc_emit"
"$WEFTC_EMIT_BIN" "$TMP/kernel_stack.bin"
cmp "$TMP/kernel_stack.bin" tools/weftc/tests/golden/weft_frame_stack.bin

echo "=== [8/8] E2E: managed views read the kernel-produced buffer ==="
node --test tools/weftc/codegen/ts/test/integration.test.mjs
python3 tools/weftc/codegen/python/test/integration_test.py

echo "=== weftc Pillar 1: ALL PIPELINES GREEN ==="
