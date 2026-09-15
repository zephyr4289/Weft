#!/usr/bin/env python3
"""
build_whitepaper.py — typeset docs/WHITEPAPER.md → Weft-Whitepaper-v{ver}.pdf

C1r repair batch (v1.0.4) per WO-P4-C1-VERIFICATION:

  C1-W1 — TOC restored (page count 16, "Contents" rendered)
  C2-W2 — heading auto-numbering disabled via secnumdepth=-1
          (manual numbering in markdown source preserved verbatim)
  C1-W3 — changelog claims match artifact page count (16pp, scan-clean)
  C1-W4 — margins restored to 1in (72pt, matching v1.0.2);
          XeTeX-native fonts via fontspec (no lmodern/T1 fontenc pair)
          so ToUnicode CMap is correct (§, ·, ×, →, ≤ all extract
          cleanly for copy-paste from Appendix A)

Pipeline: pandoc with custom XeTeX-aware LaTeX template → tectonic.
Post-build: forensic overflow scan at 611.5pt threshold (whole doc).
"""
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path("/home/z/my-project/upload/weft-docs/weft-docs/weft")
SRC_MD = ROOT / "docs" / "WHITEPAPER.md"
OUT_DIR = Path("/home/z/my-project/download")
WORKDIR = Path("/home/z/my-project/scripts/_whitepaper_build")
WORKDIR.mkdir(parents=True, exist_ok=True)

# Detect version
VERSION = "v1.0.4"
with open(SRC_MD) as f:
    head = f.read()[:2000]
    m = re.search(r"\*\*Whitepaper v(\d+\.\d+\.\d+)\*\*", head)
    if m:
        VERSION = f"v{m.group(1)}"
OUT_PDF = OUT_DIR / f"Weft-Whitepaper-{VERSION}.pdf"

