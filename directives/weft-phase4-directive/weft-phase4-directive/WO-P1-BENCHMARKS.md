# WO-P1 — Kernel Benchmark Harness & Result Pipeline

**Issued by:** Staff engineering (spec owner) · **Date:** 2026-09-11
**Assigned to:** Phase 0 executor (same team)
**Status:** Decision-complete. Execute as written. Unresolvable ambiguity = file a finding; do not improvise.
**Prerequisite:** Phase 0 signed (done, WO-P0A §11). **Task T1 (L-loom) runs FIRST** — no benchmark code before it lands.
**Estimate:** ~4 engineer-days total (0.5 d loom + 3.5 d harness).

---

## 0. What this phase is (and is not)

**One sentence:** make every performance claim Weft will ever make *falsifiable, reproducible, and tail-honest* — the harness core and evidence pipeline that the eventual public suite inherits.

**Relationship to `bench/README.md` (constitution):** that document describes the *public* program — workloads W1–W5, device matrix, implementations A–D, battery/thermal metrics, `weft.dev/benchmarks`. It requires Steward/Heddle and real hardware that do not exist yet. **WO-P1 builds the kernel-level substrate underneath it**: the catalog/runner/driver/compare/baseline machinery and the methodology rules. When W1–W5 land later, they become catalog entries in this machinery — nothing about this phase is throwaway.

Two constitutional anchors implemented here, early:

- bench/README §5: **"P99 is the headline, not P50"** and **"C and D must measure 0 B/frame or the run fails"** → methodology §4.3 and gate B5.
- bench/README §8: **the release gate** (regression on published metrics fails release) → the baseline + compare pipeline (§6) is its seed.

**Directly informed by the old Phase-1 report's findings** (the Android-lineage report, now superseded): its I2 used throughput to claim wait-freedom (corrected: §4.3 tails mandatory, publish-side p99/p99.9 under adversarial load), and its copy-on-read contradicted the zero-copy claim while its benchmarks couldn't see the difference (corrected: gate B3, the scaling fingerprint — the test that *would have caught it*).

**Out of scope** (do not build): W1–W5 workloads · device matrix · implementations A/B/D · browser/WASM benches · Android/JNI re-audit (the old Phase-1 lineage re-enters the roadmap in a later phase with its own work order) · kernel performance tuning (findings are filed, not fixed — §4.8) · external publication of any number · CI config files.

---

## 1. T1 — Phase 0.5 gate: L-loom (before anything else)

Mirror the Rust kernel's protocol core (`core/rust/src/lib.rs`) op-for-op in a loom model — same orderings (`latest.exchange(AcqRel)` for both writer and reader; `w_work`/`r_work` private, non-atomic or Relaxed-with-comment per 06 §3):

- 3 buffers, **3 publishes × 3 claims**, loom exhausts every interleaving.
- Assertions (all must hold in every history):
  - (a) **Ownership**: each buffer has ≤ 1 owner at every point (shadow-track via the exchange values).
  - (b) **No torn observation**: a claimed frame's seq, payload pattern, and canary are mutually consistent.
  - (c) **No future**: an observed seq never exceeds the highest seq published so far in that history.
  - (d) **Eventual-final**: after the last publish, some reachable claim observes it (the v1.1 L4 property, now exhaustive).
- **Staleness is legal** — loom will (must) produce histories where a claim returns the reader's own stale buffer (04 v1.1 §0.6). Assertions may not forbid it. If your model can't express "stale return is legal", the model is wrong, not the protocol.
- Evidence: full loom transcript → `litmus/evidence/loom/loom.txt`.
- **Then re-issue RFC-0001** (`weft-docs/rfcs/0001-triad-exchange-protocol.md`): status Draft → **Accepted**, citing the loom transcript + TSAN ×5 + 24/24 as the evidence triad. This flip is a Phase 0.5 deliverable, not a formality: **RFC-0001 must be Accepted before any benchmark number is quotable in public.**

Pass = exhaustive run completes with zero failed assertions. Budget 0.5 d. If loom explodes combinatorially, reduce to 2 publishes × 2 claims **only with the reduction documented in the transcript header** — do not silently shrink the model.

---

## 2. The benchmark catalog (`bench/catalog.yaml`, v1)

