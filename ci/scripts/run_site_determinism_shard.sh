#!/usr/bin/env bash
# run_site_determinism_shard.sh — regenerate site twice, compare sha256.
# PASS iff byte-identical.
set -euo pipefail

mkdir -p ci/run-artifacts

# Build the site once
python3 tools/make_site.py 2>&1 | tee ci/run-artifacts/shard-site-determinism.log
FIRST_SHA=$(sha256sum bench/site/index.html | cut -d' ' -f1)
echo "First render sha256:  $FIRST_SHA" | tee -a ci/run-artifacts/shard-site-determinism.log

# Regenerate
python3 tools/make_site.py > /dev/null 2>&1
SECOND_SHA=$(sha256sum bench/site/index.html | cut -d' ' -f1)
echo "Second render sha256: $SECOND_SHA" | tee -a ci/run-artifacts/shard-site-determinism.log

# Compare
if [ "$FIRST_SHA" = "$SECOND_SHA" ]; then
  STATUS="PASSED"
  echo "✅ Site regeneration: deterministic (byte-identical)" | tee -a ci/run-artifacts/shard-site-determinism.log
else
  STATUS="FAILED"
  echo "❌ Site regeneration: NON-deterministic — release gate FAILS" | tee -a ci/run-artifacts/shard-site-determinism.log
fi

# Also verify no client-side JS in any page
JS_HITS=$( (grep -lE "<script|javascript:" bench/site/*.html 2>/dev/null || true) | grep -c . || true)
echo "Client-side JS hits: $JS_HITS (target: 0)" | tee -a ci/run-artifacts/shard-site-determinism.log

# Write results JSON
python3 -c "
import json, os, glob
pages = [os.path.basename(p) for p in sorted(glob.glob('bench/site/*.html'))]
out = {
    'shard': 'site-determinism',
    'status': '$STATUS',
    'first_sha256': '$FIRST_SHA',
    'second_sha256': '$SECOND_SHA',
    'byte_identical': '$FIRST_SHA' == '$SECOND_SHA',
    'client_side_js_hits': $JS_HITS,
    'pages': pages,
}
json.dump(out, open('ci/run-artifacts/shard-site-determinism-results.json', 'w'), indent=2)
print(json.dumps(out, indent=2))
"

[ "$STATUS" = "PASSED" ] || exit 1
