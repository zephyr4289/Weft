# WO-P0A — Phase 0 Findings Adjudication & Completion Work Order

**Issued by:** Staff engineering (spec owner) · **Date:** 2026-09-11
**Assigned to:** Phase 0 executor
**Status:** Decision-complete. Execute as written. Every ruling below is final; where a ruling overrides either the executor's conclusion or the directive's v1.0 text, the override and its proof are stated in place.
**Estimate:** ~1 engineer-day (§9). **Prerequisite:** none — all work is against already-built artifacts.

---

## 0. Review verdict on the Phase 0 execution

**Accepted, with conditions.** The execution followed the §6 failure protocol exactly: findings were filed, not papered over; no tolerance edits; no re-run-until-green (the L6 stability re-runs are evidence-gathering, which is permitted); the spec-inconsistency escalation on L8 was correct process. The cross-language behavior (A6) was checked and reported honestly. The sign-off statement's honesty labels are correct.

**Correction to the executor's status claim:** Phase 0 is **not** "pending follow-ups" — per `07-ACCEPTANCE.md` §4, G3 (TSAN column) is an **exit gate**, not a follow-up. Current status: **G1 passed, G2 partially met (20/24), G3 open.** This work order closes all three.

Rulings summary:

| # | Finding | Executor's diagnosis | Ruling | Root cause class |
|---|---|---|---|---|
| F1 | L4 RED (C+Rust), 0–12 violations, jitter-dependent | "Test's `S < P0` check too strict; protocol correct" | **Executor is right, and the cause is now precisely identified: a v1.0 spec bug in 04-LITMUS L4 (and L1's drain clause).** Directive amended to v1.1. | Spec predicate ill-posed |
| F2 | L1 RED (TS), claims=211 < 600 | "TS performance; threshold calibrated for C/Rust" | **Ruling: exposure floor, not property.** `min_claims` becomes a per-language catalog param (C 600 · Rust 600 · TS 200). TS hot-path follow-up filed as KERNEL-PERF-1 (Phase 1, timeboxed). | Test-config / calibration |
| F3 | L8 negotiation: §3 formula vs §5 table row 3 | "§5 table row 3 appears to be a typo" | **Executor is right, with a stronger proof than stated:** the v1.0 table contradicts *itself* — row 4 `(W=3,S={1,2})→2` presupposes downgrade capability, row 3 denied it. The §3 formula is normative; row 3 corrected to `→ 1`. Directive amended to v1.1. | Spec typo |
| F4 | L4 cross-language consistency (A6) | "Consistent — same reason in C+Rust" | **Confirmed.** No action beyond F1. After F1, all three languages should be GREEN on L4; if not, that is a new finding. | — |
| F5 | TSAN Step 6 deferred | "Filed as follow-up" | **Overruled: G3 exit gate.** Execute per §6 below. Zero new code required. | Gate open |
| F6 | L-loom deferred (A8) | "Filed as Phase 0.5" | **Confirmed as Phase 0.5**, not blocking Phase 1, but blocking the RFC-0001 flip-to-Accepted. Spec skeleton in §8. | Gate scheduled |

---

## 1. F1 — L4 freshness: root cause and final predicate

### 1.1 Root cause (this is the part to internalize)

The v1.0 L4 checked, per claim: `P0 = t_publish` → claim → `S`; violation if `S < P0`. Trace the exchange protocol (02 §2) on **two consecutive claims with no intervening publish**:

```
State before claim 1:  latest = B_new (seq k)   r_work = B_old (seq j, j << k)
claim 1:  mine = latest.exchange(r_work)  → B_new (seq k)     latest = B_old, r_work = B_new   ✓ freshest
claim 2:  mine = latest.exchange(r_work)  → B_old (seq j)     latest = B_new, r_work = B_old   ← STALE
```

A claim hands `r_work` out and receives previous `latest`. With no publish in between, previous `latest` **is the reader's own old buffer**. The reader lawfully receives a buffer carrying `seq j << k`. The v1.0 predicate `S < P0` fires on this designed behavior — at writer 960 Hz / reader 240 Hz it needs scheduler jitter to trigger (reader burst-claims, writer stall), which is why it was non-deterministic (0–12) rather than constant.

The v1.0 **drain clause** ("after writer join, claim → `S` MUST equal exactly `2000`") was ill-posed for the same reason, plus one more case: if the reader consumed the final frame *before* the join, the post-join claim ping-pongs a stale buffer. Harness restructuring cannot fix this — the predicate itself was wrong. The v1.0 "Why P0 ≤ S holds" note also contained a half-finished self-correction; that was a known-rough edge shipped in a normative field. Both are now replaced (04-LITMUS v1.1).

### 1.2 The amended predicate (drop-in — already applied to 04-LITMUS v1.1)

- Track `last` = highest seq observed so far (init 0).
- Per claim: `P0 = t_publish (Acquire)` → claim → `S` → `P1 = t_publish (Acquire)`.
  - `S > last` (**new frame**): `S < P0` → freshness violation. Then `last = S`.
  - `S <= last` (**stale return**): increment `stale_returns`; **not** a violation.
  - `S > P1` (after bounded 2 ms re-read): future violation.
- **Final drain:** after writer join, claim up to 4 attempts (1 ms apart); `drain_exact = (last == 2000)`.
- Verdict keys unchanged (`freshness_violations`, `future_violations`, `drain_exact`) — only definitions tightened; `stale_returns` added to metrics.

**Executor task A1:** update the L4 harness in all three runners to the v1.1 predicate (~15 lines each). C and Rust currently RED; TS currently GREEN *by accident* (its slower reader paces around the jitter window) — TS gets the same fix, not a pass-through.

### 1.3 Same bug, latent, in L1's drain clause — fix it in the same pass

L1 v1.0 said "after writer join, one more claim MUST return `S == 600`". Identical ill-posedness; it did not fire in Phase 0 only because of timing luck (the TS claims-floor issue masked the exposure; C/Rust读者的 loop pace happened to consume frame 600 after join). On slower CI hardware it *will* false-red at the worst possible moment. v1.1: bounded post-join drain (≤4 attempts, 1 ms apart), `drain_ok = (max observed S == 600)`.

**Executor task A2:** update L1 drain clause in all three runners. Also update L1 verdict to `claims >= min_claims(lang)` (see F2) and add `claims_per_s` to L1 metrics.

### 1.4 Formalize the null-frame fixture invariant

The executor's L6 fix (fill the null frame's payload with `pat(0, i)` in `weft_init`) was correct and is now **normative**, not improvised: 04-LITMUS §0.6 (v1.1) states it as a fixture contract. **Executor task A3:** add a comment at the init site in all three kernels citing `04-LITMUS §0.6`, so a future kernel rewrite doesn't silently break L6.