Five benchmarks. Every one runs on all three kernels. Same contract family as litmus: catalog-defined params, runner emits one JSON line, driver assembles. Params below are **catalog-owned**; executor may not tune them.

| ID | Question it answers | Setup | Metrics | Gate |
|---|---|---|---|---|
| **B1-pub-throughput** | How fast can one writer publish? | Unpaced writer, reader idle. `payload_max ∈ {256, 4096}`. Warmup per §4.4, measure 3 s. | block `ops_per_s`; sampled per-op `p50/p90/p99/p999/max` | — (informational) |
| **B2-contended** | What does full contention cost? | Writer AND reader both unpaced, hammering `latest`. Measure 3 s. | `publishes_per_s`, `claims_per_s`, sampled tails both sides | — (informational) |
| **B3-scaling-fingerprint** | Is claim zero-copy? **Prove it.** | Claim sampled p50 across `payload_max ∈ {64, 256, 1024, 4096, 65536}`; writer idle-paced 1 kHz; ≥ 5000 samples per size. | per-size `p50` + `ratio = p50(64K)/p50(64B)`; same sweep for publish (expected linear — memcpy by design, informational, labeled) | **STRUCTURAL: ratio < 2.** A copier shows ~1000× (64 KB memcpy ≫ 64 B swap). ratio ≥ 2 = copy-on-read suspected → STOP and file. |
| **B4-display-adversarial** | The number that goes in the README. | Writer paced `∈ {60, 240, 960}` Hz × reader holds `∈ {0, 5, 10, 50}` ms — 12 sub-configs, the **schedule-shape matrix** (vary shape, not just frequency — the C1–C4 lesson). Measure ≥ 10 s per config, per-second aggregates. | reader `delivered_frames_per_s`; **publish sampled `p99`/`p999` under adversarial reader** (wait-freedom evidence under load — the I2 correction); claim `p99` | — (informational; headline output) |
| **B5-memory-contract** | Law 2's teeth: zero allocation at steady state. | Post-init, 10⁶ frames. Allocation counting per §5. | `alloc_bytes_delta`, `alloc_count_delta`, `rss_growth_pages` | **STRUCTURAL: C/Rust `alloc_bytes_delta == 0` AND `alloc_count_delta == 0`; RSS growth ≤ 2 pages / 10⁶ frames** (nonzero requires written explanation). **TS: advisory only, honesty-labeled "GC-noisy"** — no gate, no shame, no hiding. |

Notes: B2 on this 2-core sandbox measures scheduler + cache-line ping-pong as the signal — that is intended and env-labeled. B4's 12 sub-configs × 3 languages = 36 cells; it is the bulk of the runtime budget (~10 min/language).

---

## 3. Contracts (extends the 05 pattern — do not invent new shapes)

- **Binaries:** `core/c/bench` · `core/rust/target/release/bench` · `node core/ts/bench.ts`.
- **CLI:** `<runner> <BENCH_ID> key=value …` — catalog params only; unknown param = exit 2. Same discipline as 05 §1.
- **Output:** exactly ONE JSON line as the last stdout line; diagnostics → stderr. Shape: `{"bench":"B1-pub-throughput","lang":"c","pass":true,"metrics":{…},"notes":"≤200 chars"}`. `pass` is the structural gate where one exists (B3, B5), else `true` with the note "informational". Exit 0/1/2 as 05 §2.
- **Metric keys** are exactly as cataloged (§2 table); driver schema-lite validation (presence + type). Add `"mode"` inside metrics where dual-mode measurement applies (§4.2).
- **Catalog:** `bench/catalog.yaml` — `catalog: weft-bench`, `version: 1`, same validator discipline: `tools/validate_catalog.py --catalog bench/catalog.yaml --kind bench`. All 5 IDs present and ordered · every §2 param present · no unknown top-level keys.
- **Driver:** `tools/bench_driver.py` — per-language sequential (2 cores, 06 §1), per-cell timeout 120 s, assembles `bench/REPORT.md` + `bench/results.json`. REPORT carries: matrix table, environment block (§4.6), methodology attestation ("rules §4.1–4.8 verified: <list>"), the catalog's question strings so it reads standalone, and **sha256 of results.json in the header** (bench/README §7: hand-edited numbers do not exist as a concept).
- **Makefile:** `make build-c-bench` (kernel + bench runner, same warning-clean flags) · `make bench` (build all, run driver) · `make bench-compare A=… B=…`.
- **Rust constraint unchanged:** zero crates — timing via `std::time::Instant`, allocation counting via a `GlobalAlloc` counting wrapper (std-only). C allocation counting via a ~40-line `LD_PRELOAD` interposer on `malloc/free/posix_memalign` (runner-side artifact, kernel untouched). TS: `process.hrtime.bigint()` for clocks; `process.memoryUsage().heapUsed` deltas labeled advisory.

