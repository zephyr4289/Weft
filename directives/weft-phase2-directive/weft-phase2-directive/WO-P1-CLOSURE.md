# WO-P1-CLOSURE — Phase 1 + Phase 0.5 Adjudication & Sign-off

```
Directive:   weft-phase3-directive stream
Doc ID:      WO-P1-CLOSURE
Version:     v1.0 (supersedes nothing; closes WO-P1-BENCHMARKS v1.2)
From:        Staff adjudication
To:          Executor
Status:      RATIFIED — Phase 1 CLOSED, Phase 0.5 CLOSED, RFC-0001 ACCEPTED (countersigned)
Precedence:  On conflict, this doc > WO-P1-BENCHMARKS > roadmap PDF. Spec canon remains
             02–07 v1.2 until WO-P3 T3 applies the v1.3 patch.
```

---

## 0. Verdict

Phase 1 (benchmark harness, T2–T8) and Phase 0.5 (L-loom, T1) are **ACCEPTED**. The exit
checklist of WO-P1-BENCHMARKS is fully satisfied: 15/15 bench cells green, both structural
gates (B3 ratio < 2.0, B5 alloc == 0) pass in all three languages, loom exhaustive run clean,
RFC-0001 flipped to Accepted with the full evidence triad (loom + TSAN 5×8 + litmus 24/24).
Sign-off block is countersigned below. The word you asked for: **given — proceed per WO-P3.**

## 1. Task-by-task acceptance

| Task | Claimed | Verdict | Staff note |
|---|---|---|---|
| T1 loom | Exhaustive model clean, RFC-0001 → Accepted | **ACCEPT** | Two mid-flight model changes required adjudication — see §2, §3. Both ratified. |
| T2 catalog+driver | `bench/catalog.yaml` v1 validated | **ACCEPT** | Driver parsed `min_claims` per 05-CONTRACTS v1.1 rules. |
| T3+T4 C bench | 5/5 green, B3 ratio 1.175, B5 zero-alloc | **ACCEPT** | Two harness bug classes found and fixed by executor (`holds_ms`/`payload_max`/`writer_hz` NULL-deref after `strchr`; double-`r_claim` in sampled block). Good catches — exactly the 06-PITFALLS class. |
| T5 Rust bench | 5/5 green, ratio 1.211 | **ACCEPT** | |
| T6 TS bench | 5/5 green, ratio 0.627 final, B5 advisory-labeled | **ACCEPT** | Advisory labeling for B5 is the honest reading of a GC runtime — Law 4 compliant. See §5 for the ratio variance note. |
| T7 matrix+report | 15/15 green, results.json sha256 `16b5c663…` | **ACCEPT** | Final matrix run is the single authoritative source for baselines and all whitepaper tables. |
| T8 sign-off | Exit checklist all [x], baselines committed | **ACCEPT** | Countersigned §8. |

## 2. Adjudication A1 — loom assertion (c): telemetry counter → static range

**Finding.** The v1 loom model asserted `claimed_seq ≤ max_published` where `max_published`
is telemetry stored *after* the exchange. The reader can lawfully observe the published buffer
(via the Acquire side of the exchange) before the telemetry store lands → false RED.

**Ruling: executor CORRECT; model change RATIFIED.** This is the same defect class as Phase 0
F1: treating a telemetry counter as if it were the publish point. It is not. **The exchange is
the publish** — the Release store on the writer side paired with the Acquire load/swap on the
reader side is the sole happens-before edge that publishes buffer bytes. Telemetry lags by
design and carries no correctness meaning. Removing `max_published` from the model entirely is
the right call: the model should not contain telemetry at all, because the kernel does not use
it for correctness.

**Required strengthening (feeds WO-P3 T1, mandatory).** The replacement check
(`claimed ∈ {0..MAX_PUBLISHED_SEQ}`) is a range sanity check, but it is *weaker* than the
intended no-future property: it cannot catch a hypothetical model defect where the modeled
exchange hands the reader a seq that has not been published *in this schedule prefix*. Restore
the strong form without reintroducing the lag artifact:

- Keep a model-side watermark `published_wm`.
- Update `published_wm` **in the same atomic model step** as the writer's release into the
  slot (no interleaving point between the watermark update and the release — in loom this is
  just fusing both into one step body).
- Assert at claim time: `claimed_seq ≤ published_wm`.

The lag artifact came from the watermark living in a *separate, later store* — not from
watermarking itself. Fused at the exchange step boundary, the check is exact: no reader ever
observes a seq the writer has not yet released, across every interleaving. Re-run the
exhaustive exploration, append the v2 transcript to `litmus/evidence/loom/loom.txt` (keep v1
for the record), and reference the v2 run from RFC-0001's evidence appendix.

## 3. Adjudication A2 — loom assertion (d): eventual-final → post-join drain claim

**Ruling: executor CORRECT; change RATIFIED.** Loom is a safety model; it explores all
interleavings but cannot prove liveness. The reader-runs-first interleaving (reader drains only
the null frame, writer publishes everything after) is a legal schedule, and the property that
matters there is safety, not liveness. Eventual-final liveness remains owned by litmus L4
(v1.1 semantics: post-join drain ≤4 claims @1 ms, `drain_exact = (last == 2000)`).

One amendment, folded into WO-P3 T2: rename (d) so the model does not carry a misleading name,
and restate it as the safety fragment it actually checks — **join-quiescence**: at join, no
buffer remains in reader ownership; every buffer is in exactly one of {free, writer-held,
in-exchange}. This is fully expressible (and checked) in a safety model, and it is the
meaningful residual of "eventual-final" that loom *can* prove.

## 4. RFC-0001 — staff countersign

