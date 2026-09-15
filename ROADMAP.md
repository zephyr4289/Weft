# Roadmap

Sequential, benchmark-gated, and honest about what is conditional.

**Current status: Coordinated Release v0.1.0 (Series 1x Complete).**

> [!NOTE]
> **Owner Binding Pivot**: Physical cross-device hardware testing has been permanently abandoned across all phases. Verification is strictly build + unit/Robolectric/simulator + x86_64/arm64-sandbox CI runners. All published metrics carry explicit environment tags (`termux-arm64`, `linux-ci`, `macos-14`, `chromium/linux-sandbox`).

---

## Series 1x Engineering Execution Ledger (Directives D-10 — D-19)

- **D-10 (Wave 0 — Canonical Integrity & Forensics)**: CLOSED (`421494e`, [`reports/D-10-REPORT.md`](reports/D-10-REPORT.md))
- **D-11 (Wave 1 — npm Packages Ecosystem)**: CLOSED (`ab9425f`, `3364d06`, [`reports/D-11-REPORT.md`](reports/D-11-REPORT.md))
- **D-12 (Wave 1 — Android / Kotlin Packaging)**: CLOSED (`2abe842`, [`reports/D-12-REPORT.md`](reports/D-12-REPORT.md))
- **D-13 (Wave 1 — Apple / Swift Package Manager)**: CLOSED (`f5e65a4`, [`reports/D-13-REPORT.md`](reports/D-13-REPORT.md))
- **D-14 (Wave 1–2 — Flutter / Dart FFI Package)**: CLOSED (`4f061c0`, [`reports/D-14-REPORT.md`](reports/D-14-REPORT.md))
- **D-15 (Wave 2 — Interactive Showcase Demos W1–W5)**: CLOSED (`614bbb5`, [`reports/D-15-REPORT.md`](reports/D-15-REPORT.md))
- **D-16 (Wave 2 — Telemetry Inspector & .weftrec Playback)**: CLOSED (`fa5ff75`, [`reports/D-16-REPORT.md`](reports/D-16-REPORT.md))
- **D-17 (Background — Protocol RFC Spikes Q1–Q5)**: CLOSED (`f338bba`, [`reports/D-17-REPORT.md`](reports/D-17-REPORT.md))
- **D-18 (Wave 3 — Release Engineering & Manifest Matrix)**: CLOSED (`2284f1d`, [`reports/D-18-REPORT.md`](reports/D-18-REPORT.md))
- **D-19 (Wave 4 — End-to-End Integration Gate & Coordinated v0.1.0)**: CLOSED ([`reports/D-19-REPORT.md`](reports/D-19-REPORT.md))

---

## Phase 0 — Spec + Spike (weeks 1–2)

**Deliverable:** this documentation set (done) + the Triad Protocol spike: Kotlin reference
implementation (~500 lines, zero dependencies) + litmus runners L1–L8, against a fake 120 Hz
writer on a Pixel 7a.

**Success criterion:** L1 tears = 0 under adversarial holds (5/10/50 ms); steady-state
alloc/frame = 0; writer step bound holds on release builds. [RFC 0001](rfcs/0001-triad-exchange-protocol.md)
flips to `Accepted` when the suite goes green.

**Why this phase exists:** the protocol is the only thing that can kill the project, and the
founding draft's version of it passed naive tests while being formally unsound. De-risk it first,
adversarially, or nothing after this matters.

## Phase 1 — Android v0.1 (weeks 3–8)

**Deliverable:** `dev.weft:android` snapshot on Maven Central — Steward, Weft, Heddle, Triad
kernel. One demo (W1 audio visualizer) with the side-by-side A/B/C/D toggle.

**Success criterion:** full A→B→C→D benchmark on three Android tiers (Realme C55 / Pixel 7a /
Galaxy S24) published with result bundles. First *measured* numbers replace the predictions in
[bench/README.md](bench/README.md) and the founding spec — wherever they land.

## Phase 2 — Web v0.1 (weeks 9–14)

**Deliverable:** `@weft/core` + `@weft/react` on npm. One-copy Transferable path by default, SAB
opt-in behind cross-origin-isolation detection, `WeftCanvas` worker loop. One demo (W5 order book).

**Success criterion:** A→B→C→D on Chrome / Firefox / Safari published. Safari rows report 60 Hz
and cite WebKit bug 173434 — the honesty rule applies to our own results table.

## Phase 3 — Benchmark launch (weeks 15–18)

**Deliverable:** the open benchmark site (`weft.dev/benchmarks`) — static, reproducible, forkable
— plus the launch posts (HN, r/androiddev, dev.to). Domain, npm org, and Maven groupId registered
*before* this phase, not after (name-squatters watch launches).

**Success criterion:** a stranger reproduces a published number on their own hardware and their
bundle renders on the site. That is the moment the moat is real.

## Phase 4 — Whitepaper (weeks 19–20)

**Deliverable:** the formal whitepaper — problem, thesis, Triad Protocol, measured benchmark
results, citations. PDF, ACM-style. The citable artifact for engineering directors.

**Success criterion:** every number in it is measured or explicitly labeled; every platform claim
carries its boundary. The founding spec's honesty culture, typeset.

## Phase 5 — SwiftUI (weeks 21–28)

**Deliverable:** Swift package — Canvas + CADisplayLink path (60 Hz) and MTKView path (120 Hz,
ProMotion), Steward probe selects at `bind()`. Same five workloads.

**Success criterion:** iOS benchmark numbers published on both paths.

## Phase 6 — Flutter (weeks 29–32) — *conditional*

**Deliverable:** pub.dev package — ffi `Pointer` + `CustomPainter` with the `repaint:` Listenable.

**Gate:** this phase starts only if a real customer asks. A platform port without a demand-side
maintainer is a liability (orphan-platform rule, [GOVERNANCE.md](GOVERNANCE.md)).

## Phase 7 — React Native (weeks 33–36) — *last*

**Deliverable:** `@weft/react-native` via Reanimated `SharedValue` + worklets.

**Why last:** Reanimated is the prior art Weft's RN story wraps — the weakest differentiator.
It ships after the moat is public, when it can ride established credibility.

## Phase 8 — Pro tier (week 37+)

**Deliverable:** leak-detection dashboard + off-heap crash analytics (the Steward surfaced as a
SaaS), first enterprise design partner. Separate repository, BSL, no code migration from this
repo (charter clause 4).

**Success criterion:** revenue. The consulting wedge ("14 FPS to 120 FPS in 6 weeks, or you don't
pay") funds open-source work throughout all phases and is not a roadmap line — it is the
background funding model.

---

## Explicitly not scheduled

- **Compose Multiplatform unification** — blocked on ARCHITECTURE.md Q3 (does iOS-Compose support
  identical `graphicsLayer { }` deferred reads?).
- **GPU-resident mode (`triad-2`)** — blocked on Q1 (shader-binding API shape). A tracked
  RFC; welcome earlier as a contribution.
- **VerifiedWeft (HMAC frames)** — Q4; v0.3 candidate.
- **Anything layout-shaped.** Token streams, text relayout, grid scrolling are non-goals
  ([docs/PHILOSOPHY.md §4](docs/PHILOSOPHY.md#4-what-weft-is-not)) and will remain so.

## The roadmap's one rule

**A phase without a green success criterion does not get followed by the next phase.** Slippage
is published. The founding draft's fantasy line (three platforms in eight weeks) happened because
the roadmap optimized for ambition over evidence; this one optimizes for the reverse, and the
benchmark gate is what makes the difference enforceable.
