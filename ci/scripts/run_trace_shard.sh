#!/usr/bin/env bash
# run_trace_shard.sh — RFC 0014 trace-standard gate (issue #20, task 1).
#
# 1. codec conformance: core/c/trace_rec-test (round trip, CRC catch,
#    version gate, kind gate, exact size, JSON export, CRC vector)
# 2. cross-port byte-identity: fixtures/xlang-trace/run.sh — the same
#    deterministic scenario through every available port (TS reference
#    required; C always; Kotlin/Dart/Swift declared-skip without
#    toolchains, CI owns them) — any divergence is RED
# 3. container round trip: .weftrec v4 write -> validate -> JSON export
#    shape check against schemas/weftrec-trace.schema.json
#
# Output: ci/run-artifacts/shard-trace.log + shard-trace-results.json
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
mkdir -p ci/run-artifacts
LOG="$ROOT/ci/run-artifacts/shard-trace.log"
: > "$LOG"

FAIL=0

step() { echo "" >> "$LOG"; echo "=== $* ===" | tee -a "$LOG"; }

step "Build: trace_rec-test + trace-dump (C)"
(cd core/c && gcc -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
    -o trace_rec-test trace_rec.c trace_rec_test.c 2>>"$LOG" && \
    gcc -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
    -o trace-dump trace_dump.c trace_rec.c weft.c 2>>"$LOG") \
  && echo "  PASS build" | tee -a "$LOG" || { echo "  FAIL build" | tee -a "$LOG"; FAIL=1; }

step "Codec conformance (T-series)"
if (cd core/c && ./trace_rec-test) >> "$LOG" 2>&1; then
  echo "  PASS trace_rec-test" | tee -a "$LOG"
else
  echo "  FAIL trace_rec-test" | tee -a "$LOG"; FAIL=1
fi

step "Cross-port byte-identity (xlang-trace)"
if bash fixtures/xlang-trace/run.sh >> "$LOG" 2>&1; then
  echo "  PASS xlang-trace" | tee -a "$LOG"
else
  echo "  FAIL xlang-trace" | tee -a "$LOG"; FAIL=1
fi

step "Container -> JSON export -> schema shape"
if (cd core/c && ./trace-dump json 500 2>/dev/null) > /tmp/weft-trace.jsonl 2>>"$LOG" \
   && python3 - "$LOG" << 'PYEOF' >> "$LOG" 2>&1
import json, sys
lines = open('/tmp/weft-trace.jsonl').read().strip().split('\n')
h = json.loads(lines[0])
assert h['format'] == 'weftrec' and h['version'] == 4 and h['kind'] == 'trace', h
for ln in lines[1:]:
    e = json.loads(ln)
    assert set(e) == {'i', 'kind', 'aux', 'data'}, e
    assert e['kind'] in ['publish', 'claim', 'drop', 'revoke', 'ack', 'stall', 'tear', 'canary_fail']
print(f"  PASS schema shape ({len(lines)-1} events)")
PYEOF
then
  :
else
  echo "  FAIL schema shape" | tee -a "$LOG"; FAIL=1
fi

python3 - "$LOG" << 'PYEOF'
import json, sys, re
log = open(sys.argv[1]).read()
ok = 'FAIL' not in re.sub(r'"\w+"', '', '') # placeholder; simple scan below
ok = not any(line.startswith('  FAIL') for line in log.split('\n'))
out = {'shard': 'trace-standard', 'status': 'PASSED' if ok else 'FAILED'}
json.dump(out, open('ci/run-artifacts/shard-trace-results.json', 'w'), indent=2)
print(json.dumps(out, indent=2))
sys.exit(0 if ok else 1)
PYEOF
[ $FAIL -eq 0 ] || exit 1
