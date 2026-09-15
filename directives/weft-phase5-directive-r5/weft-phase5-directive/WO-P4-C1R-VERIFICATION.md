# WO-P4-C1R-VERIFICATION — Staff Round-4 Forensics on Whitepaper v1.0.4

```
Directive:   weft-phase5-directive stream
Doc ID:      WO-P4-C1R-VERIFICATION
Version:     v1.0 (verifies C1r repair batch; closes WO-P4-C1-VERIFICATION)
From:        Staff adjudication
To:          Executor
Status:      C1r ACCEPTED — Phase 3 (Whitepaper) CLOSED.
             Ordering ruling issued: C2 next, then C3.
             Phase 2 closes at C2; Phase 4 closes at C2–C3.
Artifacts:   upload/Weft-Whitepaper-v1.0.4.pdf (16pp)
             sha256 6f3a3211... (prefix matches executor's claim)
Staff tools: scripts/c1r_forensics_v104.py, renders in scripts/a3_out_v104/
```

---

## 0. Verdict

Replicated, not trusted — and this time everything held. All four C1r repairs
verify on the artifact at the staff standard: the pinned-criterion scan is clean
across all 16 pages, the TOC and single numbering scheme are back, the margins
are back at 1in, the text layer extracts real Unicode, and — the part that
mattered most after three rounds — **the changelog's verification claim states a
scope that is true of the file it ships in.** The v1.0.3 entry is marked
"Withdrawn" with the reason stated, which is exactly how a false claim should be
retired in a ledger that trades on honesty.

Phase 3 is CLOSED. The whitepaper that ships in the Phase 5 tarball is v1.0.4.

## 1. Verified (staff replication, all pass)

| # | Check | Evidence |
|---|---|---|
| 1 | sha256 | `6f3a3211` prefix matches executor's claim |
| 2 | Pinned scan: 0 lines x1 > 611.5pt, **all 16 pages** | PyMuPDF span scan; global max x1 = 573.9pt (p16 sign-off block); 151 benign-protrusion lines whitelisted |
| 3 | C1-W1 TOC restored | "Contents" on p1 with dotted entries + page numbers; PDF outline = 53 entries; page count 16 |
| 4 | C1-W2 numbering restored | All auto-number tokens deleted (word-diff); TOC/outline/body show single manual scheme ("1. The Problem", "8.4 React Native…"); 0 double-numbered headings |
| 5 | C1-W3 scope claim TRUE | Changelog: "this document is 16 pages, scan-clean at the pinned 611.5pt threshold across all 16 pages" — matches artifact; v1.0.3 entry marked "(Withdrawn in v1.0.4 — re-typeset broke the package…)" |
| 6 | C1-W4 geometry + CMap | Margins 72.0pt left / body edge 540.0–540.1 all pages (1in restored); text layer: § ×26, · ×3, × ×12, µ ×2, → ×13, ≤ ×7 — real codepoints; §8.2/§11 citations survive the re-typeset (COOP/COEP x1 ≤ 540.0; Compose CITE + continuation "(Jetpack Compose)]" intact; MDN + Apple complete incl. "(SwiftUI Canvas, MetalKit MTKView)]") |
| 7 | Hash contract | `16b5c663` in all bound locations (artifact index table intact); MEASURED count 7 = baseline |
| 8 | A1 / A2 verbatim | Both present; A1 squeeze-diff divergence was a page-footer artifact in the v1.0.2 baseline (paragraph crossed pages), first 200 chars identical incl. apostrophe codepoints |
| 9 | Tables | I1 invariant row identical ("Each buffer has ≤1 owner…" with real ≤ now); word-diff shows changes localized to TOC/changelog/numbering/glyphs |

## 2. Errata E-1 (recorded, non-blocking)

The v1.0.4 changelog cites "**(C2-W2)** heading auto-numbering disabled…" —
staff's finding ID is **C1-W2** (C2 is the soak-evidence item). The referenced
directive (WO-P4-C1-VERIFICATION) is correct and the described fix is
unambiguous, so this does not block closure and does not warrant a v1.0.5 on
its own. Correction recorded here; if any future whitepaper re-typeset occurs
(e.g., during Phase 5 packaging), fix the two characters then. The C3 errata
document must carry this line.

## 3. Phase ledger after C1r

```
Phase 0 / 0.5 / 1 (kernel, litmus, bench):  CLOSED (unchanged)
Phase 3 (Whitepaper):                       CLOSED at C1r — v1.0.4 is canonical
Phase 2 (Tools):                            OPEN — closes at C2 (soak evidence)
Phase 4 (Ports):                            OPEN — closes at C2–C3 (C1 satisfied)
Next:                                       WO-P5 execution starts only after
                                            C2–C3 land (T0 gate unchanged)
```

**WO-P5 amendment (staff-side):** the tarball-tree spec in WO-P5-RELEASE names
"whitepaper v1.0.3"; read it as **whitepaper v1.0.4** (or "latest closed
version at packaging time").

## 4. Ordering ruling — C2 first, then C3

Executor asked which to deliver next. Ruling: **C2.**

- C2 is the only open item with an unknown in it. The evidence is collected
  (`/litmus/evidence/soak-b2/evidence.json`), but the World A / World B
  discriminator (stale counts ≈ frame_count − 3,600 vs ≈ 0 at ~10^8 claims)
  decides whether closure is a 30-minute packaging job or a ~2-hour pacer
  fix-and-rerun. Surface that risk now, not after C3.
- C3 is bounded typesetting with no unknowns; it can absorb the E-1 errata line
  and the full B1 claim-history correction (v1.0.2 false claim → v1.0.3 four
  defects → v1.0.4 repair) in one pass.

**C2 delivery spec (restated):** for EACH 30s soak run, print the three lines —
effective writer rate (Hz), fresh vs stale claim counts, RSS before/after —
plus capture/replay sha256, and state explicitly which world the runs were in.
If stale ≈ 0 at ~10^8 frames, that is World B: fix the pacer (absolute schedule,
t_n = t0 + n/120), re-run, publish the same lines from the new runs. Either
answer closes B2; silence does not.

## 5. Sign-off

```
C1r batch (v1.0.4):        ACCEPTED — all four repairs verified on artifact
Five-round citation flag:  CLOSED (W3 -> P2-W1 -> P4-W1 -> C1-FAIL -> C1r)
Verification-claim record: v1.0.4 claim TRUE on scope, threshold, and count
Phase 3 (Whitepaper):      CLOSED — canonical whitepaper = v1.0.4 (6f3a3211...)
Errata E-1:                RECORDED (C2-W2 -> C1-W2 typo; fold into C3)
Next delivery:             C2 first (soak evidence, World ruling included),
                           then C3 (P4 errata incl. E-1 + B1 claim history)
Reviewed-by: staff adjudication
```
