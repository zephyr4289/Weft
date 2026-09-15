# Architecture

*The structural contract of the Weft project. Every module must be able to cite the section of
this document (or of an accepted RFC) that justifies its existence.*

---

## 1. The two planes

Weft introduces a second state plane alongside the reactive plane. The reactive plane is not
replaced; it is reserved for the state it is good at. The two planes coexist in every app that
uses Weft, and the loom that binds them is the Heddle.

```
┌───────────────────────────────────────────────────────────────┐
│  REACTIVE PLANE — cold state (< ~10 Hz)                       │
│  MutableState / @State / useState / setState                  │
│  Composition + Layout run when cold state changes.            │
└───────────────────────────────┬───────────────────────────────┘
                                │  Heddle binds the planes:
                                │  draw-phase read, nothing else
┌───────────────────────────────▼───────────────────────────────┐
│  WEFT PLANE — hot state (60–240 Hz)                           │
│  Off-heap buffers + Triad Protocol                            │
│  Single wait-free writer (native engine or worker),           │
│  wait-free readers on the Draw thread. Latest-wins.           │
└───────────────────────────────┬───────────────────────────────┘
                                │  draw-phase reads only
┌───────────────────────────────▼───────────────────────────────┐
│  HARDWARE RENDER LAYER                                        │
│  RenderNode / CAMetalLayer-MTLView / GPU canvas /             │
│  OffscreenCanvas — VSYNC-aligned                              │
└───────────────────────────────────────────────────────────────┘
```

End-to-end data flow, one frame's worth:

```
[Native engine / Worker]         the writer — data-driven
        │ publish(): write private buffer, swap into `latest`
        ▼
[Weft — 3 off-heap buffers]      ownership transfers by atomic exchange
        │ claim(): swap out `latest`, read in place
        ▼
[Heddle — Draw-phase binding]    the reader — VSYNC-driven
        │
        ▼
[GPU render layer]               the pixels — the only consumer that matters
```

The writer is data-driven; the reader is VSYNC-driven; the two never block each other. That
sentence is the whole architecture. Everything below is how it is made structural.

## 2. Kernel and drivers

The Linux lesson, applied strictly. The project has exactly one small, sacred, slow-moving thing —
**the Triad Protocol and its state machine** — and a large, fast-moving periphery of drivers that
implement it against four seams.

```
                  ┌─────────────────────────────────┐
                  │  TRIAD KERNEL — tiny, RFC-gated, │
                  │  litmus-tested, ~500 lines/lang  │
                  └───────────────┬─────────────────┘
      implements via four seams   │
  ┌──────────────┬────────────────┼────────────────┬──────────────┐
  ▼              ▼                ▼                ▼              ▼
android/       web/            swiftui/         flutter/          rn/
(driver)       (driver)        (driver)         (driver)      (driver, last)
```

**The canonical core is the conformance suite, not the code.** The kernel must exist per language
(the platform primitives share no abstraction), so the project's canonical artifact is
[`litmus/`](litmus/) — one language-agnostic suite of protocol conformance tests that every
implementation must pass to claim the name. "One protocol, many implementations, one conformance
suite." A contributor porting Weft to a new platform does not need to read the Kotlin; they need
to pass the litmus suite.

| Tier | Contents | Change process |
|---|---|---|
| Kernel | Triad state machine, frame envelope, ownership rules | RFC required; two kernel-maintainer approvals; litmus must pass |
| Steward | Lifecycle, leak detection | RFC for semantics; normal review for tooling |
| Heddles / demos / tools / bench | Everything else | Plain PR, one approval |

## 3. The four seams

A platform port = implement four traits + write one binding. That is the entire porting surface,
and it is deliberately small enough to hold in your head:

| Seam | Contract | Android | iOS | Web | Flutter | RN |
|---|---|---|---|---|---|---|
| `RawBuffer` | allocate / align / free; address stable for Weft lifetime | `ByteBuffer.allocateDirect` | `MTLBuffer` (shared) / `UnsafeMutablePointer` | `ArrayBuffer` (Transferable) / `SharedArrayBuffer` (opt-in) | `dart:ffi` `Pointer<Float>` via MallocAllocator | Reanimated-backed native buffer |
| `Atomic` | load / store / swap with explicit ordering | `AtomicI32`/VarHandle (`getAcquire`/`setRelease`, API 33+; CAS everywhere older) | swift-atomics (`AtomicInt`) | `Atomics` on SAB (seqcst — the web gives no weaker choice) | `package:atomic` or FFI to `std::atomic` | SharedValue (UI thread) + SAB atomics off-thread |
| `FrameClock` | vsync tick registration | `withFrameNanos` (Choreographer) | `CADisplayLink` (+`preferredFrameRateRange` for 120) | `requestAnimationFrame` (Worker side via `OffscreenCanvas` loop) | `RendererBinding` vsync / `TickerProvider` | `useFrameCallback` worklet |
| `DrawScope` | where the Heddle binds | `drawWithContent` / `graphicsLayer { }` | SwiftUI `Canvas` (60 Hz) / `MTKView` (120 Hz) | canvas in `OffscreenCanvas` worker | `CustomPainter` (+ `repaint:` Listenable) | worklet draw in `useFrameCallback` |

Honesty notes that travel with this table (see the founding spec §6 for full detail):

- **iOS is two paths, stated up front.** SwiftUI `Canvas` is Core-Graphics-backed and is the 60 Hz
  path; 120 Hz on ProMotion requires `MTKView` + `CADisplayLink.preferredFrameRateRange`. The
  Steward probes the device at `bind()` time; the dev writes one closure either way.
