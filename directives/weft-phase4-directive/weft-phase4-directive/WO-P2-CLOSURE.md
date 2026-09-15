# WO-P2-CLOSURE — Phase 2 (Tools) + Whitepaper v1.0.1 Adjudication

```
Directive:   weft-phase4-directive stream
Doc ID:      WO-P2-CLOSURE
Version:     v1.0 (closes WO-P2-TOOLS v1.0; completes WO-P3-CLOSURE verification)
From:        Staff adjudication
To:          Executor
Status:      CONDITIONAL — NOT CLOSED. Correction batch B1–B4 required (~3h total).
             Phase 3 stays open until B1; Phase 2 stays open until B1–B4.
Artifacts:   upload/Weft-Whitepaper-v1.0.1.pdf (16pp) — reviewed in full
             upload/Weft-Phase2-Tools-Report.pdf (3pp) — reviewed in full
Next:        WO-P4-PORTS (T0 = this batch; Phase 4 proper follows)
```

---

## 0. Verdict

Staff performed a full artifact-level review of both delivered PDFs. The tooling itself is
real and largely sound: FORMATS.md preceded recorder code; the debug-view accessor respects
every T1 rule; 4/4 interop combos with logged record counts; quiesced and revocation-safe
probes pass in both languages; the LOC deviation was declared under the standing rule. The
deviations discipline is visibly working — three findings were declared that a year ago
would have been buried.

It is therefore more disappointing that the batch fails on **honesty-of-evidence** grounds,
not tooling grounds. One sign-off line was checked against evidence that was not the
contracted evidence (soak), one amendment was declared "verified" against the wrong
artifact (A3), one environment claim is falsified by the executor's own C evidence (EBADF),
and one contracted test case was marked N/A without a deviation declaration. Per
07-ACCEPTANCE §6 and Law 4, these are correction batches, not a phase redo: B1–B4 below are
~3h of work total. But nothing closes until they land.

## 1. Verified and RATIFIED (no action)

| Item | Ruling |
|---|---|
| T0 A1 model-fidelity note | ✅ inserted at end of §6c **verbatim** (staff diff-checked the rendered page against WO-P3-CLOSURE §2 A1 word-for-word) |
| T0 A2 §1 scope sentence | ✅ **verbatim** at top of §1 |
| v1.0.1 version + changelog line | ✅ correct wording, cites WO-P3-CLOSURE |
| Hash index | ✅ still cites `16b5c663` for results.json; all `[MEASURED x86_64-sandbox]` labels intact |
| T1 debug-view accessor | ✅ all five rules honored per report (Acquire header reads, `mid_publish_sample`, no deref of unowned buffers, telemetry advisory, no unsafe in Rust public API); semver minor noted; no other kernel change |
| T2 spec-before-code | ✅ FORMATS.md committed before recorder code, per report |
| T3 quiesced dump | ✅ both languages, seq=100 round-trip exact |
| T3 revocation-safe probe | ✅ both languages, no UAF, `revoked=true` reported |
| T4/T5 interop | ✅ 4/4 combos green with per-combo record counts (C-stream: 7,103,427 records; Rust-stream: 3,120,649) |
| LOC −59% deviation | ✅ **ACCEPTED.** Tools are functionally complete and compact; budgets were guidance. Declaring it was exactly right — this is the standing rule succeeding |

## 2. Findings and rulings

### P2-W1 — A3 "verified" against the wrong artifact (evidence attached)

The report claims: *"A3: URL clipping verified in the typeset PDF."* Staff forensics on the
rendered v1.0.1 prove the three flagged citations are **still physically truncated at the
page edge**:

| Location | Text layer ends at | Next line | Span geometry (PyMuPDF) |
|---|---|---|---|
| §8.2 MDN | `.../Reference/Global_Obje` | prose (no URL continuation) | x1 = 612.1pt vs cropbox 612.0pt |
| §8.3 Apple | `.../documentation/metalkit/mt` | prose (no URL continuation) | x1 = 616.1pt vs cropbox 612.0pt |
| §11 Compose | `developer.android.com/jetpack/co` | next bullet (no continuation) | x1 = 612.1pt vs cropbox 612.0pt |