---

## 4. Methodology — the moat (non-negotiable; every rule is auditable from the REPORT attestation)

**4.1 Clocks:** `clock_gettime(CLOCK_MONOTONIC)` / `std::time::Instant` / `process.hrtime.bigint()`. Never wall-clock, never `Date.now`, never `System.currentTimeMillis`.

**4.2 Two measurement modes, labeled per metric:**
- **block mode** — time K ops as one block, divide. For sub-µs ops, K ≥ 1000× clock-call overhead (measured once per language and reported: `clock_overhead_ns`). This is the honest throughput number.
- **sampled per-op mode** — time every Nth op individually, N ≥ 256, for distribution shape. Sampled per-op **includes** timing overhead (~2 clock calls); that is expected and bounded.
- **Cross-check (methodology gate):** `sampled_p50 − block_mean` ≈ `clock_overhead_ns` (constant offset). Discrepancy > 3× clock overhead = measurement bug → STOP and file. This check would have caught the old report's copy-on-read contamination.

**4.3 Tails are mandatory:** every latency metric reports `p50/p90/p99/p999/max`. **No trimming, no min-of-N, no outlier discarding, no re-run-until-pretty.** If an external event pollutes a run (documented), re-run the whole config and keep BOTH result files. Wait-freedom lives in the tail; trimming the tail is editing the evidence.

**4.4 Warmup:** ≥ 1 s AND ≥ 10⁵ ops (whichever is later). TS: ≥ 2 s, and the warmup must exercise **both claim branches** (new-frame and stale-return — branch predictor and JIT see both).

**4.5 Windows:** B1/B2: ≥ 30 windows of 100 ms inside the 3 s measurement → mean, stddev, CI95 (bootstrap acceptable) per metric. B3: ≥ 5000 samples per payload size. B4: ≥ 10 s per sub-config, per-second aggregates + pooled sampled tails.

**4.6 Environment capture per run** — no env block = not a result (bench/README §5 hygiene): uname · CPU model · cores · governor + MHz range observed · memory · gcc/rustc/node/python versions · harness + catalog version · UTC timestamp · **env label** (today: `x86_64-sandbox`). Every number in the REPORT carries the label at least once at the top. Cross-machine comparison without an identical label is forbidden.

**4.7 Kernel frozen:** the kernels gain **zero** benchmark code — no timers, no counters, no branches added for measurement. Timing scaffolding lives in runners only. If a benchmark exposes a kernel performance issue: file `PERF-<n>` with the repro command, **do not fix it in Phase 1**. Fixes happen later, each one landing with before/after evidence from this same harness. A kernel tuned to its own benchmark is a brochure.

**4.8 Frequency sanity:** record governor/MHz; if the sandbox allows setting `performance` governor, do it and record; if not, record that it wasn't. Absolute numbers are never gated — only structural gates and regressions are.

## 5. B5 measurement spec (the one with a trap)

- **C:** `LD_PRELOAD` interposer counts bytes + calls on `malloc/calloc/realloc/free/posix_memalign`. Snapshot before the steady-state loop (post-init, post-warmup), snapshot after; `alloc_bytes_delta` = allocated − freed. Gate on delta == 0. RSS from `/proc/self/statm` before/after.
- **Rust:** process-wide counting `GlobalAlloc` (std-only, `unsafe impl`, ~30 lines, SAFETY comment). Same snapshot discipline. Warmup allocations (e.g., Vec for result buffers) are outside the snapshot window — the window is the publish/claim loop only.
- **TS:** no allocator hooks. `global.gc?.()` if `--expose-gc` was passed (record whether it was), else raw `heapUsed` deltas over 10⁶ frames, reported with the note "GC-noisy, advisory". It does not gate. Publishing a TS number that looks like C without this label is exactly the dishonesty the suite exists to prevent.