The re-issued Accepted status is **ratified by staff adjudication**. Evidence triad complete:

| Evidence | Scope | Result |
|---|---|---|
| L-loom exhaustive | all interleavings, 3 pub × 3 claim × 3 buffers | 4/4 assertions hold |
| TSAN | 5 runs × 8 tests = 40 executions | zero reports (post L7 fix) |
| Litmus | 24/24 cells, 3 languages | green |

**Executor action:** append this block to the RFC-0001 header:

```
Countersigned (staff adjudication): ACCEPTED — evidence triad verified
  loom: exhaustive, assertions a-d hold (v2 strengthened run per WO-P3 T1 attaches)
  tsan: 5x8 clean | litmus: 24/24 | adjudication: WO-P1-CLOSURE §4
```

The v2 loom run (WO-P3 T1) is a mandatory attachment to the whitepaper's verification
section; it does not reopen acceptance, it hardens it.

## 5. Discrepancy notes (no re-run required; rules instead)

1. **TS B3 ratio: 1.427 (standalone run) vs 0.627 (final matrix).** Both pass the < 2.0
   structural gate, so the verdict is stable — but a 2.3× swing between runs must never reach
   the whitepaper unexplained. Rules effective immediately (patched into 05-CONTRACTS by
   WO-P3 T3):
   - The ratio is **definitionally pinned**: `ratio := p50(64K) / p50(64B)`. Both p50s are
     always reported alongside it.
   - A sub-1.0 ratio is *legal* for a swap primitive (no size dependence; cache/alignment
     effects may favor either size) but REPORT.md must carry a one-line note whenever
     `ratio < 1.0` or when run-to-run ratio delta exceeds 0.3. No silent suppression.
   - Baselines and whitepaper tables must cite the **same** `results.json` sha256
     (`16b5c663…`) — single-source integrity. If WO-P3 work re-runs the matrix, the pipeline
     re-stamps everything together.
2. **C ratio 1.205 → 1.175, B1 2.89M → 2.92M ops/s.** Ordinary run-to-run variance. Final
   matrix numbers are authoritative; intermediate narration numbers are not canon.

## 6. KERNEL-PERF-1 — CLOSED (RESOLVED-BY-DESIGN)

Filed in Phase 0 adjudication (F2) as a timeboxed Phase 1 task: "make the TS litmus claim loop
faster." Not executed in Phase 1; disposition ruled here rather than re-issued:

- **The performance property moved to its correct home.** Litmus-loop claim rate was a harness
  exposure artifact, not a protocol measurement. B1 now measures TS publish throughput
  properly: **1.61M ops/s** — the protocol is fast in TS. Performance belongs to the bench
  harness; litmus belongs to soundness.
- **The exposure-floor mechanism works.** `min_claims` (C 600 / Rust 600 / TS 200) with
  `claims_per_s` telemetry met its purpose: TS litmus ran 211 ≥ 200.
- **Fragility hardening, not optimization.** The 5.5% margin (211 vs 200) is thin, so WO-P3
  T3 adds an exposure-retry rule: the driver retries an L1 cell **once** when
  `claims < min_claims`, labeled `EXPOSURE-RETRY` in the report. An exposure shortfall after
  retry is reported as such — it is never auto-converted to green.
- **Reopen rule.** If any future litmus change leaves TS below floor after retry, harness
  optimization becomes blocking again and KERNEL-PERF-1 reopens.

## 7. Roadmap ledger (published deviations & forward obligations)

Per the roadmap's own rule, deviations are published, not hidden:

1. **Phase 2 ↔ Phase 3 swap: APPROVED.** Whitepaper (P3) executes next, tools (P2) immediately
   after, before Phase 4. Rationale: (a) founding spec §5 still documents the formally-unsound
   two-variable protocol — every day it stays un-superseded is a correctness liability;
   (b) Phase 4 is ~12k LOC across four ecosystems and must be written against a frozen
   normative reference, not scattered RFCs and reports; (c) measured numbers are fresh in the
   results pipeline now; (d) weft-probe/record's primary consumers are Phase 4 porting and
   Phase 6+ debugging — nothing in the whitepaper consumes them. **Phase 2 is not cancelled:**
   the Phase 5 tarball tree requires `tools/weft-probe/` + `tools/weft-record/`.
2. **Phase 5 dependency note (filed now, resolved at WO-P5 scoping).** Roadmap Phase 1 defined
   W1–W5 workloads × A–D impls × fairness pin × static site; our Phase 1 (correctly, per
   bench/README) delivered the B1–B5 protocol-proof suite instead. Phase 5's gates reference
   `make bench` / `make site` over workloads/results/site. Before Phase 5 signs off, staff will
   scope the W-suite/site gap (build it, or formally re-scope the Phase 5 gate). Recorded so it
   cannot be discovered late.
3. **Phase 3 reproducibility criterion highlighted.** The roadmap's success criterion for the
   whitepaper: *a stranger can reproduce a published number on their own hardware.* WO-P3 T8
   therefore requires a reproducibility appendix (exact commands, toolchain versions, env
   capture, artifact hashes), and T4 makes every table machine-generated from `results.json`.

## 8. Sign-off

```
Phase 1   (benchmark harness, B1-B5 x 3 languages):  CLOSED  15/15 green, gates pass
Phase 0.5 (L-loom exhaustive model):                 CLOSED  assertions hold, strengthened v2 queued
RFC-0001  (Triad Exchange Protocol):                 ACCEPTED (countersigned, section 4)
KERNEL-PERF-1:                                       CLOSED  RESOLVED-BY-DESIGN (section 6, reopen rule)
Next work order:                                     WO-P3-WHITEPAPER (Phase 3)

Reviewed-by: staff adjudication
```
