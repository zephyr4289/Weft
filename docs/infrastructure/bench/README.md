# The Weft Benchmark Suite

> Without numbers, Weft is a blog post. With numbers, it is the standard. This suite is the moat:
> the asset a platform vendor can absorb code but cannot absorb credibility.

This directory contains the workload definitions, device matrix, harness specification, and
methodology for the open, reproducible benchmark program published at `weft.dev/benchmarks`.
Anyone can fork, run on their own hardware, and submit a result — and every result ships with the
script that produced it.

---

## 1. The fairness pin (read before anything else)

**Implementations A, B, C, and D run byte-identical draw code. Only the state plumbing differs.**

Every implementation calls the same shared `drawBar`-class routine with the same values. Any FPS
difference between implementations is therefore attributable to the state layer — recomposition,
allocation, GC, marshalling, handoff — and to nothing else. Without this pin, the entire suite is
attackable: "your Weft demo draws fewer bars." The harness verifies the pin mechanically (same
draw-call count per frame, same element count) and records the verification in every result.

Second pin: **B is platform best practice, done seriously** — deferred draw-phase reads, pooled
arrays, no amateur mistakes. A vs. C proves hot state breaks naive reactive code; *B vs. C* is the
result that matters, and it is the comparison all headline charts lead with.

## 2. Workloads

Five workloads, each stressing a different dimension. All fixed-bounds by design — Weft does not
claim to fix layout-bound hot state (Law 4), so the suite measures what the plane actually serves.

| # | Workload | Hot state | Bounds | Why this workload |
|---|---|---|---|---|
| W1 | Audio visualizer | 1024-float PCM @ 60/120 Hz | Fixed | The canonical hot-state surface: draw-phase reads + GC pressure. The headline workload. |
| W2 | Particle field | 500 particles × 6-DOF RK4 @ 120 Hz | Fixed | Native write throughput + atomic handoff; each particle also a reactive element (the "why not a shader" case). |
| W3 | Spectrogram / heatmap | 256×64 float matrix @ 60 Hz | Fixed | Dense numeric payload; replaced the founding draft's token-stream workload, which was layout-bound and off-limits by non-goal. |
| W4 | Data grid | 10k rows × 20 cols, live updates | Fixed-cell subset | Large-buffer identity + JNI zero-copy. Only the fixed-bounds live-cell subset uses Weft; scroll is framework territory. |
| W5 | Order book | 1000 levels × 10 fields, 60 Hz L2 feed | Fixed | Cross-thread write protocol + reader-faster-than-writer pacing (60 Hz feed, 120 Hz display). |

## 3. Device matrix

Chosen to span the performance envelope; every row is commercially common, no lab curiosities.

| Tier | Device | Chip | RAM | OS | Display |
|---|---|---|---|---|---|
| Low Android | Realme C55 | Helio G88 | 4 GB | Android 13 | 60 Hz |
| Mid Android | Pixel 7a | Tensor G2 | 8 GB | Android 14 | 90 Hz |
| High Android | Galaxy S24 | SD 8 Gen 3 | 12 GB | Android 14 | 120 Hz |
| Low Apple | iPhone SE (2022) | A15 | 4 GB | iOS 17 | 60 Hz |
| High Apple | iPhone 15 Pro | A17 Pro | 8 GB | iOS 17 | 120 Hz ProMotion |
| Web | M2 MacBook Air | — | 16 GB | Chrome 131 / Firefox 132 / Safari 17.6 | 120 Hz |

Platform honesty that carries into results: **no 120 FPS claims on Safari** (rAF capped at 60 Hz
by default — WebKit bug 173434). Safari rows report 60 Hz results and say so.

## 4. Implementation matrix

| Impl | Description | Role |
|---|---|---|
| **A — Reactive naive** | Hot state in `MutableState` / `useState` / `@State` | The failure mode. Lower bound. |
| **B — Platform best practice** | Deferred draw-phase reads, pooled arrays, framework-canonical hot paths | **The real competitor.** The honest baseline. |
| **C — Weft** | Steward + Triad Protocol + Heddle | The candidate. |
| **D — Hand-rolled** | Production reference implementation (the pattern Weft was extracted from) | The ceiling. Shows what Weft leaves on the table. |