The URLs are truncated mid-token in the PDF text layer itself, and every flagged line
overflows the physical page width (overfull hbox clipped at the cropbox). WO-P3-CLOSURE
§1 W3 anticipated exactly this failure: *"This may be a parse artifact — but in a citable
artifact a clipped citation is a broken citation, and Law 4 does not allow 'probably
fine.'"* The instruction was to inspect **the rendered pages** and confirm every `[CITE]`
**resolves visually**. The verification was evidently performed on the markdown source or a
text parse — the one artifact where the defect does not appear.

**Ruling: A3 = FAILED.** Not a conduct violation — a verification-method error, and the
fix is 45 minutes. But it must be said plainly: the whitepaper is the citable artifact, and
right now three of its citations do not resolve. B1 below.

### P2-W2 — Soak evidence silently substituted (2×30s @ 120 Hz → 2×2s unpaced @ ~3.5 MHz)

Contracted (WO-P2 §1 T6): *"Soak: writer at 120 Hz / 64 B payload / 30 s => ~3,600 frames
per language; both complete; replay byte-identical; stale-return counts reported."*
Delivered: **2 seconds per language, writer unpaced at ~3.5M publishes/sec** (7,103,427
frames in 2s). Three defects in one finding:

1. **Duration substituted** (30s → 2s) — never declared. The deviations field lists LOC,
   frame_count patching, and pacing — not duration. Silence on a known quantitative miss is
   precisely the failure mode the standing rule exists to prevent (WO-P3-CLOSURE §1 W1).
2. **Rate substituted** (120 Hz → unpaced ~3.5M Hz) — declared as finding 3, correctly
   diagnosed as a harness timing issue, but then **not converted into a fix-and-rerun or a
   RED**. The failure protocol (07-ACCEPTANCE §6) offers two lawful responses to a broken
   gate: fix and re-run, or report RED. Running a different, easier test and ticking the
   box is a third option the protocol does not offer.
3. **The sign-off writes `[x] 2x2s clean` directly against the WO's `2x30s` line** — the
   checkmark claims the contracted evidence exists. It does not. The capture table even
   labels the runs "2s @ 120Hz" while the same page reports ~3.5M publishes/sec — the
   artifact mislabels its own evidence.

What the delivered run *does* prove (millions of records, per-record CRC, byte-identical
replay) is welcome stress evidence — but it tests throughput, not **sustain**. The soak's
purpose is a realistic 30-second display-rate session: no drift, no descriptor growth, no
slow leak over time. B2 below. Staff note for B2: a sleep-per-tick pacer drifts; pace
against an absolute schedule (`t_n = t0 + n/120`, sleep-until-absolute) and log the
effective rate; it must land within 119–121 Hz. Report RSS before/after each soak as well
(one line, cheap, makes the no-leak claim visible).

### P2-W3 — Rust frame_count EBADF: environment claim falsified by the executor's own C tool

Report finding 2: *"The OpenOptions seek+write+read pattern failed on this sandbox's
filesystem (EBADF)... a documented degradation, not a bug."*

**Ruling: the environment theory is falsified by the report's own evidence.** The C
recorder patches `frame_count` in place successfully — the C column in the byte-identical
row carries *no* scan-to-EOF annotation, meaning C's in-place header patch on close worked
on the same filesystem. If the filesystem refused in-place rewrite, both tools would fail
identically. The overwhelmingly likely cause is a Rust-side bug: the reopen handle missing
`.write(true)` (an O_RDONLY handle returns EBADF on seek-to-write precisely this way), or
the patch attempted through a stale/read-only duplicate.

The scan-to-EOF path is format-legal — but adopting it as the **normal close path for every
Rust recording** conflates "crashed session" with "healthy Rust session." The header's
`frame_count` exists so consumers can validate without scanning; a fleet of forever-zero
headers erodes that. B3: fix the reopen (or attach a ≤5-line isolated repro proving an
environment-level EBADF against a plain `OpenOptions::new().read(true).write(true).open()`
seek-write — if that repro actually fails, staff will retract this finding with an apology).

### P2-W4 — Rust probe live-dump mode missing, marked N/A, undeclared

WO-P2 §1 T3 defines three test cases for the probes, C **and** Rust: quiesced, live, and
revocation-safe. The report delivers live mode for C only and marks Rust *"N/A (Rust probe:
quiesced + revocation only)."* N/A is a status an executor may assign to a test that cannot
apply — it is not a status an executor may assign to a contracted case it chose not to
build. This is a scope cut against the WO, and it was **not declared** in the deviations
field (the LOC table's aside "no live-mode JSON needed for Rust" is a rationale, not a
declaration). B4: implement the Rust live dump — it is the quiesced loop with a sleep
between samples, well inside remaining budget — and run it, or file a formal scope-change
request against WO-P2 §1 T3 and let staff rule.

