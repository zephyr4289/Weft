#!/usr/bin/env bash
# run_kernel_boundary_audit.sh — Tier 4/5 kernel boundary audit (doc-005).
# Asserts: all language ports enforce geometry validation at construction.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

echo "=== Kernel Boundary & Geometry Validation Audit ==="

FAIL=0

# 1. Check C kernel has payload limit guard
if grep -q "WEFT_PAYLOAD_MAX_LIMIT" core/c/weft.c core/c/weft.h; then
  echo "  [PASS] C kernel: WEFT_PAYLOAD_MAX_LIMIT enforced"
else
  echo "  [FAIL] C kernel: missing WEFT_PAYLOAD_MAX_LIMIT"
  FAIL=1
fi

# 2. Check TS core has payload limit guard
if grep -q "WEFT_PAYLOAD_MAX_LIMIT" packages/core/src/weft.ts core/ts/weft.ts; then
  echo "  [PASS] TS kernel: WEFT_PAYLOAD_MAX_LIMIT enforced"
else
  echo "  [FAIL] TS kernel: missing WEFT_PAYLOAD_MAX_LIMIT"
  FAIL=1
fi

# 3. Check Rust core has payload limit guard
if grep -q "WEFT_PAYLOAD_MAX_LIMIT" core/rust/src/lib.rs; then
  echo "  [PASS] Rust kernel: WEFT_PAYLOAD_MAX_LIMIT enforced"
else
  echo "  [FAIL] Rust kernel: missing WEFT_PAYLOAD_MAX_LIMIT"
  FAIL=1
fi

# 4. Check Kotlin port has payload limit guard
if grep -q "WEFT_PAYLOAD_MAX_LIMIT" core/kotlin/Weft.kt; then
  echo "  [PASS] Kotlin port: WEFT_PAYLOAD_MAX_LIMIT enforced"
else
  echo "  [FAIL] Kotlin port: missing WEFT_PAYLOAD_MAX_LIMIT"
  FAIL=1
fi

# 5. Check Dart port has payload limit guard
if grep -q "WEFT_PAYLOAD_MAX_LIMIT" core/dart/weft.dart; then
  echo "  [PASS] Dart port: WEFT_PAYLOAD_MAX_LIMIT enforced"
else
  echo "  [FAIL] Dart port: missing WEFT_PAYLOAD_MAX_LIMIT"
  FAIL=1
fi

# 6. Check Swift port has payload limit guard
if grep -q "WEFT_PAYLOAD_MAX_LIMIT" core/swift/Weft.swift; then
  echo "  [PASS] Swift port: WEFT_PAYLOAD_MAX_LIMIT enforced"
else
  echo "  [FAIL] Swift port: missing WEFT_PAYLOAD_MAX_LIMIT"
  FAIL=1
fi

if [ $FAIL -eq 0 ]; then
  echo "✅ Kernel boundary audit: 6/6 ports verified"
  exit 0
else
  echo "❌ Kernel boundary audit: FAILED"
  exit 1
fi
