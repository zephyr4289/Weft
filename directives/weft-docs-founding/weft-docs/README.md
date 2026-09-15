# Weft

**The continuous-state plane for declarative UI.**

> Reactive UI frameworks are correct for **cold state** (forms, lists, dialogs, navigation) and
> catastrophic for **hot state** (audio buffers, particle fields, sensor telemetry, order books,
> dense numeric streams). Weft is the second state plane: an off-heap, zero-copy,
> draw-phase-read channel with explicit lifecycle semantics, exposed through a consistent
> cross-platform API, and validated by a reproducible benchmark suite.

**Display state is a river, not a ledger.** The frame being drawn does not want history or
consistency — it wants *now*. Weft is a channel for now. Read [docs/PHILOSOPHY.md](docs/PHILOSOPHY.md)
before you read a single line of code.

---

| | |
|---|---|
| **Status** | Pre-alpha — specification phase. The Triad Protocol is specified in [rfcs/0001](rfcs/0001-triad-exchange-protocol.md); no reference implementation has merged yet. |
| **License** | Apache-2.0 (everything in this repo). The future Weft Pro tier is a separate, closed product. |
| **Founding document** | *Weft Specification v0.1* (September 2026) — the PDF this repository supersedes with living docs. |
| **Author** | Zephyr ([@zephyr4289](https://github.com/zephyr4289)) |
| **First artifact** | [`litmus/`](litmus/) — the protocol conformance suite. It is publishable before v0.1 and it is the project's canonical core. |

---

## Why Weft exists

Route hot state through a reactive framework's state system and four failure modes stack up:

1. **Recomposition cascade** — every state tick invalidates, re-composes, and re-lays-out the
   subtree. A 1024-bar visualizer reading reactive state at 120 Hz collapses to 11–14 FPS on a
   Pixel 7a. This is inherent to the reactive contract, not a framework bug.
2. **GC allocation storms** — per-frame wrappers and array copies become young-gen garbage at
   exactly the rate the UI thread can least afford (~480 KB/s for one 1024-float stream at 120 Hz).
3. **FFI marshalling** — native engines cannot hold a pinned JVM/Swift array across calls, so every
   native write copies across the boundary.
4. **Main-thread blocking (Web)** — big data on the main thread starves the event loop.

**The honest boundary of the claim:** platforms already provide draw-phase-deferred reads
(`graphicsLayer { }`, `Canvas`, `CustomPainter`) that eliminate recomposition. Weft's delta is
*not* "beats naive reactive." It is what best practice does not address:

- **Buffer identity** — a stable off-heap address native engines can write, zero-copy, forever.
- **GC scan avoidance** — large buffers that the collector never touches.
- **A specified cross-thread write protocol** — the Triad Protocol ([rfcs/0001](rfcs/0001-triad-exchange-protocol.md)): wait-free on both sides, no torn reads, latest-wins.
- **Lifecycle** — the Steward binds buffer lifetime to scope and detects leaks. LeakCanary for off-heap.

On raw draw-bound FPS, Weft roughly ties best practice. On P99 frame time, GC pauses, and
native-writer safety, it is a different animal. Marketers will read the FPS column. Read the P99 column.

## The Four Laws

Every contribution, every RFC, every release is measured against these. They are enforced
mechanically where possible — see [docs/PHILOSOPHY.md](docs/PHILOSOPHY.md) for the rationale of each.

1. **The reader is always right; the writer is never blocked.** No waits, no spins, no back-pressure. Rejected by definition, not by performance review.
2. **Zero is a contract.** 0 alloc/frame, 0 locks on the hot path, 0 GC scans, 0 torn reads. Each zero has a test that fails the build.
3. **Mechanism, not policy.** Weft owns the channel — buffer, protocol, lifecycle. It has no opinions about what you draw.
4. **Honesty is a feature.** Every claim ships with its boundary. Non-goals are a first-class document.

## What it looks like

Android / Jetpack Compose (Phase 1 target API — subject to the RFC process):

```kotlin
// The Steward is ViewModel-scoped: the buffer survives configuration change.
class AudioVm : ViewModel() {
    val steward = Steward()
    val pcm = steward.weft(capacity = 1024)     // off-heap, 16-byte aligned
    init { NativeBridge.attachAudioTap(pcm) }   // native writer, wait-free publish
    override fun onCleared() = steward.releaseAll()
}

@Composable
fun AudioBars(vm: AudioVm) {
    Canvas(Modifier.fillMaxSize().weftDraw(vm.pcm)) {   // the Heddle: draw-phase binding
        vm.pcm.read { buf ->                            // claim-and-read, wait-free
            for (i in 0 until buf.capacity()) drawBar(i, buf[i])
        }
    }
}
```

Web (Phase 2), same shape:

```typescript
const pcm = steward.weft<Float32Array>(1024);
return <WeftCanvas weft={pcm} draw={(ctx, buf) => {
    for (let i = 0; i < buf.length; i++) drawBar(ctx, i, buf[i]);
}} />;
```

One mental model — Weft, Heddle, Steward, Triad — across every platform. See
[ARCHITECTURE.md](ARCHITECTURE.md) for why this is one API and *n* implementations rather than
one implementation, and [docs/GLOSSARY.md](docs/GLOSSARY.md) for the vocabulary.

## The four terms

| Term | Role |
|---|---|
| **Weft** | The library. The continuous-state channel and its off-heap buffers. |
| **Heddle** | The binding layer. Reads a Weft during the Draw phase only. |
| **Steward** | The lifecycle manager. Allocates, binds, frees, leak-detects. |
| **Triad Protocol** | The writer–reader synchronization protocol. Three buffers, one atomic, ownership by exchange. |

A senior engineer learns the whole model in five minutes: a **Steward** manages the lifetime of a
**Weft**; a **Heddle** binds it to the Draw phase; the **Triad Protocol** keeps writer and reader
from ever blocking or tearing.

## Platform status

Sequential, honest, benchmark-gated. Details in [ROADMAP.md](ROADMAP.md).

| Platform | Phase | Status | Draw path |
|---|---|---|---|
| Android (Jetpack Compose) | 1 | Planned — after Phase 0 spike | `drawWithContent` / `graphicsLayer`, `withFrameNanos` |
| Web (React et al.) | 2 | Planned | OffscreenCanvas in a Worker; rAF; Transferables default, SAB opt-in |
| Benchmark site | 3 | Planned | `weft.dev/benchmarks` — open, reproducible, forkable |
| Whitepaper | 4 | Planned | ACM-style; cites real numbers, not projections |
| SwiftUI | 5 | Planned | Canvas + CADisplayLink at 60 Hz; MTKView at 120 Hz on ProMotion |
| Flutter | 6 | Conditional — only with a customer | `CustomPainter` + ffi `Pointer` |
| React Native | 7 | Last — weakest differentiator | Reanimated `SharedValue` + worklets |

Claims the project will not make: 120 FPS on Safari (WebKit caps rAF at 60 Hz by default —
[WebKit bug 173434](https://bugs.webkit.org/show_bug.cgi?id=173434)); zero-copy on every browser
(SAB needs COOP/COEP; most consumer sites cannot ship it); one-KMP-module magic (the primitives
share no abstraction — the API is consistent, the implementations are expect/actual).

## The benchmark suite is the moat

The library can be absorbed by a platform. The benchmark site cannot. Without numbers this is a
blog post; with numbers it is the standard. Five workloads, six devices, four implementations
(A naive reactive / B platform best practice / C Weft / D hand-rolled), P99-first metrics,
zero-alloc assertions, 30-minute thermal sustained runs. Full methodology and the fairness pin in
[bench/README.md](bench/README.md).

| Impl | Role | Predicted P99 (Pixel 7a, W1) |
|---|---|---|
| A — Reactive naive | The failure mode | ~6 FPS |
| B — Best practice | The real competitor | ~54 FPS |
| C — Weft | The candidate | 120 FPS, 0 B/frame |
| D — Hand-rolled | The ceiling | 120 FPS, 0 B/frame |

*Predicted, not measured. The founding spec was careful to label projections as projections, and
this README continues that discipline. First measured numbers land with Phase 1.*

## Repository map

```
weft/
├── README.md            ← you are here
├── ARCHITECTURE.md      two planes, four seams, frame envelope, stability tiers
├── ROADMAP.md           phases 0–8, success criteria, current status
├── CONTRIBUTING.md      the contribution ladder, RFC process, PR checklist
├── GOVERNANCE.md        roles, decisions, code ownership
├── CODE_OF_CONDUCT.md   adopted from Contributor Covenant 2.1
├── SECURITY.md          reporting, threat model, supported versions
├── docs/
│   ├── PHILOSOPHY.md    the river-not-ledger kernel, the Four Laws, non-goals
│   └── GLOSSARY.md      the weaving vocabulary — governed, with banned names
├── rfcs/
│   ├── TEMPLATE.md      proposal format
│   └── 0001-triad-exchange-protocol.md   the protocol, corrected and specified
├── litmus/              ★ the canonical conformance suite (L-series tests)
├── bench/               ★ the benchmark suite — the moat
├── core/                kernel implementations (kotlin/ swift/ ts/) — RFC-gated
├── steward/             lifecycle + leak detection, per language
├── heddles/             per-framework bindings — the fast lane
├── demos/               one per workload, side-by-side A/B/C/D toggles
└── tools/               weft-probe, weft-record
```

`core/`, `steward/`, `heddles/`, `demos/`, and `tools/` populate as phases land. The directories
exist now because the structure telegraphs the architecture: new code has an obvious home, and
the four seams bound every driver's blast radius.

## Contributing

Read [docs/PHILOSOPHY.md](docs/PHILOSOPHY.md) first, then
[CONTRIBUTING.md](CONTRIBUTING.md). The short version: docs and demos need no review pedigree;
platform ports must pass the litmus suite; the kernel requires an accepted RFC and a shipped port
first. DCO sign-off, no CLA. Lazy consensus on RFCs with a 7-day window.

## Governance

Maintainers per platform directory, two for the kernel, decisions by lazy consensus.
[GOVERNANCE.md](GOVERNANCE.md) is one page on purpose. The charter's permanent clauses: the Four
Laws, and the benchmark site never goes closed.

## Prior art and credits

Weft is a disciplined packaging of ideas that already exist, and it says so:

- **Jetpack Compose** — draw-phase-deferred state reads (the official best practice Weft builds on).
- **Reanimated** — `SharedValue` + worklets: the prior art Weft's RN story wraps.
- **LeakCanary** — the model for the Steward's leak detection.
- **Apache Arrow** — the model for the frozen frame envelope.
- **Graphics triple buffering** — the ownership-exchange pattern the Triad Protocol formalizes.
- **WebKit bug 173434** — documented, not hidden: the Safari 60 Hz rAF cap.

## License

Apache-2.0. Everything in this repository, forever. The future Weft Pro tier (leak-detection
dashboard, crash analytics) is a separate closed product under BSL and lives in a separate
repository. Open core means the open part genuinely stays open.

---

## STATUS BLOCK (Phase 3, 2026-09-11)

> **Canonical reference:** `docs/WHITEPAPER.md` v1.0 supersedes the founding spec PDF.
> For errata to the founding spec, see `docs/ERRATA.md`.
>
> **RFC-0001:** Accepted (evidence triad: loom + TSAN + litmus 24/24).
> **Phase 0:** CLOSED. **Phase 0.5:** CLOSED. **Phase 1:** CLOSED. **Phase 3 (this):** Whitepaper delivered.
> **Roadmap deviation:** Phase 2 ↔ Phase 3 swap (WO-P1-CLOSURE §7.1) — whitepaper executes before tools.
> **All numbers:** `x86_64-sandbox` (runtime-verified). Phase 6+ replaces with real-device numbers.