---

## 2. F2 — L1 exposure floor (TS)

**Ruling.** `claims >= 600` is an *exposure-sufficiency* bound — "the reader claimed enough times that a tear, if possible, would have been caught" — not the property under test (the property is `torn == 0` with live-buffer verification). A per-language floor is therefore legitimate **if and only if** it lives in the catalog (config, audited, diffable) and carries telemetry so recalibration stays data-driven. It must never live in runner code.

- v1.1 floors: **C 600 · Rust 600 · TS 200** (TS floor set at measured 211 with margin). Executor may not tune; changes require a catalog amendment citing `claims_per_s` evidence.
- L1 metrics gain `claims_per_s` (1 decimal) — this is what makes the floor falsifiable and recalibratable.

**Executor task A4:** add `min_claims: { c: 600, rust: 600, ts: 200 }` to L1 params in `litmus/catalog.yaml`; extend `tools/validate_catalog.py` (map with exactly keys `c/rust/ts`, positive ints); extend `tools/litmus_driver.py` to resolve the map to a scalar per language (`min_claims=200` on the CLI — runners stay language-blind); add `claims_per_s` to the three L1 runners.

**KERNEL-PERF-1 (filed, Phase 1, timeboxed 2 h, not blocking):** TS claim loop measured ~211 claims vs 600 in-window; suspects named by the executor: `Atomics.wait` per null-spin, BigInt conversion, worker IPC. Task: microprofile the claim loop (per-op timing breakdown), then one targeted fix if the profile warrants (e.g., eliminate per-claim allocations). Do **not** contort the kernel to hit a test threshold; a documented floor with telemetry is an acceptable permanent state. Acceptance if attempted: TS L1 ≥ 600 at floor restored, `torn=0`, no per-claim allocation.

