# WO-P3-WHITEPAPER — Phase 3: The Citable Artifact

```
Directive:   weft-phase3-directive stream
Doc ID:      WO-P3-WHITEPAPER
Version:     v1.0
From:        Staff adjudication
To:          Executor
Status:      ACTIVE — supersedes roadmap ordering per WO-P1-CLOSURE §7.1 (Phase 2 <-> 3 swap)
Depends:     Phase 0 CLOSED, Phase 0.5 CLOSED, Phase 1 CLOSED, RFC-0001 Accepted
Budget:      ~2.0 engineer-days
Kernel:      FROZEN for the duration of this WO (see section 5)
```

---

## 0. Authority and context

The whitepaper is the roadmap's citable artifact and it **supersedes the founding spec PDF**.
Two of its inputs just closed: the evidence triad (loom + TSAN + litmus 24/24) and the
measured bench matrix (15/15, both structural gates green in three languages). You will write
the document that Phase 4's ~12k LOC of platform ports are implemented against — every
ambiguity you leave in it becomes a porting bug later, so the bar is *decision-complete prose*,
not marketing.

Roadmap section 3a fixes the section structure (11 sections); section 3b fixes the errata
duties. This WO adopts that structure with three staff amendments, marked `[AMEND]`:

1. Section 6 gains a formal-verification subsection (loom + evidence triad) — the loom result
   is now a first-class proof artifact and must appear in the citable doc.
2. The protocol section carries **Axiom T** (telemetry), ratified in WO-P1-CLOSURE §2 — two
   independent defect findings (Phase 0 F1, loom v1) traced to the same root cause; that rule
   is now normative.
3. A reproducibility appendix is mandatory — the roadmap's own Phase 3 criterion is
   *"a stranger can reproduce a published number on their own hardware."*

**Deliverables (roadmap 3a/3b, amended):** `docs/WHITEPAPER.md`,
`Weft-Whitepaper-v1.0.pdf` (typeset; fallback rule in T7), `docs/ERRATA.md` +
supersession banners, `tools/whitepaper_tables.py`, updated 05-CONTRACTS v1.3,
loom v2 transcript.

---

## 1. Tasks

### T1 — Loom strengthening: fused watermark (mandatory, ~1h)

Per WO-P1-CLOSURE §2. In the loom model:

- Add model-side `published_wm: usize`.
- Fuse its update into the **same atomic model step** as the writer's release into the slot —
  there must exist no interleaving point between "release lands" and "watermark advances".
  In loom terms: one step body, both effects.
- Assertion (c) becomes: `claimed_seq <= published_wm`, checked at claim time. Keep the
  existing `claimed <= MAX_PUBLISHED_SEQ` range check as a sub-assertion.
- Re-run the exhaustive exploration. Expected: clean, same schedule count as v1 (the model
  gained strictness, not behavior — if the schedule count changed, you changed the protocol
  step structure; halt and report under section 6).
- Append the v2 transcript to `litmus/evidence/loom/loom.txt` (v1 preserved above it, clearly
  delimited). Reference the v2 run from RFC-0001's evidence appendix.

### T2 — Loom assertion (d) rename + join-quiescence (~15m)

Per WO-P1-CLOSURE §3. Rename (d) and restate it as the safety fragment:

> (d) join-quiescence: at join, no buffer remains in reader ownership; every buffer is in
> exactly one of {free, writer-held, in-exchange}.

Liveness (eventual-final) stays out of the model and remains owned by litmus L4. Update the
RFC-0001 assertion list wording to match.

### T3 — Contracts v1.3 surgical patch (~1h)