## 6. Baselines & compare (seed of the release gate)

- First full run per env label → `bench/baselines/<env-label>.json` (committed).
- Every later run: driver computes deltas vs the matching baseline and prints a **compare table** in the REPORT: metric · baseline · now · Δ% · FLAG.
- FLAG policy: `p50 Δ > +10%` or `p99 Δ > +15%` or any structural-gate metric regressed. FLAGS are **investigation-mandatory, not auto-red** on this shared sandbox (noise band must be established empirically — the first two weeks of runs calibrate it; record the band in the catalog when known).
- Self-compare sanity (T7): run the suite twice back-to-back, compare with itself — every Δ must sit inside noise (structurally: flags allowed only on tails ≤ p99, none on p50/block throughput, none structural). A harness that can't reproduce itself is not a harness.

## 7. Divergence rule (A6, refined for benchmarks)

Absolute numbers **may** differ across languages — TS being slower than C is a *finding to publish*, not hide (bench/README §4: a suite that only flatters its author is a brochure). What must hold identically in all three languages: the **structural gates** (B3 ratio < 2, B5 alloc == 0 in C/Rust) and the **schedule identity** wherever the PRNG drives it (B4 hold schedule, catalog seed — A5 differential replay applies). Structural-gate divergence across languages = STOP and file; number divergence = annotate and publish.

## 8. Tasks, order, budget

| # | Task | Est. |
|---|---|---|
| T1 | **L-loom model + evidence + RFC-0001 re-issue (Accepted)** — §1 | 0.5 d |
| T2 | `bench/catalog.yaml` + validator `--kind bench` + `bench_driver.py` skeleton (CLI, JSON contract, env capture) | 0.5 d |
| T3 | C runner: B1, B2, B4 (block + sampled modes, clock-overhead measurement, hold/pacing reuse from litmus harness) | 0.5 d |
| T4 | C runner: B3 + B5 (LD_PRELOAD interposer, RSS, structural gates) | 0.5 d |
| T5 | Rust runner: B1–B5 mirror (GlobalAlloc counter, Instant) | 0.5 d |
| T6 | TS runner: B1–B5 (hrtime.bigint, JIT warmup both-branches, heapUsed advisory; B5 labeled) | 0.5–1 d |
| T7 | REPORT + sha256 + baselines + compare + **self-compare sanity** | 0.5 d |
| T8 | Findings ledger pass (any PERF-n filed), bench/README contract sync if this WO drifted from it, sign-off block | 0.25 d |

**Total ≈ 3.75–4.25 d.** T1 gates everything; T2 gates T3–T6; T7 last.

## 9. Exit checklist (Phase 1 is DONE when all hold)

- [ ] Loom: exhaustive model clean, transcript in `litmus/evidence/loom/`, **RFC-0001 re-issued as Accepted**
- [ ] 15-cell bench matrix (5 × 3 languages) green on all structural gates
- [ ] B3 numbers recorded for claim AND publish sweeps (plot optional, non-normative)
- [ ] B5: C/Rust zero-alloc evidence; TS advisory labeled
- [ ] REPORT.md auto-generated: env block, methodology attestation §4.1–4.8, sha256 header, env labels
- [ ] `bench/baselines/x86_64-sandbox.json` committed; self-compare sanity passed (T7)
- [ ] Clock-overhead per language measured and reported; sampled-vs-block cross-check documented
- [ ] Methodology checklist signed per language; every red = filed finding, nothing papered over
- [ ] Kernels untouched: `git diff core/*/weft.* core/rust/src/lib.rs core/ts/weft.ts` = only T1-permitted comments (none expected)

## 10. Sign-off block (paste at end of bench/REPORT.md)

```
Phase 1 complete: 15/15 bench cells green (structural gates: B3 ratio, B5 alloc).
Methodology §4.1–4.8 attested per language. Baselines committed; self-compare within noise.
RFC-0001: Accepted (loom evidence: litmus/evidence/loom/). All numbers labeled x86_64-sandbox.
Absolute numbers are informational; regression policy per WO-P1 §6.
Signed: <engineer> · Reviewed-by: <steward>
```