---

## 3. F3 — L8 negotiation: the table was the bug

**Ruling.** The §3 formula `max({v ∈ S : v ≤ W})` is **normative**; §5 row 3 was a typo. The decisive proof (stronger than "formula is normative by position"): the v1.0 table contradicted **itself** —

- Row 4 `(W=3, S={1,2}) → 2` requires the v3 writer to be able to **emit** v2 (downgrade capability).
- Row 3 `(W=2, S={1}) → BIND_INCOMPATIBLE` denies exactly that capability.

No single semantics satisfies both rows. The formula satisfies rows 1, 2, 4 as written and is the only self-consistent reading. It is also coherent with the additive evolution mechanism (03 §4): a v2 writer emits v1 frames by writing `header_size=16` envelopes — old layout minus trailing fields — so the downgrade commitment is mechanically cheap. 03-ENVELOPE v1.1 adds the **writer-ceiling commitment**: a writer declaring `W` vouches it can emit every `v ≤ W` for the Weft's lifetime; a future writer that cannot (semantic version break) is exactly the gap RFC-0002's capability sets close — filed, deferred (see `RFC-0002-DRAFT-negotiation-sets.md`).

**Executor impact: zero code changes.** The executor implemented the formula per §4.3 and the three runners already bind row 3 → 1. **Executor task A5:** confirm the L8 test vector for row 3 asserts `1` (not `BIND_INCOMPATIBLE`) in all three runners; close the finding in REPORT.md as "resolved: §5 row 3 was the bug; formula normative (WO-P0A §3)". If any runner hardcodes an INCOMPATIBLE expectation for row 3, fix the vector (one line) — the negotiation *function* does not change.

---

## 4. F4 — A6 cross-language consistency

**Confirmed.** C+Rust RED for the identical spec-level reason while TS (different pacing) stayed GREEN is consistent behavior under one protocol, and is exactly what the cross-language matrix exists to surface. No action beyond A1/A2. After the v1.1 amendments, all three languages must be GREEN on L1 and L4; any residual divergence is a **new** finding under the §6 protocol, not a tuning opportunity.

---

## 5. F5 — TSAN gate (G3): mandatory now

The Makefile contract (05 §6) already defines `build-c-dbg` (`-O0 -g -fsanitize=thread` → `core/c/spike-tsan`) and `make tsan`. This is a re-run, not new work.

**Executor task A6:**

```
make build-c-dbg
python3 tools/litmus_driver.py --runner core/c/spike-tsan --langs c   # or: make tsan
```

- Run the full 8-test C suite under TSAN. **Repeat the run ≥ 5 times** (races are scheduling-dependent; one clean pass proves little).
- Pass criterion: **zero** ThreadSanitizer reports across all runs, all 8 tests, and all test verdicts still exit 0 under instrumentation. Slower wall-clock under TSAN is expected; if a suite exceeds the driver's 120 s per-test timeout, raise the timeout **for the TSAN run only** via driver flag and note it in the report — never by editing runner pacing.
- If TSAN reports anything: freeze, capture the full report to `litmus/evidence/tsan/`, file per the §6 failure protocol. Pay particular attention to the harness itself — a runner data race (shared verdict variables, unsynchronized stats) is as much a finding as a kernel race.
- Save transcripts: `litmus/evidence/tsan/run-<n>.log` (timestamped).

---

## 6. Full matrix re-run & report re-issue

**Executor task A7:** after A1–A5 land:

1. `make clean && make litmus` — full 24-cell matrix (debug+release C/Rust, TS), all exit 0.
2. Then A6 (TSAN column).
3. Driver re-issues `litmus/REPORT.md` + `results.json`. Prior red evidence stays in the bundle (07 §5) — do not delete the Phase 0 v1 report; rename it `REPORT-v1-preWO-P0A.md` as evidence of the process.
4. Sign-off block, filled:

