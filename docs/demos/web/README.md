# Weft Showcase Demos (web)

Interactive workload matrix for the Weft Triad Protocol: five canonical
workloads (W1–W5) plus the W6 feed workload, each carried through four
buffer-protocol modes (A/B/C/D), rendered through one shared draw routine
per workload (FAIRNESS PIN), with a Telemetry Inspector and `.weftrec`
Playback on the side.

Run: `pnpm dev` · Test: `pnpm test` (6×4 matrix + conformance + fan-out
litmus, 31 tests) · Build: `pnpm build`.

---

## W6 — L2 Order Book Feed (SIMULATED)

W6 is the feed-shaped workload: a synthetic L2 order book ingests **50
delta messages per display tick** (add / modify / cancel / trade — the
50:1 feed:display pressure regime) and folds them into **one latest-wins
frame** of 584 floats (64-level ladder × 6 + 64-trade tape × 3 + 8-float
header). The engine is integer-exact by construction (prices in cents,
sizes in lots) and allocates nothing in steady state.

**Honesty labels** (the workload title carries these too):

- **SIMULATED FEED** — deterministic synthetic microstructure (LCG,
  seed pinned in `src/workloads/l2feed.ts`). No exchange connectivity;
  nothing here was measured against a real venue feed.
- **DEMO SCALE** — 64 levels + 64 trades; a real L2 snapshot is
  comparable in size, but the fold ratio (50:1) is a demo-scale model of
  the real feed:display gap (which is orders of magnitude wider).
- **Model boundary** — prices live on a fixed grid recomputed from a
  walking mid; price-time queue positioning (L3) is out of scope.

### The RFC-0004 fan-out panel

When W6 is selected, a second panel runs below the mode matrix: **one
`WeftFanoutBroadcaster` driving three `WeftFanoutCanvas` consumers**
(depth ladder, trade tape, stats HUD), each with its own reader and its
own drop accounting (`claim.dropped` — visible in the stats view). The
producer runs on the main thread at rAF cadence (~3,000 msgs/s simulated
at 60 Hz); the cross-thread writer case is covered by
`test/feedfanout.test.ts`, which runs an independent worker-side writer
(the F-series litmus tradition) for 40,000 frames / 2M folded messages
with zero invalid claims.

### Measured: feed-pressure GC comparison

`pnpm bench:feedgc` (Node ≥ 22.6 with type stripping; requires
`--expose-gc`, which the script sets) runs the same deterministic stream
through all four modes and writes `evidence/feed-gc-bench.log`.
Environment: **node v24.19.0 · linux x64 (sandbox)** — medians of 5 reps:

| mode | ingestion alloc (B/100 ticks) | GC events (3k ticks) | GC pause | worst tick (P100) |
|---|---:|---:|---:|---:|
| A reactive model | ~900,000 | 15 | ~2.0 ms | ~250 µs |
| B pooled | 144 (noise) | 0 | 0.00 ms | ~18 µs |
| C weft | 8,944 ¹ | 0 | 0.00 ms | ~19 µs |
| D hand-rolled | 144 (noise) | 0 | 0.00 ms | ~18 µs |

¹ By subtraction against B/D (same engine, other transport), Mode C's
8.9 KB/window is the kernel TS port's per-publish advisory telemetry —
BigInt atomics (`t_publish`, `t_wsteps`, `BigInt(seq)` canary) at ~90 B
per publish — a declared TS-port cost (06-PITFALLS §4 "safe but noisier"),
not a W6-engine cost. The consume path adds one `rReadSlice` view per
claim (the kernel's public read API) plus ≤ 3 cached Float32 wrappers.

Mode A **models** per-message reactive event materialization (one object
per feed message + object-graph book + fresh snapshot per tick) — it is
not a measurement of any specific framework. Method, known biases
(sampling overhead < 1 KB/window, identical across paths; event counts
are lower bounds at default heap settings), and the full table are in
the log. This is the repo's "no GC scan overhead" thesis carried as a
measured, environment-tagged artifact — the claim's boundary included.
