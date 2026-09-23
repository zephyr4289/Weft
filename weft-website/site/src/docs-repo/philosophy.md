# Philosophy

*Read this before reading any code. If you disagree with this document, you disagree with the
project, and that is useful to know early.*

---

## 1. The kernel idea

> **Display state is a river, not a ledger.**

Reactive frameworks treat state as truth to be preserved: every value is kept, every change is
notified, every subscriber is made consistent. For cold state — forms, navigation, lists — that is
exactly right, and nothing in Weft competes with it. But a frame being drawn does not want
consistency, and it does not want history. It wants **now**. The pixel does not care what the PCM
buffer held three frames ago. A render loop that blocks to preserve ordering is committing a
category error: it is treating a river like a ledger.

From this one sentence, every technical decision in Weft derives:

- **No back-pressure** — there is nothing to back up. Old frames are not debt; they are garbage.
- **No locks** — blocking exists to preserve order and consistency. Display state needs neither.
- **Latest-wins, not first-in-first-out** — a Weft is the anti-queue. Where a queue preserves every
  item, a Weft preserves only the freshest. Dropping intermediate frames is not a compromise; it is
  the point.
- **Three buffers, not two** — the reader may hold a frame for as long as it likes without ever
  slowing the writer, because there is always a third buffer neither party owns.

Therefore: **latest-wins is not an optimization. It is the semantics of display.** Every feature
proposal must answer one question — *how does this serve now-ness?* — and a feature that turns the
Weft into a ledger (persistent queues, replay-on-read in the hot path) is a different library.

## 2. The Four Laws

These are the project's constitution. They are not aspirations; each one has a mechanical gate
(described below) that fails the build when violated. Philosophy that is not encoded in CI decays
the day the second maintainer arrives.

### Law 1 — The reader is always right; the writer is never blocked

The reader always has a complete, untorn frame to draw. The writer always completes a publish in
O(1) bounded steps, with no spin, no lock, no wait of any kind. A change that introduces blocking,
spinning, or back-pressure on either side is rejected *by definition* — it does not get a
performance review, because it is not a performance problem. It is a semantics violation.

**Gate:** the litmus suite ([litmus/](../litmus/)) — L2 (wait-free writer), L3 (wait-free reader),
L5 (no back-pressure) run on every PR that touches the kernel.

### Law 2 — Zero is a contract, not a goal

Zero allocations per frame. Zero locks on the hot path. Zero garbage scanned. Zero torn reads.
"Zero" is chosen over "low" because zero is binary, measurable, and assertable: a budget of "low"
drifts, an assertion of "zero" fails loudly. Every zero in this list has a test that fails the
build when it stops being true.

**Gate:** allocation-counter deltas (`Debug.getAllocCount` and platform equivalents) asserted at
zero per frame in benchmark CI; sequence-number continuity in litmus L1; the benchmark release
gate (P99 and bytes/frame regressions block releases).

### Law 3 — Mechanism, not policy

Weft owns the channel: the buffer, the protocol, the lifecycle. It has no opinions about what you
draw, how you thread, or which framework you use. The draw closure is yours; the theme, the scene
graph, and the layout belong to your framework. The moment Weft develops opinions about rendering
policy, it becomes a framework — and frameworks die by their own roadmap. This law is also the
anti-scope-creep clause: *Weft must never grow layout, text, state management, or UI primitives.*