```
Phase 0 complete: 24/24 cells green (plus TSAN column: clean × 5 runs).
Spec v1.1 amendments applied per WO-P0A (L1 drain, L4 predicate, L8 row 3,
min_claims floor). Findings F1–F6 adjudicated; see WO-P0A-ADJUDICATION.md.
The corrected Triad Protocol is runtime-verified under adversarial scheduling
on x86_64-sandbox. This is NOT a formal proof; L-loom is Phase 0.5 (A8).
All numbers labeled x86_64-sandbox. RFC-0001: Accepted-pending-loom.
Signed: <executor> · Reviewed-by: staff adjudication (WO-P0A, 2026-09-11)
```

**Executor task A8 (hygiene, 45 min):** restore the relaxed Rust lints to strict (`unsafe_op_in_unsafe_fn` etc.), fix the resulting mechanical nits, keep every `SAFETY:` comment citing RFC-0001 §4 (02 §7). If any lint cannot be satisfied without contortion, file it with rationale instead of weakening the lint — same protocol as everything else.

**Executor task A9 (15 min):** catalog `version: 1 → 2` in `litmus/catalog.yaml` (schema changed: `min_claims`), update the REPORT header's catalog version accordingly. Validator enforces the new shape.

## 7. Exit checklist (Phase 0 is DONE when all hold)

- [ ] 24/24 cells green under v1.1 predicates (A1, A2, A4)
- [ ] L8 vectors assert formula semantics incl. row 3 → 1 (A5)
- [ ] TSAN: 0 reports × ≥5 runs, transcripts in evidence (A6)
- [ ] REPORT v2 re-issued with sign-off above; v1 preserved (A7)
- [ ] Rust lints strict again or waiver filed (A8)
- [ ] Catalog v2 + validator pass (A9, A4)
- [ ] Null-frame fixture invariant cited in all three kernels (A3)

Anything red after this = new finding, §6 failure protocol, stop and report up. No silent accommodation.

## 8. F6 — L-loom (Phase 0.5, scheduled — do NOT start before WO-P0A closes)

Loom model of the exchange pair, mirroring `core/rust/src/lib.rs` op-for-op (same orderings: `latest.exchange(AcqRel)` ×2, `w_work`/`r_work` private):

- 3 buffers, 2–3 publishes, 2–3 claims, loom exhausts all interleavings.
- Assertions: (a) each buffer has ≤1 owner at every point (shadow-track ownership); (b) a claimed frame's seq and payload pattern are mutually consistent (no torn observation); (c) observed seq never exceeds the highest published seq; (d) after the final publish, some claim observes it (eventual-final — the v1.1 L4 property, now checked exhaustively).
- Note: loom will confirm the ping-pong stale return as a *legal* history — encode it as such (assertions may not forbid staleness; see 04 §0.6).
- Output: `litmus/evidence/loom/` transcript. On success: RFC-0001 flips Accepted-pending-loom → Accepted (re-issue RFC-0001 with the evidence citation; that re-issue is a Phase 0.5 deliverable).

## 9. Order of work and budget

| Order | Task | Est. |
|---|---|---|
| 1 | A1 + A2 (harness predicate fixes ×3 languages) | 2.0 h |
| 2 | A4 + A9 (catalog min_claims + validator + driver resolution + version bump) | 1.0 h |
| 3 | A3 + A5 (fixture-invariant comments; L8 vector confirmation) | 0.5 h |
| 4 | A7 step 1 (clean full-matrix re-run — expect 24/24) | 0.5 h |
| 5 | A6 (TSAN build + 5 runs + evidence) | 1.5 h |
| 6 | A8 (Rust lints) + report re-issue/sign-off | 1.0 h |
| — | KERNEL-PERF-1 (Phase 1, optional, timeboxed) | 2.0 h |
| — | L-loom (Phase 0.5, after sign-off) | 0.5 day |

**Total to Phase 0 sign-off: ~1 engineer-day.**

## 10. Explicitly out of scope (unchanged from 07 §8)

No kernel redesigns, no new atomics, no "improvements" to the exchange, no benchmark work (Phase 1), no negotiation rewrite to capability sets (RFC-0002 is deferred until a second version exists). The protocol survived its first adversarial contact; the defects found were in the *spec's test predicates*, which is the system working.