### P2-W5 — Minor notes (no batch items)

- The report's T0 section does not evidence the RFC-0001 loom-appendix mirror line required
  by WO-P3-CLOSURE A1. Fold the confirmation into B1's checklist (quote the line in the
  batch report).
- "Update repo README / docs index" (WO-P2 §1 T7) is checkmarked but not evidenced in the
  report. No action beyond quoting the pointer in the B-batch report.
- Tools report T7 content otherwise complete; typeset pipeline fine.

## 3. Correction batch B1–B4 (mandatory, ~3h total — wording and harness only, no kernel change)

**B1 — Whitepaper v1.0.2 (A3 proper), ~45m.**
Enable breakable URLs in the typeset chain (`\usepackage{xurl}` / `url` breaking, or
`seqsplit` for the `texttt` citations), re-typeset, then **verify on the rendered PDF**:
extract the three flagged lines (§8.2, §8.3, §11) and confirm every URL resolves end-to-end
in the text layer *and* renders without crossing the page edge. State the verification
method used in the batch report (staff's own method, available as the standard: `pdftotext
-layout` line extraction + span-bbox check that no line exceeds page width − margin;
equivalent visual inspection at high zoom is acceptable). Bump to v1.0.2 with changelog
"A3 re-verified on rendered artifact, per WO-P2-CLOSURE P2-W1". A1/A2 text and all
tables/hashes unchanged. Quote the RFC-0001 loom-appendix mirror line in the batch report
(P2-W5).

**B2 — Contracted soak, ~1h.**
Fix the pacer to absolute-schedule pacing (t_n = t0 + n/120; effective rate 119–121 Hz,
logged). Run the contracted evidence: **2 × 30 s @ 120 Hz / 64 B payload**, one capture per
language (~3,600 frames each), replay byte-identical both, stale-return counts reported,
RSS before/after reported. Update the tools report table with the real evidence and relabel
the earlier unpaced runs as what they were (burn-in stress, informational). The 2s runs may
remain in the report as supplementary — labeled honestly, they are good evidence.

**B3 — Rust frame_count patch, ~45m.**
Fix the reopen path so close() patches frame_count + header CRC (open with read+write,
seek 16, write u32 LE, recompute CRC over bytes 0..20, write at 20). Re-run one Rust
capture; show `frame_count == record count` on close and replay validating via the count
(not scan-to-EOF). If you believe the environment is genuinely at fault, attach the ≤5-line
isolated repro instead and staff will adjudicate the repro.

**B4 — Rust probe live-dump mode, ~45m.**
Implement live mode for the Rust probe (same contract as C: repeated advisory samples at
display rate, `mid_publish_sample` flags, no crash/block/alloc). Run it; report the sample
count and any flags observed. Or file the scope-change request described in P2-W4.

**Batch report:** one consolidation — quote each of B1–B4 with its evidence; fill the
deviations field with anything quantitative that moved; no re-typeset of the tools report
beyond the soak table and a B-item evidence appendix is required.

## 4. Phase ledger after this batch lands

```
Phase 3 (Whitepaper):  OPEN -> CLOSED when B1 lands (v1.0.2 render-verified)
Phase 2 (Tools):       OPEN -> CLOSED when B1–B4 land
Next work order:       WO-P4-PORTS (T0 = this batch; ports follow)
Standing rule reaffirmed: every quantitative deviation is declared, even the
ones that reflect well on you. Especially those.
```

## 5. Sign-off

```
WO-P2 (Tools):               CONDITIONAL — correction batch B1–B4
WO-P3 (Whitepaper):          OPEN pending B1 (A1/A2 verified verbatim; A3 failed,
                             evidence attached)
Loom fidelity note (A1):     ACCEPTED verbatim
Interop + format:            RATIFIED — .weftrec v1 is language-neutral (4/4)
Debug-view accessor:         RATIFIED — semver minor, T1 rules honored
Standing rule (deviations):  REAFFIRMED — two undisclosed misses this round; the field
                             exists precisely for the misses that look like wins

Reviewed-by: staff adjudication
```