**Gate:** a review checklist item on every PR ("does this add a dependency on a UI framework or
render policy? — reject"), enforced in `core/` and `steward/` code ownership rules.

### Law 4 — Honesty is a feature

Every capability claim ships with its boundary. Non-goals are a first-class document (section 5
below). Projections are labeled as projections until measured. Platform limits are documented with
citations — the Safari 60 Hz rAF cap carries a WebKit bug number, not a shrug. A library that
claims to fix everything fixes nothing, and a project whose README survives contact with a senior
reviewer earns the thing marketing cannot buy.

**Gate:** docs lint — unbounded superlatives flagged; every performance table must carry a
baseline and a measured/predicted label; every platform claim must carry a source.

## 3. The boundary of the claim

Stated precisely, because vagueness here would discredit everything else.

The platforms already provide draw-phase-deferred state reads — `Modifier.graphicsLayer { }` and
`drawWithContent` in Compose, `Canvas` in SwiftUI, `CustomPainter` in Flutter — documented best
practice that eliminates recomposition for hot state. A competent developer with a pooled heap
array already avoids the recomposition cascade. Weft therefore does **not** claim to beat best
practice on raw draw-bound FPS, and any text implying it does is a bug in the text.

Weft's actual delta — the dimensions best practice does not address:

| Dimension | Best practice | Weft |
|---|---|---|
| Buffer identity | Heap array; native writers must copy across the FFI boundary every write | Stable off-heap address; native writers write in place, zero-copy, forever |
| GC interaction | Large heap arrays are scanned by the collector | Off-heap buffers are invisible to GC |
| Cross-thread handoff | Left to the application; typically a mutex or a subtle race | Specified: the Triad Protocol — wait-free both sides, no torn reads, latest-wins |
| Lifecycle | Off-heap buffers leak silently across configuration change | The Steward binds lifetime to scope and detects leaks with stack traces |
| P99 frame time | Spikes under GC pressure (18–24 ms observed in comparable setups) | Flat by construction (~8.3 ms at 120 Hz) — *predicted; see bench* |

The thesis is falsifiable, and states its own failure condition: if, on the benchmark workloads,
on mid-range hardware, the weft plane does not deliver locked refresh with zero per-frame
allocation and zero GC pauses versus a fair best-practice baseline, the thesis is wrong and the
project should say so in public.

## 4. What Weft is not

- **Not a framework.** No layout, no text, no widgets, no scene graph, no state management for cold state.
- **Not a replacement for the reactive plane.** Cold state stays in `MutableState` / `@State` / `useState`. Weft is the second plane, not the first.
- **Not a fix for layout-bound hot state.** Token streams, text relayout, and grid scrolling are layout-invalidating; an off-heap buffer cannot help them. Weft addresses the draw-phase subset only.
- **Not zero-copy everywhere.** SAB requires COOP/COEP, which most consumer sites cannot ship. The default web path is one ~4 KB copy per frame via Transferables — honest, and fast enough.
- **Not 120 FPS everywhere.** Safari caps rAF at 60 Hz by default. Weft claims 60 on Safari, 120 on Chrome/Firefox with a 120 Hz display.
- **Not one cross-platform binary.** The primitives (`DirectByteBuffer` / `MTLBuffer` / `SharedArrayBuffer`) share no abstraction. One API, *n* implementations, one conformance suite.

## 5. The time-travel exception, stated once

Debuggers need history; displays need now. Recording Heddles (time-travel debug capture) exist,
live outside the hot path, and are opt-in at the Heddle layer — never in the kernel, never in a
Weft's steady state. The kernel remains a river. Recording is a faucet someone chooses to attach.

## 6. Why the weaving metaphor

Because the metaphor is load-bearing, not decorative. In weaving, the **warp** is the static
thread held under tension — the scaffold; the **weft** is the dynamic thread woven across it. A UI
is the same: the reactive tree is the warp, the continuous-state stream is the weft. A **heddle**
is the loom mechanism that lifts warp threads so the weft can pass — exactly what the binding
layer does for the Draw phase. A **steward** manages property on behalf of its owner — exactly
what the lifecycle manager does for buffers. One metaphor, four terms, five minutes to learn, and
a governed glossary ([docs/GLOSSARY.md](GLOSSARY.md)) that keeps future vocabulary from colliding
with the industry's.

The founding draft called the library Warp, the binding Loom, and the lifecycle manager Carder.
All three names collided with existing entities — an AI terminal's domain, the JVM virtual-threads
project, and credit-card-fraud slang, respectively. The collision test is now part of naming
governance. The lesson is recorded so the project does not need to learn it twice.
