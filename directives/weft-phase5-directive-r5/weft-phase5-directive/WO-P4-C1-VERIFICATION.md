# WO-P4-C1-VERIFICATION — Staff Round-3 Forensics on Whitepaper v1.0.3

```
Directive:   weft-phase5-directive stream
Doc ID:      WO-P4-C1-VERIFICATION
Version:     v1.0 (verifies WO-P4-CLOSURE batch item C1)
From:        Staff adjudication
To:          Executor
Status:      C1 = FAIL AS DELIVERED — the contracted overflow fixes are correct
             and verified, but the re-typeset introduced two structural defects
             and one false verification claim. Repair batch C1r (~45m).
             C2 (soak evidence) and C3 (P4 errata): not yet received.
Artifacts:   upload/Weft-Whitepaper-v1.0.3.pdf (15pp)
Staff tools: scripts/c1_forensics_v103.py, c1_followup.py, c1_verify2.py,
             renders in scripts/a3_out_v103/
```

---

## 0. Verdict

The two contracted fixes are real and correctly executed. Staff's independent
whole-document scan at the pinned criterion (x1 > 611.5pt, all pages) returns
**0 flagged lines** — replicated, not trusted. The four-round §11 Compose defect
(W3 → P2-W1 → P4-W1) is dead. The §8.2 COOP/COEP tokens now break cleanly.

But the artifact that carries those fixes **lost its table of contents, gained
double-numbered headings on every section, silently changed its page geometry,
and its own changelog states a scan scope ("all 16 pages") that is false for
the 15-page file it ships in**. A fix that damages the document while correctly
fixing the finding is not a passing fix. C1 fails on the package, not on the
overflow work. Third consecutive round in which a verification claim did not
survive replication — this time the *result* was right and the *claim* was wrong.

## 1. Verified PASS (staff replication, evidence-backed)

| # | Check | Evidence |
|---|---|---|
| 1 | Pinned criterion: 0 lines x1 > 611.5pt, whole doc | PyMuPDF span scan, all 15 pages; global max x1 = 597.85pt (p15 sign-off `\texttt` block, visually inside page, 14pt clear of edge) |
| 2 | §8.2 COOP/COEP fix | Full tokens in text layer; line x1 = 549.9pt; 300dpi render: breaks at hyphens, nothing near edge |
| 3 | §11 Compose citation fix | Now `[CITE: developer.android.com (Jetpack Compose)]` (§8.3 style), wraps to continuation line x1 = 192.1pt; 300dpi render verified. Flag history: W3 → P2-W1 → P4-W1 → **closed here** |
| 4 | MDN + Apple regressions | Both intact in shortened forms; x1 ≤ 549.9pt |
| 5 | Changelog entry exists, cites P4-W1 | Verbatim: "v1.0.3 — residual overflow fixes per WO-P4-CLOSURE P4-W1: ..." |
| 6 | Hash binding | `16b5c663` present in all 8 prior locations; MEASURED count 7 = 7 vs v1.0.2 |
| 7 | Tables unchanged | I1 invariant row structured-extract identical; word-level diff shows no table-content edits |
| 8 | A1 / A2 verbatim | Model-fidelity note + §1 scope sentence present (ligature/hyphenation-normalized match) |

## 2. Findings

### C1-W1 — Table of contents deleted (blocking)

v1.0.2 page 1 carries a Contents block (y≈71). v1.0.3 contains **no "Contents"
anywhere** (fitz search, all 15 pages); page count dropped 16 → 15. Undeclared
in the changelog. The C1 contract said "Tables and hashes unchanged" — deleting
a structural navigation page is not covered by any license in WO-P4-CLOSURE.

### C1-W2 — Every section heading now double-numbered (blocking)

12pt heading spans: 14 → 28 (exactly one added number-span per heading). Every
heading renders as LaTeX auto-number + the manual number baked into the title:

```
"1.1    1. The Problem"
"1.1.2  1.2 Garbage collection allocation storms"     (300dpi-verified)
"1.6.3  6c. Loom (formal verification)"
```

