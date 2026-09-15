# WO-P3-CLOSURE — Phase 3 (Whitepaper) Adjudication & Sign-off

```
Directive:   weft-phase2-directive stream
Doc ID:      WO-P3-CLOSURE
Version:     v1.0 (closes WO-P3-WHITEPAPER v1.0)
From:        Staff adjudication
To:          Executor
Status:      ACCEPTED — Phase 3 CLOSED conditional on amendment batch A1–A3 (~1h, no re-run)
Artifact:    upload/Weft-Whitepaper-v1.0.pdf (16pp) — reviewed in full by staff
Next:        WO-P2-TOOLS (Phase 2: weft-probe + weft-record)
```

---

## 0. Verdict

This was the first round where the deliverable itself (not a report about it) sat in front of
staff, so the review was a full 16-page read, not a claims audit. Result: **the whitepaper is
the artifact this project promised.** Structure matches the amended outline exactly (11
sections + 2 appendices); AXIOM T is §3.5 verbatim; the pinned B3 ratio and the TS sub-1.0
note appear verbatim (§6b + Appendix B); every table carries
`[MEASURED x86_64-sandbox sha256:16b5c663]`; §8's platform claims are all cited; §8.6
explicitly refuses forward-looking port claims; the TSAN cautionary tale (§5.2) teaches the
caller contract the right way; the v1.0 row-3 typo and stale-return legality are documented
where a confused reader will look for them; Appendix A nails the roadmap's stranger-reproduction
criterion — including the honest line that a differing sha256 on other hardware is *expected*,
with structural gates as the invariant.

Amendment batch below is wording-only. No measurement re-runs, no pipeline changes, tables
and hashes unchanged.

## 1. Findings and rulings

### W1 — T5 word budget (~5,000 delivered vs 7,500–10,000 targeted)

**Ruling: ACCEPTED, no expansion.** Staff read all 16 pages against the per-section briefs:
worked examples in every §1 subsection, complete protocol pseudocode, full invariant and
envelope bit tables, decode rules, negotiation formula with the corrected row, HB-chain
argument in §5.2, structural-gate interpretation prose, divergence rule, 9 non-goals, 5 open
questions each with a moving-evidence plan, 14+ citations. Nothing a Phase 4 porter needs is
missing; the sections are dense, not thin. The budget was guidance; decision-completeness is
the bar — padding prose to hit a word count would have made this document worse.

**Process miss, standing rule issued:** the execution report's deviations field was empty
despite a ~35% quantitative shortfall. Effective immediately (applies to WO-P2 onward):
*any* quantitative target miss — words, LOC, cells, runs, margins — must be declared in the
deviations field, even when acceptance is likely. Silence on a known miss is the failure
mode the reporting protocol exists to prevent.

**W1b (minor):** §1's illustrative numbers (11–14 FPS, 480 KB/s, 2.3 s TTI) predate the
labeling regime and carry no marker. They are founding-spec problem-statement figures, not
whitepaper measurements — but the doc should say so. Folded into A2.

### W2 — §6c loom fidelity boundary (the substantive finding)

What happened in T1, reconstructed from the report: the executor first packed
`latest | published_wm` into one AtomicU64 so a single swap would publish both — correct
instinct — then hit a real wall: the *reader's* swap stores `r_work`, which carries no
watermark bits, so every claim would zero the watermark. The adopted fix wraps the model's
exchange in `loom::sync::Mutex<(u32,u32)>`: the writer's critical section fuses release +
watermark advance (no interleaving point — the fusion requirement is met), the reader's
critical section preserves the watermark.

**Ruling on the design: RATIFIED as verification-sound.** Loom explores all interleavings of
the Mutex's underlying atomics, so exhaustiveness is preserved; the strong no-future check
(`claimed_seq ≤ published_wm`) is now exact; the packed-atomic dead end was diagnosed
correctly and the journey is good engineering. Kernel remains untouched (FROZEN respected).

**Ruling on the whitepaper wording: AMENDMENT REQUIRED.** §6c honestly names the
"Mutex-serialized exchange" but never draws the fidelity boundary. As written, a careful
reader can over-conclude that loom verified the *lock-free* exchange's memory ordering. It
did not, and the project's credibility depends on nobody being able to say the citable
artifact overclaims. A1 (below) inserts the boundary paragraph — verbatim text provided —
and mirrors one line into RFC-0001's loom appendix. Then re-typeset as **v1.0.1**.

### W3 — Citation URL clipping (verify-and-fix)

§8.2 (MDN URL), §8.3 (Apple URLs), and §11 (developer.android.com) appear to clip long
`texttt` URLs at the line edge in the rendered PDF (e.g. `.../Global_Obje`, `.../jetpack/co`).
This may be a parse artifact — but in a *citable* artifact a clipped citation is a broken
citation, and Law 4 does not allow "probably fine." A3: inspect the rendered pages; if any
URL is visually truncated, enable breakable URLs (`\url`/`xurl` with `url` breaking, or
shorten to domain + path stub + access date) and confirm every `[CITE]` resolves visually.

## 2. Amendment batch A1–A3 (mandatory, ~1h total)

**A1 — insert at the end of §6c (verbatim):**

> **Model-fidelity note.** The loom model serializes the exchange with a loom Mutex as a
> *verification device*: it exists so that the no-future watermark can be fused into the
> writer's publish step with no interleaving point between them — which a packed atomic
> cannot express (the reader's swap would clobber watermark bits it does not carry). The
> exhaustive proof therefore covers the protocol's **state machine** — ownership,
> no-torn-observation, no-future, join-quiescence — under mutual exclusion. The shipped
> kernel is lock-free: one AcqRel exchange per side. Memory-ordering correctness of the
> lock-free exchange is not claimed from loom; it is carried by (i) the ownership argument
> of §3.3 — two RMWs on a single variable are totally ordered, and each exchange transfers
> exactly one buffer — and (ii) TSAN 5×8 on the real implementation (§6d). The Mutex is a
> property of the model, not of the kernel; any suggestion that the kernel should grow a
> lock is a misreading of this section.

Plus one line in RFC-0001's loom evidence appendix: "v2 model fuses the no-future watermark
via a loom Mutex (verification device only — kernel exchange remains lock-free; ordering
proof burden: §3.3 ownership argument + TSAN). See WHITEPAPER §6c model-fidelity note."

**A2 — one scope sentence at the top of §1 (verbatim):**

> Numbers in this section are illustrative of the failure modes, carried from the founding
> spec's problem statement and public documentation; the whitepaper's own measurements
> begin in §6 and carry the MEASURED label.

**A3 — URL clipping check/fix per W3.**

**Then:** bump the doc to `Whitepaper v1.0.1` (footer/changelog line: "v1.0.1 — staff review
amendments A1–A3, per WO-P3-CLOSURE"), re-typeset, confirm the hash index still cites
`16b5c663` for results.json (unchanged), and post the two PDFs plus WHITEPAPER.md diff in the
completion report.

## 3. Sign-off

```
Phase 3 (Whitepaper):        CLOSED conditional on A1-A3 (~1h, wording only)
WHITEPAPER v1.0:             ACCEPTED -> v1.0.1 after amendments
Numbers pipeline (T4):       ACCEPTED — single-source sha256 discipline verified end-to-end
Loom v2 (T1/T2):             RATIFIED as verification-sound; fidelity note mandatory (A1)
Standing rule issued:        quantitative target misses must be declared in deviations field
Next work order:             WO-P2-TOOLS (weft-probe + weft-record, Phase 2 restored to sequence)

Reviewed-by: staff adjudication
```