- **The web default is one copy per frame.** Transferable `ArrayBuffer` works everywhere.
  `SharedArrayBuffer` (true zero-copy) requires COOP/COEP and is opt-in for sites that can ship
  cross-origin isolation. Safari rAF is capped at 60 Hz by default (WebKit bug 173434).
- **React Native is the weakest differentiator.** Reanimated `SharedValue` is itself the prior
  art; Weft-RN is a disciplined wrapper, which is why RN ports last in the roadmap.

## 4. The frame envelope (frozen forever)

Every Weft buffer carries a permanent header. This is the project's deepest compatibility promise:
**the envelope never changes shape; it only gains versions.** Profilers, recorders, transports, and
future buffer media can therefore read any Weft ever created — the Apache Arrow lesson.

```
offset  0  : magic "WEFT"          (4 bytes)
offset  4  : proto version  u16    (1 = triad-1)
offset  6  : header size    u16
offset  8  : seq            u32    (writer sequence; telemetry + tear detection)
offset 12  : dtype          u8     (f32 / i16 / …)
offset 13  : dims           u8     (rank)
offset 14  : reserved       u16
offset 16  : shape[dims]    u32 × dims
offset 16+4·dims (padded to 16): payload, 16-byte aligned
```

Rules:

- Unknown trailing header fields must be ignored (forward compatibility).
- A semantic protocol change ships as `triad-2` alongside `triad-1`; capability negotiation happens
  at `Steward.bind()` time, TLS-style. Breaking the envelope requires a new magic — which is to
  say, it is not done.
- `seq` is written by the publisher and is the backbone of litmus tear detection.

## 5. The Steward and the lifecycle state machine

```
   ALLOCATED ──bind(scope)──▶ BOUND ──attachWriter()──▶ WRITING
       │                        │                          │
       │                        │                    scope dispose /
       │                   (no writer:                release()
       │                   reader sees zeros)             │
       ▼                        ▼                          ▼
   (leak watch)             (leak watch)              RELEASED  ← terminal
       │                        │                          │
       └────────────────────────┴──── survived scope ────▶ LEAK (debug: stack trace)
```

- **Single writer** — `attachWriter` panics in debug if a writer is already attached.
- **No read/write after release** — panics in debug; reads return zeros, writes no-op in release.
- **Scope-bound lifetime** — composition/`View`/`dispose` exit frees the buffer; a buffer that
  survives its scope is logged as `LEAK` with an allocation stack trace (the LeakCanary model).
- **Configuration change** — the Steward is ViewModel/`@StateObject`/root-scoped, *not*
  composition-scoped: the buffer survives recreation; the composition borrows it. (`rememberWeft`
  was rejected for exactly this reason — see founding spec §7.3.)
- **Writer revocation (invariant I6)** — a released Weft's writer token is revoked *before* the
  buffer is freed, and the writer checks the token once per publish (one relaxed load). Freeing is
  deferred until the writer is quiescent. This is the use-after-free guard across the FFI boundary,
  where no panic can save you. Full contract in [rfcs/0001](rfcs/0001-triad-exchange-protocol.md).

## 6. Stability tiers and versioning

| Tier | Surface | Promise |
|---|---|---|
| **Tier 0 — frozen** | Frame envelope, protocol semantics | Never changes shape. Semantics change = new protocol version coexisting via negotiation. |
| **Tier 1 — semver** | Kernel and Steward public APIs | RFC + two kernel approvals; semver respected religiously. |
| **Tier 2 — fast lane** | Heddles, demos, tools, bench | One approval, move fast, break nothing above them. |

The versioning philosophy in one line: *the wire format is HTTP, the APIs are semver, the demos
are disposable.*

## 7. Upgrade axes (the future has a home)

Every known future feature already has a directory and a seam. New capabilities plug in; they do
not force redesigns.

1. **New media** — GPU-resident Wefts (`AHardwareBuffer` / `MTLBuffer` / WebGPU `GPUBuffer`):
   new `RawBuffer` implementations + a `triad-2` protocol version. The kernel math is unchanged.
2. **New readers** — fan-out Heddles (multi-consumer), recording Heddles (debug capture), network
   Heddles (co-visualization): all live in `heddles/`, layered on the same claim protocol.
3. **New writers** — compute shaders, AudioWorklets, sensor daemons: the WriterToken contract (I6)
   already defines their seam.
4. **New platforms** — the four seams plus a passing litmus run is the whole porting checklist.
5. **New workloads** — `bench/` is data-driven; a workload is a config plus a draw routine, so the
   benchmark site grows without touching the kernel.

## 8. Open questions (tracked, not hidden)

These are the design questions the project admits it has not answered. Each will become an RFC;
none block Phase 0–1.

- **Q1 — GPU-resident API.** When the writer is a compute shader and the reader a render shader,
  what does the Heddle become? (Shader binding generation vs. manual AGSL/Metal/WGSL.)
- **Q2 — Fan-out snapshot policy.** Per-reader snapshot buffers cost an allocation (violates Law 2
  as stated); a shared snapshot with per-reader epochs costs complexity. Undecided.
- **Q3 — Compose Multiplatform.** Does iOS Compose support `graphicsLayer { }` deferred reads
  identically? If not, iOS-Compose Heddles fall back to MTKView — same closure or two?
- **Q4 — Authenticated Wefts.** Network-sourced hot state inherits the tick-spoofing risk. A
  `VerifiedWeft` (HMAC per frame, ~µs cost) is plausible at v0.3.
- **Q5 — Process-death policy.** Re-allocate vs. re-hydrate on Android process recreation should
  be an explicit Steward policy (`ReattachPolicy`), not an afterthought.