Apply to `05-CONTRACTS.md` (banner bump v1.2 -> v1.3, changelog line "AXIOM-T + ratio pin +
exposure-retry; per WO-P1-CLOSURE"). Add this section verbatim:

```markdown
## AXIOM T — Telemetry is not a correctness reference

Telemetry counters (claims_per_s, publish counters, max_published, and any future counter)
are advisory. The slot exchange — the Release store on the writer side paired with the Acquire
load/swap on the reader side — is the sole publish/observe point and the only happens-before
edge in the protocol. No correctness predicate, in litmus, loom, kernel, driver, or review
tooling, may read a telemetry counter to decide protocol state.

Rationale: Phase 0 F1 and the loom v1 false-RED are the same defect class — a lagging
telemetry store mistaken for the publish point. Telemetry lags the exchange by construction.
(Post-join reads of telemetry for reporting are fine; in-flight reads are not.)
```

Additionally in 05-CONTRACTS (bench section):

```markdown
### B3 ratio definition (pinned)
ratio := p50(64K) / p50(64B). Both p50s are always reported alongside the ratio.
A ratio < 1.0 is legal for a swap primitive (cache/alignment effects may favor either size);
REPORT.md must carry a one-line note whenever ratio < 1.0 or run-to-run ratio delta > 0.3.
Gate remains: ratio < 2.0 (structural, zero-copy proof). [WO-P1-CLOSURE §5]

### Exposure retry (L1)
Driver retries an L1 cell exactly once when claims < min_claims, labeled EXPOSURE-RETRY in
the report. An exposure shortfall after retry is reported as an exposure finding — it is
never auto-converted to green. [WO-P1-CLOSURE §6]
```

Mirror the two bench rules as one-line cross-references in `bench/catalog.yaml` comments.

### T4 — Numbers pipeline: `tools/whitepaper_tables.py` (~1–2h)

The whitepaper must not contain a single hand-typed measured number. Write a Python generator
that:

- Reads `bench/results.json` + `bench/baselines/x86_64-sandbox.json` (+ litmus REPORT summary
  JSON if present).
- Emits `docs/WHITEPAPER-TABLES.md` fragments: structural-gates table, headline throughput
  table, claim p50/p99 by payload size, clock overhead, tail latencies, litmus 24-cell
  summary.
- Stamps every table with: source file sha256 (must equal `16b5c663…` unless the matrix was
  re-run — then re-stamp everywhere), label `MEASURED x86_64-sandbox`, and generation UTC
  timestamp.
- Exits non-zero on missing/stale inputs (refuses to generate from nothing).
- The whitepaper includes these fragments verbatim at build time (include-marker comments).

### T5 — `docs/WHITEPAPER.md` draft (~1–1.5d, target 7,500–10,000 words)

Adopt roadmap 3a's 11 sections with the three `[AMEND]` insertions. Per-section briefs —
each section's body must be grounded in the listed evidence, and the content-depth rules of
section 2 apply:

| § | Title | Brief (what it must establish) | Evidence source | Words |
|---|---|---|---|---|
| 1 | The problem | Recomposition cascade, GC storms, FFI marshalling, main-thread blocking. Name the four failure modes concretely with one worked example each. | Founding spec §1–4 (problem statement), ARCHITECTURE.md | ~600 |
| 2 | The thesis | Sharpened claim + **corrected boundary** (what Weft does NOT claim). State the Four Laws as the contract frame. | ARCHITECTURE.md, PHILOSOPHY.md | ~500 |
| 3 | The Triad Protocol | Corrected single-atomic-exchange design; worked ownership trace from RFC-0001 §4.4; invariant table I1–I6; **Axiom T** `[AMEND]` with both finding citations (Phase 0 F1, loom v1) as empirical justification. | RFC-0001 (Accepted), 02-KERNEL | ~1,200 |
| 4 | The frame envelope | Tier 0 frozen 16-byte header; why frozen; triad-1/triad-2 coexistence; unknown-field tolerance. | 03-ENVELOPE v1.2, L8 results | ~500 |
| 5 | Writer revocation (I6) | ACK-before-poison handshake; the one-relaxed-load-per-publish cost; the L7 TSAN saga as the cautionary tale (caller contract: after ACK, the buffer is not yours) — this section teaches the contract by showing the race that violated it. | TSAN logs, WO-P0A addendum S11, 06-PITFALLS S5 | ~600 |
| 6 | Measured results | 6a litmus: 24/24, adversarial condition table, stale-return legality (§0.6). 6b bench: structural gates first (B3 ratio, B5 alloc), then headline numbers — **all tables from T4 pipeline**. 6c loom `[AMEND]`: schedule count, assertions a–d, v2 strengthened run. 6d evidence-triad table. Ratio definition + the TS variance note appear here verbatim. | results.json, REPORT.md, loom.txt v2, TSAN logs | ~1,500 |
| 7 | Cross-language consistency | Same catalog, three runtimes, one verdict rule ("each test passes in all three, or the protocol is wrong"). TS memory-model note: SC Atomics ≥ C relaxed — where the web port is *stronger*, and why that is not cheating. Divergence rule as published. | litmus REPORT v2, 05-CONTRACTS | ~600 |
| 8 | Platform honesty | Safari 60 Hz cap (WebKit bug 173434 — cite), SAB COOP/COEP requirement, iOS Canvas-vs-MTKView split, RN as weakest differentiator, TS B5 advisory status (GC). Every platform claim carries a citation. | Founding spec §9, docs suite | ~700 |
| 9 | Non-goals | First-class section: single-writer/single-reader, bounded slot count, no cross-process today, no ordering guarantees beyond latest-frame, Pro tier out of scope. | ARCHITECTURE.md, charter clause 4 | ~400 |
| 10 | Open questions | Q1–Q5 from ARCHITECTURE.md, listed not hidden; for each: current best answer + what evidence would move it. | ARCHITECTURE.md | ~400 |
| 11 | References + prior art | Compose graphicsLayer, Reanimated SharedValue, LeakCanary, Apache Arrow, graphics triple buffering; all RFCs, directive docs, roadmap; artifact hash index. | docs suite, prior-art search | ~400 |
| A | Appendix: reproducibility `[AMEND]` | Exact commands (litmus, bench, loom), toolchain versions (GCC 14.2.0, Rust 1.98.1, Node 24.19.0, Python 3.12.14), env capture (CPU model, x86_64-sandbox label), artifact sha256 index, and the "stranger reproduction" procedure. | env capture in results.json | ~500 |
| B | Appendix: catalog + bench cells | catalog.yaml schema summary, B1–B5 definitions, min_claims exposure-floor policy, Axiom T cross-ref. | bench/catalog.yaml, 05-CONTRACTS | ~400 |

**Labeling regime (roadmap 3a success criterion, mechanical):**
- Every number: `[MEASURED x86_64-sandbox <sha8>]` via T4 fragments, or `[PREDICTION: <reasoning>]`.
- Every platform claim: `[CITE <ref>]`.
- No forward-looking performance claims for Phase 4 ports — they are source-only and unbenchmarked; say so explicitly in §8.
- Founding spec §9.5's predicted numbers appear only inside the errata comparison, never as current claims.

### T6 — Errata + supersession (~2h)

- `docs/ERRATA.md`: founding spec §5 (two-variable protocol — withdrawn, formally unsound per
  RFC-0001 §3), §9.5 (predicted numbers — replaced by measured), and the Phase-1
  implementation report's protocol (withdrawn-protocol code, corrected in Phase 0). Each item:
  what was wrong, the correction, where the truth now lives.
- Supersession banners: repo README + `docs/` index get a STATUS block pointing at the
  whitepaper; founding spec references in the docs suite get "superseded by WHITEPAPER §n"
  pointers. (The founding PDF itself is an external artifact — the ERRATA doc is the
  supersession instrument. State that plainly in ERRATA.)

### T7 — Typeset PDF (~2–3h)

- Produce `Weft-Whitepaper-v1.0.pdf` from WHITEPAPER.md. Preferred: pandoc -> LaTeX, or
  tectonic if present. ACM-style plainness is fine; no design work.
- Fallback rule: if no LaTeX toolchain exists in-sandbox, deliver md + pandoc HTML and file
  `PDF-DEFERRED` under the 07-ACCEPTANCE failure protocol with the missing-toolchain evidence.
  Do not hand-render a fake PDF.
- Generate the cross-language consistency appendix table from REPORT v2 + bench REPORT
  (script-generated, same pipeline discipline as T4).

### T8 — Honesty & reproducibility pass (~2h)

- Traceability check (script or checklist): every measured number in WHITEPAPER.md exists in
  results.json / REPORT v2 / loom.txt; every `[CITE]` resolves; zero unlabelled numbers.
- Verify no kernel/spec contradiction introduced by the doc (whitepaper describes what IS —
  if you find a suspected protocol defect while writing, section 5 applies, not a quiet edit).
- Publish the roadmap deviation: ROADMAP.md (or README STATUS block) gains the Phase 2 <-> 3
  swap note with one-line rationale, per WO-P1-CLOSURE §7.1.
- Confirm baselines, REPORT.md, and WHITEPAPER-TABLES.md all cite the same results.json sha256.

---

## 2. Writing rules (non-negotiable)

1. **Decision-complete prose.** A Phase 4 porter must be able to implement Kotlin from §3–§5
   alone. Any sentence a porter could ask "yes but what exactly does that mean?" is unfinished.
2. **Adversarial-reviewer stance.** Every claim must survive "cite the artifact" — if the
   artifact does not exist, the claim does not ship.
3. **Content depth.** No section under its word budget without a stated reason; no
   single-sentence paragraphs; tables get interpretive prose ("what this means"), not bare rows.
4. **No marketing.** "Zero-copy confirmed" is a structural-gate statement with a ratio and a
   method behind it, not a slogan. Adjectives need numbers; numbers need labels.
5. **Honesty about the sandbox.** Every number carries `x86_64-sandbox`. The doc says once,
   plainly: these are protocol-proof numbers, not device numbers; Phase 6+ replaces them.

## 3. Exit checklist

- [ ] T1 loom v2: fused watermark, exhaustive re-run clean, transcript appended, RFC appendix ref
- [ ] T2 assertion (d) renamed join-quiescence; RFC wording updated
- [ ] T3 05-CONTRACTS v1.3 applied verbatim (AXIOM T + ratio pin + exposure retry); catalog comments mirrored
- [ ] T4 `tools/whitepaper_tables.py` runs green; all tables stamped + sha256-bound
- [ ] T5 WHITEPAPER.md complete per section table; labeling regime mechanically satisfied
- [ ] T6 ERRATA.md + supersession banners in place
- [ ] T7 PDF delivered (or PDF-DEFERRED filed with evidence)
- [ ] T8 traceability pass clean; deviation published; sha256 single-source confirmed
- [ ] REPORT/README pointers updated (whitepaper referenced from docs index)

## 4. Budget

| Task | Estimate |
|---|---|
| T1 + T2 (loom) | 1.25h |
| T3 contracts v1.3 | 1h |
| T4 pipeline | 1.5h |
| T5 whitepaper | 9h |
| T6 errata | 2h |
| T7 typeset | 2.5h |
| T8 honesty pass | 2h |
| **Total** | **~19h ≈ 2.0 engineer-days** |

## 5. Scope guards

- **Kernel FROZEN.** The whitepaper documents what IS. If writing it surfaces a suspected
  protocol defect: file an RFC draft, halt the affected section, report under the failure
  protocol. No kernel edits inside a documentation WO — that is how specs and code drift apart.
- **Out of scope:** Phase 2 tools (weft-probe/record — next WO after this), Phase 4 ports,
  W1–W5 workloads / static site (Phase 5 scoping), any performance tuning, any catalog
  re-measurement beyond what T1/T3 force.
- Failure protocol: 07-ACCEPTANCE §6 as always — report the RED, not a workaround.

## 6. Sign-off block (executor completes)

```
WO-P3-WHITEPAPER execution report
- T1 loom v2:            [ ] schedules=<N> clean, transcript appended
- T2 join-quiescence:    [ ]
- T3 contracts v1.3:     [ ] applied verbatim
- T4 pipeline:           [ ] tables generated from sha256=<...>
- T5 WHITEPAPER.md:      [ ] <word count> words, sections 1-11 + A/B
- T6 errata + banners:   [ ]
- T7 PDF:                [ ] delivered | [ ] PDF-DEFERRED filed, reason=<...>
- T8 honesty pass:       [ ] findings=<N>

Deviations/findings (section 6 protocol):
<none | list>

Sign-off:
- Executor: ______________  date: ______
- Staff review: ______________
```
