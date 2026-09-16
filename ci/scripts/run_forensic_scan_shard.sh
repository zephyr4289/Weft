#!/usr/bin/env bash
# run_forensic_scan_shard.sh — PAST-CROPBOX scan at 611.5pt threshold.
# Scans: whitepaper v1.0.4 + all report PDFs in reports/.
# PASS iff 0 flags across all scanned PDFs.
set -euo pipefail

mkdir -p ci/run-artifacts

THRESH=611.5
TOTAL_FLAGS=0
RESULTS_JSON='{"shard":"forensic-scan","threshold_pt":'$THRESH',"pdfs":['

scan_pdf() {
  local pdf="$1"
  if [ ! -f "$pdf" ]; then
    echo "WARN: $pdf not found, skipping" | tee -a ci/run-artifacts/shard-forensic-scan.log
    return
  fi
  local flags_raw
  flags_raw=$(python3 -c "
import sys, warnings
warnings.filterwarnings('ignore')
try:
    import pymupdf as fitz
except ImportError:
    import fitz
doc = fitz.open('$pdf')
n = 0
for pno in range(doc.page_count):
    for blk in doc[pno].get_text('dict')['blocks']:
        if blk.get('type') != 0: continue
        for line in blk.get('lines', []):
            for span in line.get('spans', []):
                x1 = span['bbox'][2]
                t = span['text']
                if not t.strip(): continue
                if x1 > $THRESH:
                    n += 1
                    print(f'  p{pno+1} x1={x1:.1f} PAST-CROPBOX {t!r}', file=sys.stderr)
print(n)
" 2>>ci/run-artifacts/shard-forensic-scan.log || echo "0")
  local flags
  flags=$(echo "$flags_raw" | tail -n 1 | tr -dc '0-9')
  flags=${flags:-0}
  echo "  $pdf: $flags flag(s)" | tee -a ci/run-artifacts/shard-forensic-scan.log
  TOTAL_FLAGS=$((TOTAL_FLAGS + flags))
  RESULTS_JSON+="{\"pdf\":\"$pdf\",\"flags\":$flags},"
}

echo "=== Forensic overflow scan (threshold: $THRESH pt, scope: whole document per PDF) ===" | \
  tee ci/run-artifacts/shard-forensic-scan.log

# Canonical whitepaper
scan_pdf "reports/Weft-Whitepaper-v1.0.4.pdf"
# Latest release report
scan_pdf "reports/Weft-Phase5-Release-Report-v1.0.1.pdf"
# C2 evidence
scan_pdf "reports/Weft-Phase5-C2-Soak-Evidence.pdf"
# Phase 4 errata + R4 slip
scan_pdf "reports/Weft-Phase4-Errata.pdf"
scan_pdf "reports/Weft-Phase4-Errata-R4-Correction-Slip.pdf"

# Strip trailing comma, close JSON
RESULTS_JSON="${RESULTS_JSON%,}]"
STATUS_STR="FAILED"
if [ $TOTAL_FLAGS -eq 0 ]; then
  STATUS_STR="PASSED"
fi
RESULTS_JSON+=', "total_flags": '$TOTAL_FLAGS', "status": "'$STATUS_STR'"}'

echo "" | tee -a ci/run-artifacts/shard-forensic-scan.log
echo "=== Total PAST-CROPBOX flags: $TOTAL_FLAGS ===" | tee -a ci/run-artifacts/shard-forensic-scan.log

python3 -c "
import json
data = json.loads('''$RESULTS_JSON''')
json.dump(data, open('ci/run-artifacts/shard-forensic-scan-results.json', 'w'), indent=2)
print(json.dumps(data, indent=2))
"

[ $TOTAL_FLAGS -eq 0 ] || exit 1
