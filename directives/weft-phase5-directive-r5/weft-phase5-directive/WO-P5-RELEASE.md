# WO-P5-RELEASE — Phase 5: Sandbox Release Artifact (Tarball + W-Suite + Site)

```
Directive:   weft-phase5-directive stream
Doc ID:      WO-P5-RELEASE
Version:     v1.0
From:        Staff adjudication
To:          Executor
Status:      ACTIVE — begins after T0 (WO-P4-CLOSURE batch C1–C3)
Depends:     Phase 2/3/4 CLOSED (v1.0.3 + soak evidence + ports ratified), contracts v1.3
Budget:      ~31h ≈ 3.9 engineer-days (incl. the ~1.5h C1–C3 batch)
Kernel:      FROZEN — zero code changes to any kernel this phase. Release engineering only.
```

---

## 0. Authority, context, order of work

Roadmap §5 contract: the sandbox-buildable project is complete at this phase. The
deliverable is **one tarball** (`weft-sandbox-v0.1.tar.gz`) that a contributor with a real
dev machine can unpack, build, and use, plus `INSTALL.md`, `SHA256SUMS`, and
`Weft-Phase5-Release-Report.pdf`.

This work order also resolves the **pre-registered gap** (WO-P1-CLOSURE ledger): the P5
gates reference `bench/workloads/` (W1–W5) and `bench/site/` which the B-cell Phase 1
deliberately did not build. They are built here, in-sandbox, per the roadmap's own §1b–1d
definitions.

**Order of work:** (T0) WO-P4-CLOSURE batch C1–C3 — the release ships the whitepaper, so
the whitepaper must be final first. Then T1 onward.

**Standing decisions for this phase (staff-ruled, implement as specified):**

1. **Canonical results isolation.** The tarball ships the Phase 1 bench bundle
   (`bench/results.json`, sha256 `16b5c663…`) **as-is** — it is the whitepaper's pinned
   single source. The `make bench` release gate verifies the harness **runs green**, NOT
   that it bit-reproduces `16b5c663`: results.json contains timing data, and re-running
   any benchmark lawfully produces a different hash. A re-run that overwrote the shipped
   bundle would silently break the whitepaper's hash index and Appendix A contract.
   Re-run output goes to a scratch path and is discarded after the gate; only the
   canonical bundle ships. W-suite results write to **separate bundle files**
   (`bench/results/wsuite-*.json`), never into results.json.
2. **W-suite scope = roadmap §1b–1d exactly: four Python-model implementations A–D.**
   A = reactive naive (Python list, redraw every frame); B = best practice (pooled
   `array.array`, deferred draw into a pre-allocated render buffer); C = Weft (C kernel
   via ctypes); D = hand-rolled triple-buffer (same envelope + I6 contract). The shared
   draw routine is the fairness pin — one `drawBar`-class function, byte-identical logic
   across A–D, parameterized only by backend. Do NOT invent a Rust W-cell: the roadmap's
   P5 gate phrase "runs the harness against C and Rust kernels" is satisfied by the
   existing B-cells (C/Rust/TS) plus W-C (ctypes); §1c defines no Rust W-implementation,
   and inventing one would smuggle an unmodeled implementation into a fairness-pinned
   suite.
3. **Workloads W1–W5 per roadmap §1b, verbatim bounds:**
   W1 audio visualizer (1024-float PCM @ 60/120 Hz) · W2 particle field (500 × 6-DOF RK4
   @ 120 Hz) · W3 spectrogram/heatmap (256×64 float matrix @ 60 Hz) · W4 data grid (10k
   rows × 20 cols, fixed-cell subset updates) · W5 order book (1000 levels × 10 fields,
   60 Hz L2 feed).
4. **Metrics per §1d, with sandbox-honesty labels:** P50/P99/P100 FPS (**P99 is the
   headline** — same rule as the B-suite), allocation bytes/frame (asserted 0 for C and
   D), GC pause count + total ms via `gc.get_stats()`, CPU% via psutil, cold-start
   overhead, thermal sustained (30-minute W2 run, FPS decay curve). Label the thermal
   cell honestly: a headless server has no meaningful thermal envelope; if the curve is
   flat, the report says "no thermal decay observable on headless server — expected;
   device thermal is Phase 6+." No invented throttling.
5. **The site is a static generator, not a web app.** `make site` renders the result
   bundles to plain static HTML (no JS build chain, no framework): one page per suite
   (B-cells, W-cells), tables first, every number carrying `[MEASURED x86_64-sandbox]`,
   structural gates stated as the only normative claims, telemetry marked advisory
   (AXIOM T), and a banner that `weft.dev` is deferred (roadmap: public launch Phase 6+).
   The site consumes bundles; it never computes numbers itself.
