# Roadmap

Sequential, benchmark-gated, and honest about what is conditional. The founding draft proposed
three platforms in eight weeks; this roadmap replaces that with the corrected plan. Phases do not
overlap, and each phase's success criterion is mechanical — a phase ends when its criterion is
met, not when its calendar slot does.

**Current status: Phase 0 — not started.** The next physical action on this project is the Triad
Protocol spike.

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