If C ≈ D, the library costs nothing. If C ≈ B on raw FPS but wins on P99 / GC / native-write
safety, that *is* the headline, stated without embarrassment. If C < B on anything — publish that
too. A benchmark suite that only ever flatters its author is a brochure.

## 5. Metrics

Per workload × device × implementation, a **100-second run** records:

| Metric | Target / note |
|---|---|
| **P50 / P99 / P100 FPS** | **P99 is the headline**, not P50. Smooth medians hide jank; P99 does not. |
| **Frame allocation rate** | bytes/frame, averaged + asserted: **C and D must measure 0 B/frame or the run fails.** |
| **GC pause count + total ms** | Young-gen and full GC reported separately. |
| **CPU%** | UI thread, RenderThread, and native writer thread, reported individually. |
| **Battery drain** | mAh/min; fixed 50% brightness, airplane mode + WiFi; **3 runs minimum, median + IQR reported** (software battery stats are coarse — say so). |
| **Cold start overhead** | Added ms vs. baseline; target < 5 ms. |
| **Thermal sustained** | **30-minute run** (not 100 s): FPS decay curve + throttle events. Mid-tier throttling is where GC jank compounds; burst-only numbers are marketing. |

Measurement hygiene:

- Allocation instrumentation **off** during steady-state measurement (tracking skews counts);
  verified by a short instrumented run adjacent to each measured run.
- Device state logged per run: thermal status, battery level, background app set. A result
  without device state is not a result.
- Android FPS via frame-deadline statistics (`dumpsys gfxinfo` / JankStats), not wall-clock frames.

## 6. Predicted headline (labeled: PREDICTION, not measurement)

Pixel 7a, W1 (audio visualizer), 120 Hz writer — *projected from the reference production
codebase; to be replaced by measured values at Phase 1. Published predictions keep the project
honest when the measurements arrive.*

| Impl | P50 FPS | P99 FPS | Bytes/frame | GC pauses/sec |
|---|---|---|---|---|
| A — Reactive naive | ~11 | ~6 | ~2.3 KB | ~47 |
| B — Best practice | ~116 | ~54 | ~380 B | ~3 |
| C — Weft | 120 | 120 | 0 | 0 |
| D — Hand-rolled | 120 | 120 | 0 | 0 |

## 7. Reproduction and submission

1. Fork the repo; the harness is self-contained per platform (`bench/android/`, `bench/web/`,
   later `bench/ios/`).
2. Pin: device, OS build, brightness, airplane mode, ambient temperature if you have it.
3. Run: `./bench run --workload W1 --device <auto> --impls A,B,C,D --duration 100s`
   (shape of the CLI; landing with Phase 1).
4. The harness emits a signed result bundle: raw frame timestamps, alloc counters, GC logs,
   device state, harness version, and the draw-code pin verification.
5. Submit the bundle via PR to `bench/results/`. Site regeneration is automatic from bundles.
   Hand-edited numbers do not exist as a concept in this pipeline.

## 8. The release gate

A Weft release candidate that regresses any published C-row metric (P99 beyond noise band,
any nonzero bytes/frame) **fails release** and blocks until fixed or the regression is publicly
explained. The Laws are enforced here: this gate is Law 2's teeth at release time, and the reason
the philosophy survives contact with a thousand contributors.

## 9. T-series evidence benches (RFC 0012 TLEL)

The T-bench family (`core/c/turbo_runner.c`) is evidence tooling for the
Tail-Latency Eradication Layer — not catalog benches; parameters are
runner-owned and every cell runs in its own process (the B-suite
methodology, applied per-variant). Modes: `caps`, `TL-writer`,
`TL-reader`, `TL-ring`, `ING-uring`. Committed evidence (3 reps per cell,
A-B interleaved) lives in `litmus/evidence/turbo/` with the honesty labels
in [RFC 0012](../../rfcs/0012-tail-latency-turbo.md): MEASURED (reader
early-hint p50 −27%, +pin p99 −45%, placement determinism) vs PREDICTED
(THP/TLB on real hardware, FIXED/multishot ingestion on kernel ≥ 5.19,
cross-NUMA, SCHED_FIFO). The zero-regression guardrail re-runs B1/B2/B3
and the F-series alongside every T-bench collection.