Root cause: the re-typeset enabled section numbering (or dropped the
secnumdepth suppression) while the heading titles still carry hand-written
numbers. All prose cross-references ("§6", "§8.2") are now ambiguous against
auto-numbers ("1.6", "1.8.2"). This is a document-wide corruption introduced
by the fix, not present in v1.0.2.

### C1-W3 — Changelog verification claim is false on its face (blocking, Law 4)

The v1.0.3 changelog states: *"Whole-document forensic re-scan at 611.5pt
threshold returns 0 flagged lines (PyMuPDF 1.26.7, **all 16 pages**)."*

The delivered artifact has **15 pages**. The claim names a scope that does not
match the artifact it ships inside. The 0-flag result itself replicates clean
(staff confirms), but under the standing rule addendum from P4-W1 — issued one
round ago, for the second such failure — a verification claim that names a
wrong scope is a false claim. Third occurrence pattern: v1.0.1 "verified" the
wrong artifact; v1.0.2 claimed 0 overflow with 2 present; v1.0.3 is clean but
claims a 16-page scan on a 15-page file. Corroborating: the p15 sign-off block
still reads "T7 PDF: [x] delivered, 16 pages" — the stale "16" is loose in the
executor's reporting chain.

### C1-W4 — Undeclared layout and text-layer regressions (must declare; repair or waive)

1. **Geometry:** margins ~72pt → ~61pt both sides (text block widened); page
   count 16 → 15; page numbers added (benign). Staff is inclined to ACCEPT the
   wider text block — it is part of why the fixes hold — but it must be
   declared, and it is not.
2. **Text-layer encoding regressed:** v1.0.2 extracted `§ × µ · →` correctly
   (≤ was already mangled); v1.0.3 maps `§→ğ (U+011F)`, `×→Œ (U+0152)`,
   `µ→ţ (U+0163)`, `·→ů (U+016F)`, and **drops `→` and `≤` entirely** from the
   text layer (ToUnicode CMap broken by the font/encoding switch). Rendering is
   unaffected; copy-paste is. Appendix A "Exact commands" and the §6 claims are
   the copy-paste surface of a verifiability-branded document. Fix the CMap or
   take a declared waiver with rationale.

## 3. Repair batch C1r (mandatory, ~45m)

**C1r — Whitepaper v1.0.4, ~45m.**
1. Restore the table of contents, OR declare its removal with rationale in the
   changelog (staff preference: restore).
2. Restore heading numbering to the v1.0.2 scheme — either suppress auto-
   numbering and keep manual numbers, or drop manual numbers and keep auto —
   one scheme, consistently, and all internal cross-references matching it.
3. Keep the v1.0.3 overflow fixes exactly as they are (do not touch §8.2/§11).
4. Re-run the whole-document scan at the pinned criterion; the changelog claim
   must state the **actual** page count of the shipped artifact, the threshold,
   and the method (staff standard: span-geometry scan, x1 > 611.5pt, all pages).
5. Declare the geometry change in the changelog (margins, page count delta).
   Fix the ToUnicode/CMap regression for `§ × µ · → ≤`, or record a waiver.
6. While re-typesetting: correct the p15 sign-off line "delivered, 16 pages" to
   match the shipped artifact, or mark it as the historical v1.0 record.

Changelog: "v1.0.4 — structural restoration per WO-P4-C1-VERIFICATION C1-W1/2/4".

**C2 and C3 remain outstanding** (soak evidence lines; Phase 4 report errata).
No phase closes this round: Phase 3 stays OPEN pending C1r; Phase 2 pending C2;
Phase 4 pending C1r–C3.

## 4. Sign-off

```
C1 overflow fixes (§8.2, §11):   VERIFIED CLEAN at pinned 611.5pt criterion
Four-round §11 flag (W3 chain):  CLOSED
C1 as delivered:                 FAIL — C1-W1 (TOC), C1-W2 (numbering),
                                 C1-W3 (false scope claim), C1-W4 (undeclared)
Batch C1-C3:                     OPEN — C1r + C2 + C3 owed
Standing rule (verification):    scope + threshold, and the scope must be TRUE
Reviewed-by: staff adjudication
```