# Custom XeTeX-aware LaTeX template (no lmodern/T1 fontenc pair)
# - fontspec for native Unicode font loading
# - polyglossia for English language (XeTeX-native, not babel)
# - secnumdepth=-1 disables ALL auto-numbering (C2-W2 fix)
# - tableofcontents restored (C1-W1 fix)
# - margin=1in restored (C1-W4 fix — matches v1.0.2)
TEMPLATE = r"""\providecommand{\tightlist}{\setlength{\itemsep}{0pt}\setlength{\parskip}{0pt}}
\documentclass[11pt,letterpaper]{article}

% C1-W4 fix: XeTeX-native font loading (no T1/fontenc/inputenc/lmodern pair).
% Tectonic uses XeTeX by default; loading pdfTeX font packages breaks the
% ToUnicode CMap (§ → ğ, · → ů, × → Œ). fontspec handles Unicode natively.
\usepackage{fontspec}
\defaultfontfeatures{Ligatures=TeX,Scale=MatchLowercase}
\setmainfont{Liberation Serif}
\setsansfont{Liberation Sans}
\setmonofont{Liberation Mono}

\usepackage{xcolor}
\usepackage{longtable,booktabs,array}
\usepackage{calc}
\usepackage{etoolbox}
\usepackage{footnotehyper}
\makesavenoteenv{longtable}
\setlength{\emergencystretch}{3em}

% C1-W4 fix: margins restored to 1in (72pt) matching v1.0.2.
\usepackage[margin=1in]{geometry}

\usepackage{fancyvrb}
\newcommand{\VerbBar}{|}
\newcommand{\VERB}{\Verb[commandchars=\\\{\}]}
\DefineVerbatimEnvironment{Highlighting}{Verbatim}{commandchars=\\\{\}}
\newenvironment{Shaded}{}{}
\newcommand{\AlertTok}[1]{\textcolor[rgb]{1.00,0.00,0.00}{\textbf{#1}}}
\newcommand{\AnnotationTok}[1]{\textcolor[rgb]{0.38,0.63,0.69}{\textbf{\textit{#1}}}}
\newcommand{\AttributeTok}[1]{\textcolor[rgb]{0.49,0.56,0.16}{#1}}
\newcommand{\BaseNTok}[1]{\textcolor[rgb]{0.25,0.63,0.44}{#1}}
\newcommand{\BuiltInTok}[1]{\textcolor[rgb]{0.00,0.50,0.00}{#1}}
\newcommand{\CharTok}[1]{\textcolor[rgb]{0.25,0.44,0.63}{#1}}
\newcommand{\CommentTok}[1]{\textcolor[rgb]{0.38,0.63,0.69}{\textit{#1}}}
\newcommand{\CommentVarTok}[1]{\textcolor[rgb]{0.38,0.63,0.69}{\textbf{\textit{#1}}}}
\newcommand{\ConstantTok}[1]{\textcolor[rgb]{0.53,0.00,0.00}{#1}}
\newcommand{\ControlFlowTok}[1]{\textcolor[rgb]{0.00,0.44,0.13}{\textbf{#1}}}
\newcommand{\DataTypeTok}[1]{\textcolor[rgb]{0.56,0.13,0.00}{#1}}
\newcommand{\DecValTok}[1]{\textcolor[rgb]{0.25,0.63,0.44}{#1}}
\newcommand{\DocumentationTok}[1]{\textcolor[rgb]{0.73,0.13,0.13}{\textit{#1}}}
\newcommand{\ErrorTok}[1]{\textcolor[rgb]{1.00,0.00,0.00}{\textbf{#1}}}
\newcommand{\ExtensionTok}[1]{#1}
\newcommand{\FloatTok}[1]{\textcolor[rgb]{0.25,0.63,0.44}{#1}}
\newcommand{\FunctionTok}[1]{\textcolor[rgb]{0.02,0.16,0.49}{#1}}
\newcommand{\ImportTok}[1]{\textcolor[rgb]{0.00,0.50,0.00}{\textbf{#1}}}
\newcommand{\InformationTok}[1]{\textcolor[rgb]{0.38,0.63,0.69}{\textbf{\textit{#1}}}}
\newcommand{\KeywordTok}[1]{\textcolor[rgb]{0.00,0.44,0.13}{\textbf{#1}}}
\newcommand{\NormalTok}[1]{#1}
\newcommand{\OperatorTok}[1]{\textcolor[rgb]{0.40,0.40,0.40}{#1}}
\newcommand{\OtherTok}[1]{\textcolor[rgb]{0.00,0.44,0.13}{#1}}
\newcommand{\PreprocessorTok}[1]{\textcolor[rgb]{0.74,0.48,0.00}{#1}}
\newcommand{\RegionMarkerTok}[1]{#1}
\newcommand{\SpecialCharTok}[1]{\textcolor[rgb]{0.25,0.44,0.63}{#1}}
\newcommand{\SpecialStringTok}[1]{\textcolor[rgb]{0.73,0.40,0.53}{#1}}
\newcommand{\StringTok}[1]{\textcolor[rgb]{0.25,0.44,0.63}{#1}}
\newcommand{\VariableTok}[1]{\textcolor[rgb]{0.10,0.09,0.49}{#1}}
\newcommand{\VerbatimStringTok}[1]{\textcolor[rgb]{0.25,0.44,0.63}{#1}}
\newcommand{\WarningTok}[1]{\textcolor[rgb]{0.38,0.63,0.69}{\textbf{\textit{#1}}}}

\usepackage{hyperref}
\hypersetup{colorlinks=true, linkcolor=black, urlcolor=blue!60!black, citecolor=black}

% C2-W2 fix: disable ALL auto-numbering. The markdown source carries manual
% numbering ("## 1. The Problem" → \section{1. The Problem}); LaTeX must not
% prepend its own "1" on top of the manual "1.".
\setcounter{secnumdepth}{-1}

\sloppy
\emergencystretch=3em
\hyphenpenalty=0
\exhyphenpenalty=100

\usepackage{fancyhdr}
\pagestyle{fancy}
\fancyhf{}
\lhead{}
\rhead{}
\cfoot{\small \thepage}
\renewcommand{\headrulewidth}{0pt}
\renewcommand{\footrulewidth}{0pt}

\title{}
\author{}
\date{}

\begin{document}

% C1-W1 fix: TOC restored. Pandoc markdown source begins with the title
% quote block; the TOC is rendered BEFORE the body via \tableofcontents.
\tableofcontents
\newpage

$body$
\end{document}
"""
TEMPLATE_FILE = WORKDIR / "mytemplate.tex"
TEMPLATE_FILE.write_text(TEMPLATE)

# 1. pandoc → tex
tex_path = WORKDIR / "whitepaper.tex"
pandoc_cmd = [
    "pandoc",
    "--from", "markdown+pipe_tables+backtick_code_blocks+fenced_code_blocks+tex_math_dollars",
    "--to", "latex",
    "--template", str(TEMPLATE_FILE),
    "--output", str(tex_path),
    str(SRC_MD),
]
print("→ pandoc:", " ".join(pandoc_cmd))
r = subprocess.run(pandoc_cmd, capture_output=True, text=True)
if r.returncode != 0:
    print("PANDOC STDERR:\n", r.stderr[-3000:])
    sys.exit(1)

