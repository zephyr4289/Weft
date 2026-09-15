"""Scan whitepaper PDF for span geometry overflow per WO-P4-CLOSURE P4-W1.

Mechanical criterion: flag any line whose span x1 exceeds 611.5pt
(within 0.5pt of the 612.0pt physical page edge — ink touching the cropbox).

Lines past the 540pt body margin but short of 611.5pt are benign typographic
protrusion: whitelisted, not findings.

Verifies: whole document (all pages), with the named numeric threshold.
"""
import sys, fitz

PDF = sys.argv[1] if len(sys.argv) > 1 else "/home/z/my-project/download/Weft-Whitepaper-v1.0.2.pdf"
THRESH = 611.5

doc = fitz.open(PDF)
print(f"# Whitepaper overflow scan")
print(f"# Artifact:  {PDF}")
print(f"# Criterion: span x1 > {THRESH}pt  (612.0pt = physical page edge)")
print(f"# Scope:     whole document, all {doc.page_count} pages, every text span")
print(f"# Protrusion lines (540 < x1 <= 611.5) are whitelisted (benign).")
print()
flagged = 0
protrusion_count = 0
for pno in range(doc.page_count):
    page = doc[pno]
    pw = page.rect.width
    ph = page.rect.height
    if abs(pw - 612.0) > 1.0:
        # Not US-Letter sized? print anyway
        pass
    blocks = page.get_text("dict")["blocks"]
    for blk in blocks:
        if blk.get("type") != 0: continue
        for line in blk.get("lines", []):
            for span in line.get("spans", []):
                x1 = span["bbox"][2]
                text = span["text"]
                if not text.strip():
                    continue
                if x1 > THRESH:
                    flagged += 1
                    print(f"  p{pno+1:>2} x1={x1:.1f}/{pw:.1f}  PAST-CROPBOX  {text!r}")
                elif x1 > 540.0:
                    protrusion_count += 1
print()
print(f"# Flagged lines (x1 > {THRESH}): {flagged}")
print(f"# Protrusion lines (540 < x1 <= {THRESH}, whitelisted): {protrusion_count}")
print(f"# Method: PyMuPDF {fitz.__version__}, page_count={doc.page_count}")
print(f"# Scope:  all pages, whole document")
print(f"# Threshold: {THRESH}pt (pinned per WO-P4-CLOSURE P4-W1)")