6. **Fresh-machine criterion, in-sandbox proxy.** "Builds cleanly on a fresh Linux
   machine" is tested as: unpack the tarball into an **empty directory**, run all five
   make targets, log results. The literal fresh-machine claim stays honest in
   INSTALL.md: verified in-sandbox on the equivalent of a clean tree; contributor
   confirmation on foreign hardware is the Phase 6+ / post-release loop.
7. **Tarball tree = roadmap §5a verbatim.** `core/{c,rust,ts}` litmus-passing;
   `core/{kotlin,swift,dart}` source-only with STATUS banners; `steward/`, `heddles/`
   per-language/framework; `litmus/` (catalog, runners, REPORT.md, evidence/ incl. loom
   v1+v2 transcripts and TSAN 5×8); `bench/` (harness, workloads, results, site);
   `tools/{weft-probe,weft-record}`; `docs/` (all docs + **whitepaper v1.0.3** +
   ERRATA); `rfcs/`; `reports/` (all phase PDFs); `README.md`; `INSTALL.md`; top-level
   `Makefile` + `SHA256SUMS`.
8. **Release notes document the one kernel delta:** the Phase 2 debug-view accessor
   (semver minor, both kernels, read-only/wait-free per WO-P2 T1). Nothing else has
   touched the kernel since Phase 0 freeze; say so.
9. **AXIOM T and Law 4 apply to release artifacts.** No number on the site or in the
   report without the MEASURED + env label; no claim without its boundary.
10. **Quantitative misses are declared** (standing rule), and **verification claims name
    scan scope + threshold** (standing rule addendum, WO-P4-CLOSURE §2 P4-W1).

## 1. Tasks

### T0 — WO-P4-CLOSURE batch C1–C3 (~1.5h)

Exactly as specified in WO-P4-CLOSURE §3: v1.0.3 scan-clean (611.5pt criterion, whole
document); B2 evidence supplement (or World-B fix + re-run); Phase 4 report errata.
This closes Phases 2, 3, and 4.

### T1 — W-suite: configs + shared draw routine + A–D implementations (~8h)