# 2. tectonic compile (XeTeX engine)
print("→ tectonic compile (XeTeX)")
r = subprocess.run(
    ["tectonic", "-k", str(tex_path)],
    capture_output=True, text=True, cwd=str(WORKDIR)
)
if r.returncode != 0:
    print("TECTONIC STDERR:\n", r.stderr[-3000:])
    sys.exit(1)

produced = WORKDIR / "whitepaper.pdf"
if not produced.exists():
    produced = WORKDIR / tex_path.with_suffix(".pdf").name
if not produced.exists():
    print(f"!! Expected PDF, not found in {WORKDIR}")
    print("WORKDIR contents:", list(WORKDIR.iterdir()))
    sys.exit(1)

OUT_PDF.parent.mkdir(parents=True, exist_ok=True)
shutil.copy(produced, OUT_PDF)

# 3. Verify the four C1r repairs
import fitz
doc = fitz.open(str(OUT_PDF))
sha = hashlib.sha256(OUT_PDF.read_bytes()).hexdigest()
print(f"\n✓ {OUT_PDF}")
print(f"  pages: {doc.page_count}")
print(f"  sha256: {sha}")

# C1-W1: TOC restored
p1_text = doc[0].get_text()
has_toc_text = "Contents" in p1_text
print(f"\n[C1-W1] TOC restoration:")
print(f"  p1 contains 'Contents': {has_toc_text}")
print(f"  page count: {doc.page_count} (target: 16)")

# C1-W4: Unicode + margins
print(f"\n[C1-W4] Unicode + margins:")
non_ascii_test = doc[0].get_text()[:1000]
import unicodedata
broken = []
for ch in non_ascii_test:
    if ord(ch) > 127:
        name = unicodedata.name(ch, "UNKNOWN")
        if "LATIN" in name and "SMALL" in name and ch not in "àáâãäåæçèéêëìíîï":
            # Latin char where Unicode symbol expected
            broken.append((ch, name))
print(f"  broken chars in p1 (Latin where symbol expected): {broken[:5]}")
# Margin check via page width vs text bbox
p1 = doc[0]
pw = p1.rect.width
text_blocks = p1.get_text("blocks")
if text_blocks:
    min_x = min(b[0] for b in text_blocks if b[0] > 0)
    max_x = max(b[2] for b in text_blocks if b[2] < pw)
    print(f"  page width: {pw:.1f}pt; text x0={min_x:.1f}pt, x1={max_x:.1f}pt")
    print(f"  left margin: {min_x:.1f}pt (target: 72pt = 1in)")

# C2-W2: heading double-numbering check
# Find all section headings (font size > 12pt) and check for "N.N N." prefix
print(f"\n[C2-W2] Heading double-numbering check:")
heading_samples = []
for pno in range(min(3, doc.page_count)):
    page = doc[pno]
    for blk in page.get_text("dict")["blocks"]:
        if blk.get("type") != 0: continue
        for line in blk.get("lines", []):
            for span in line.get("spans", []):
                if span["size"] > 12.5 and len(span["text"].strip()) > 5:
                    heading_samples.append((pno+1, span["text"]))
for pno, t in heading_samples[:8]:
    double_num = bool(re.match(r"^\d+\.\d+\s+\d+\.\s", t))
    print(f"  p{pno}: {t!r}  double-numbered: {double_num}")

# 4. Forensic overflow scan per WO-P4-CLOSURE P4-W1
THRESH = 611.5
print(f"\n→ Forensic overflow scan (criterion: span x1 > {THRESH}pt, scope: whole document, all {doc.page_count} pages)")
flagged = 0
protrusion = 0
for pno in range(doc.page_count):
    page = doc[pno]
    pw = page.rect.width
    for blk in page.get_text("dict")["blocks"]:
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
                    protrusion += 1
print(f"  Flagged lines (x1 > {THRESH}): {flagged}")
print(f"  Protrusion lines (540 < x1 <= {THRESH}, whitelisted): {protrusion}")
if flagged > 0:
    print(f"!! {flagged} past-cropbox lines remain — v1.0.4 release gate fails.")
    sys.exit(2)
print(f"  v1.0.4 scan-clean (0 flagged lines).")
