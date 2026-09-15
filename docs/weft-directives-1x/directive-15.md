# DIRECTIVE-15 — Interactive showcase demos: W1–W5 with A/B/C/D mode toggles (web)

- Wave: 2 · Depends: D-11 · Effort: ~10 h · Status: ISSUED

## 1. Context

The five canonical macro workloads (W1 Audio Visualizer, W2 10k Particle Simulation, W3 Sensor Gyroscope, W4 EEG Telemetry, W5 L2 Order Book) exist as headless benchmark routines (W-suite, A–D Python models, single shared `draw_spectrum` fairness pin). The owner wants real UI demos with live A/B/C/D toggles. Per the pivot: **web demos only in this directive** (mobile demos arrive with D-12/D-13 follow-ups); all numbers environment-tagged.

Mode semantics (pinned from WO-P5 release rulings):
- **A** = reactive naive · **B** = pooled best-practice · **C** = weft (this kernel) · **D** = hand-rolled triple-buffer with envelope + I6.

## 2. Tasks

- **T15.1 Shared workload cores.** Port the W-suite workload generators (from `bench/workloads/`) to TS once, in `@weft/core`-consuming demo-core package; all four modes implement the same workload contract and the **same shared draw module** (fairness pin discipline carried over: one `drawFrame` implementation, mode-agnostic).
- **T15.2 Demo shell.** Single Vite app: workload selector (W1–W5) × mode toggle (A/B/C/D) × run/pause, hot-switchable without teardown (switch = new Heddle attach, no page reload). Frame-time graph (p50/p99), `t_drop` counter, mode label burned into the overlay.
- **T15.3 Honesty overlay.** A/B are expected to jank or allocate — that is the comparison's point. Overlay shows GC pressure indicator where available; a footer carries the environment tag (`chromium/linux-sandbox`) and states device numbers do not exist in this program.
- **T15.4 Workload specifics.** W3 gyroscope: synthetic sensor stream (no device — labeled "synthetic source"); W5 order book: deterministic replay from a seeded generator; W1/W2/W4 as in W-suite catalog (same params).
- **T15.5 Deploy.** Static build under `demos/web/` (GitHub Pages-capable); COOP/COEP headers in the deploy config for the SAB path (§8.2 discipline).

## 3. Non-goals

No mobile demos here. No new benchmark numbers for the W-suite catalog (demos display live numbers, they don't amend `results.json`). No kernel diffs. Mode internals must not be tuned to make A/B look worse — the W-suite models are the contract.

## 4. Acceptance criteria (mechanical)

1. 5×4 matrix runs; every cell reaches steady state (or an honest degradation label for A in W2/W4 — declared per cell in the report).
2. C and D cells: 60 fps steady (p99 frame time ≤ 16.7 ms over 60 s, `chromium/linux-sandbox` tag), `t_drop` == 0, heap delta ±2 MB over 60 s.
3. Fairness pin: single shared draw module in the tree (import graph check — no mode-local draw paths).
4. Mode hot-switch: 100 switches across workloads without page reload, no leaked listeners (heap snapshot count stable).
5. Static deploy builds with COOP/COEP headers; SAB path active in the C cell (fallback path exercised by test).

## 5. Evidence to return

`evidence/D-15/`: per-cell 60 s log (fps p50/p99, t_drop, heap delta + tags), switch-cycle heap snapshots, import-graph check output, deploy config.

## 6. Report

`reports/D-15-REPORT.md` per index §4, with the per-cell matrix as its centerpiece table.