`bench/workloads/`: five config files (W1–W5, bounds per decision 3) + the shared draw
routine module + four backend implementations per decision 2. The fairness pin is
mechanical: the draw routine is one module imported by all four backends; backend
selection at runtime; workload code identical regardless of backend. D carries the
envelope + I6 contract hand-rolled (its purpose is to show a competent hand-rolled
triple-buffer still loses to a specified protocol on the metrics that matter — if it
doesn't lose, that is a finding to publish, not to bury).

### T2 — W-cell harness integration + metrics (~4h)

Extend `bench_driver.py` with W-cells: per workload × per implementation, collect P50/
P99/P100 FPS (P99 headline), alloc bytes/frame (assert 0 for C and D — assertion failure
= RED), GC pauses (count + total ms), CPU%, cold-start. Output: one JSON line per cell,
same contract pattern as B-cells; bundles to `bench/results/wsuite-<env>.json` (decision
1). Env capture block identical to B-cells (`x86_64-sandbox` label).

### T3 — Thermal-proxy run (~1h active, 30m wall-clock)

W2 × all four backends × 30 minutes sustained; FPS decay curve per backend into the
bundle. Flat curve → honest label per decision 4. This is the only long-running gate;
schedule it so it doesn't serialize the rest of the phase.

### T4 — Static site generator (~4h)

`tools/make_site.py` + `make site`: consumes result bundles → `bench/site/` static HTML
per decision 5. Pages: index (what is normative vs informational), B-suite tables,
W-suite tables (P99 headline), reproducibility page (mirrors whitepaper Appendix A
commands + the sha256-expectation honesty). Zero client-side JS required to read the
numbers.

### T5 — Makefile + release wiring (~3h)

Top-level `Makefile` with the five contracted targets: `make litmus` (24 cells, exit 0),
`make bench` (B-cells + W-cells; canonical bundle isolation per decision 1), `make site`,
`make validate` (port_validator over all four source-only targets), plus `make build-*`
as needed. Wire `make validate` to fail the release on any non-zero validator exit.

### T6 — Tarball + INSTALL.md + SHA256SUMS + release notes (~3h)

Assemble `weft-sandbox-v0.1.tar.gz` per decision 7's tree. `INSTALL.md` per roadmap §5b:
in-sandbox vs real-device split, Rust+Node+Python steps, JDK/Android SDK, Xcode, Flutter
SDK sections (the latter three explicitly "not in-sandbox — Phase 6+"). `SHA256SUMS`
covers the tarball, the whitepaper v1.0.3 PDF, and all shipped report PDFs. Release
notes per decision 8 (the debug-view semver note; kernel otherwise untouched since
freeze).

### T7 — Clean-tree validation (~2h)

Unpack the tarball into an empty directory; run all five make targets in sequence; log
every exit code. Gate: litmus 24/24 exit 0; bench green (both suites); site regenerates
byte-stable given identical bundles (deterministic render — no timestamps in the HTML
output, or the diff check excludes a declared generated-on field); validate 4/4 exit 0.
Any RED → failure protocol, no workaround.

### T8 — `Weft-Phase5-Release-Report.pdf` + doc pointers (~2h)

Same typeset pipeline. Contents: T0 batch evidence recap; W-suite design notes (fairness
pin mechanics, D-vs-C findings published honestly); thermal honesty label; site
structure; clean-tree validation log summary; SHA256SUMS listing; known limitations
(what the sandbox cannot prove — device matrix, real UI stacks, fresh-machine literal,
all Phase 6+); deviations field **mandatory**; verification claims with scope +
threshold. Update repo README / docs index to point at the release.

## 2. Rules (non-canonical→canonical summary, non-negotiable)

1. Kernel FROZEN. Zero kernel changes. Doubt = RFC.
2. Canonical bundle `16b5c663` ships untouched; re-runs discard to scratch (decision 1).
3. No number without `[MEASURED x86_64-sandbox]`; gates normative-first; telemetry
   advisory (AXIOM T).
4. Site renders bundles; never computes.
5. Validator green is a release gate; weakening a check = RED.
6. Honest labels for: thermal proxy, fresh-machine proxy, device deferrals.
7. Quantitative misses declared; verification claims name scope + threshold.
8. Failure protocol: 07-ACCEPTANCE §6 — report the RED.

## 3. Success criteria (mechanical)

- [ ] T0 batch C1–C3 evidence complete (Phases 2/3/4 close first)
- [ ] W-suite: 5 workloads × 4 implementations, shared draw module, bounds verbatim
- [ ] W-cell bundles separate from canonical results.json; 16b5c663 unchanged in sha
- [ ] alloc/frame assertion enforced for C and D (RED on violation)
- [ ] 30-min thermal run archived with honest label
- [ ] `make site` regenerates deterministic static HTML from bundles
- [ ] Clean-tree run: litmus 24/24 exit 0; bench green; site green; validate 4/4 exit 0
- [ ] Tarball tree = §5a; INSTALL.md = §5b; SHA256SUMS complete; release notes note the
      Phase 2 semver minor as the only kernel delta since freeze
- [ ] Report PDF typeset; deviations filled; verification claims scoped

## 4. Exit checklist

- [ ] T0 C1–C3 closed
- [ ] T1 workloads + fairness pin
- [ ] T2 W-cell harness + bundles
- [ ] T3 thermal run archived
- [ ] T4 site generator + pages
- [ ] T5 five make targets wired
- [ ] T6 tarball + INSTALL + SHA256SUMS + notes
- [ ] T7 clean-tree log archived
- [ ] T8 report + doc pointers

## 5. Budget

| Task | Estimate |
|---|---|
| T0 C1–C3 batch | 1.5h |
| T1 W-suite A–D + draw pin | 8h |
| T2 W-cell harness + metrics | 4h |
| T3 thermal 30-min run | 1h |
| T4 site generator | 4h |
| T5 Makefile wiring | 3h |
| T6 tarball + INSTALL + sums | 3h |
| T7 clean-tree validation | 2h |
| T8 report | 2h |
| **Total** | **~31h ≈ 3.9 engineer-days** |

## 6. Out of scope

Public weft.dev launch (Phase 6+), device matrix and real UI stacks (A/B device
implementations, Phase 6+), literal fresh-machine verification on foreign hardware
(post-release contributor loop), TSAN re-runs (evidence ships as-is from Phase 0 G3),
any new kernel surface, cross-process/shm (Phase 6+), Pro tier (charter clause 4).

## 7. Sign-off block (executor completes)

```
WO-P5-RELEASE execution report
- T0 C1–C3:              [ ] v1.0.3 scan-clean | [ ] B2 evidence | [ ] P4 errata
- T1 W-suite:            [ ] 5 workloads | [ ] shared draw | [ ] A/B/C/D | [ ] bounds verbatim
- T2 harness:            [ ] P99 headline | [ ] alloc assert C/D | [ ] wsuite bundles separate
- T3 thermal:            [ ] 30-min x4 archived | [ ] honest label
- T4 site:               [ ] static | [ ] deterministic | [ ] MEASURED labels
- T5 make targets:       [ ] litmus | [ ] bench | [ ] site | [ ] validate | [ ] build
- T6 release:            [ ] tarball tree §5a | [ ] INSTALL §5b | [ ] SHA256SUMS | [ ] notes
- T7 clean-tree:         [ ] 24/24 exit 0 | [ ] bench green | [ ] site green | [ ] validate 4/4
- T8 report PDF:         [ ] + doc pointers

Deviations/findings (mandatory field — 07-ACCEPTANCE §6):
<none | list>

Sign-off:
- Executor: ______________  date: ______
- Staff review: ______________
```
