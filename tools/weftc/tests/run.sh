#!/usr/bin/env bash
# tests/run.sh — the weftc gate (RFC-0017 §8). Runs the three in-process
# suites (WL/WH/WD) plus the CLI, external-JSON, and verify-header
# handshake gates. Works for both the plain and the ASAN build (the
# Makefile test-asan target rebuilds every binary with -fsanitize=address
# then re-runs this script).
#
# Exit non-zero on ANY failure. Output: human-readable steps, house style.
set -uo pipefail

cd "$(dirname "$0")/.."

fail=0
step() { echo ""; echo "=== $1 ==="; }

step "WL-series: layout conformance"
./tests/test_layout || fail=1

step "WH-series: hash quality (avalanche, mutations, collisions)"
./tests/test_hash || fail=1

step "WD-series: emitters + golden pinning"
./tests/test_dump || fail=1

step "CLI gates: exit codes + artifact determinism"
# clean check exits 0
if ./weftc check tests/golden/frames.weft > /dev/null; then
    echo "PASS: clean check exits 0"
else
    echo "FAIL: clean check did not exit 0"; fail=1
fi
# missing file -> WE039, exit 1
if ./weftc check tests/golden/does-not-exist.weft 2>/dev/null; then
    echo "FAIL: missing file accepted"; fail=1
else
    echo "PASS: missing file rejected (exit 1)"
fi
# unknown command / no args -> usage, exit 2
if ./weftc frobnicate 2>/dev/null; then
    echo "FAIL: unknown command accepted"; fail=1
else
    echo "PASS: unknown command rejected (exit 2)"
fi
if ./weftc 2>/dev/null; then
    echo "FAIL: bare invocation accepted"; fail=1
else
    echo "PASS: bare invocation prints usage (exit 2)"
fi
# artifacts byte-match the goldens (same input path => same IR)
./weftc compile tests/golden/frames.weft \
    --emit-json /tmp/weftc-gate.json \
    --emit-ir /tmp/weftc-gate.weftir \
    --emit-header /tmp/weftc-gate.verify.h > /dev/null || fail=1
cmp -s /tmp/weftc-gate.json tests/golden/frames.json \
    && echo "PASS: CLI JSON artifact byte-identical to golden" \
    || { echo "FAIL: CLI JSON artifact drift"; fail=1; }
cmp -s /tmp/weftc-gate.weftir tests/golden/frames.weftir \
    && echo "PASS: CLI binary IR byte-identical to golden" \
    || { echo "FAIL: CLI binary IR drift"; fail=1; }
cmp -s /tmp/weftc-gate.verify.h tests/golden/frames.verify.h \
    && echo "PASS: CLI verify header byte-identical to golden" \
    || { echo "FAIL: CLI verify header drift"; fail=1; }

step "JSON IR validated by an external parser"
if command -v python3 > /dev/null; then
    python3 -c "
import json
d = json.load(open('tests/golden/frames.json'))
assert d['ir_version'] == 1
assert d['decl_count'] == 7
assert d['abi_hash'].startswith('0x') and len(d['abi_hash']) == 18
print('python3 json.load: VALID — 7 decls, schema_id', d['schema_id'])
" || fail=1
else
    echo "SKIP: python3 not available (declared)"
fi

step "verify-header handshake demo — consumer side (positive)"
if cc -std=c11 -Wall -Wextra -Itests/golden \
       -c tests/golden/verify_consumer.c -o /tmp/weftc-consumer.o 2>/tmp/weftc-consumer.err
then
    echo "PASS: hand-written C structs match every pin (compile clean)"
else
    echo "FAIL: consumer did not compile:"; cat /tmp/weftc-consumer.err; fail=1
fi

step "verify-header drift detection (negative — MUST fail to compile)"
sed '/uint64_t stamp;/d; s/    Kind     kind;/    uint64_t stamp;\n    Kind     kind;/' \
    tests/golden/verify_consumer.c > /tmp/weftc-drift.c
if cc -std=c11 -Itests/golden -c /tmp/weftc-drift.c -o /tmp/weftc-drift.o 2>/tmp/weftc-drift.err
then
    echo "FAIL: drifted consumer compiled — the pins are not biting"; fail=1
else
    if grep -q "drifted" /tmp/weftc-drift.err; then
        echo "PASS: layout drift rejected AT COMPILE TIME:"
        grep -m1 "error: static assertion failed" /tmp/weftc-drift.err || true
    else
        echo "FAIL: rejected for the wrong reason:"; head -5 /tmp/weftc-drift.err; fail=1
    fi
fi

echo ""
if [ "$fail" -eq 0 ]; then
    echo "weftc gate: ALL PASS"
else
    echo "weftc gate: FAILURES PRESENT"
fi
exit "$fail"
