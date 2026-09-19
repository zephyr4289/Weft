# Weft Technical Whitepaper — Volume I

## Core Foundations, Concurrency Invariants, Kernel APIs & Formal Proofs

> **Volume I of III** · 2026-09-18 · Compiled from the merged mainline at commit `6a9a4db`
> (PRs #1–#14: native fan-out parity, VM ports & flight recorder, VerifiedWeft, nanoseconds
> telemetry work order, trust & time travel, runs-everywhere-proven, chaos-torture & formal
> proofs, cadence/0-GC lifecycle, hardware acceleration).
>
> **Status:** Volume I documents what **IS** — the frozen kernel, the driver-layer protocol
> surfaces, and the verification evidence committed at the cited revision. The kernel is
> frozen for the duration of this document (Tier 0/Tier 1 per `ARCHITECTURE.md` §6).
>
> **Honesty labels (Law 4):** every measured number in this volume carries one of:
> - `[MEASURED x86_64-sandbox sha256:16b5c663]` — committed bench evidence,
> - `[COMMITTED EVIDENCE: <path>]` — a log in `litmus/evidence/` at the cited revision,
> - `[RE-VERIFIED 2026-09-17T17:18Z @ 6a9a4db]` — re-executed for this volume at the
>   documented commit with the pinned toolchain (Appendix A records the exact commands).
>
> **Volume scope:** §1–§6 per the volume assignment — problem statement, the three-language
> kernel and its freeze contract, the invariant system (I1–I8) and memory model, the complete
> kernel and RFC-0004 protocol API reference, formal verification (TLA+, Loom), and the
> litmus conformance suite. Hardware acceleration (SIMD batch verification, inter-process
> shared-memory ring sessions, GPU-resident rings — PR #14) and the higher presentation
> layers (governor, cadence policies, recyclers, bindings) are Volume II/III material; they
> appear here only where they constrain core semantics, and Appendix C maps every merged
> contribution to its volume.

---

## Table of Contents

- [§0 Reading Guide & Notation](#0-reading-guide--notation)
- [§1 Executive Summary & Problem Statement](#1-executive-summary--problem-statement)
- [§2 The 3-Language Kernel & the Kernel Freeze Contract](#2-the-3-language-kernel--the-kernel-freeze-contract)
- [§3 Concurrency Invariants & Memory Model (I1–I8)](#3-concurrency-invariants--memory-model-i1i8)
- [§4 Complete Core Kernel & Protocol API Reference](#4-complete-core-kernel--protocol-api-reference)
- [§5 Formal Verification & Mathematical Proofs](#5-formal-verification--mathematical-proofs)
- [§6 Litmus Conformance Suite (L1–L8)](#6-litmus-conformance-suite-l1l8)
- [Appendix A — Reproducibility](#appendix-a--reproducibility)
- [Appendix B — Artifact Manifest](#appendix-b--artifact-manifest)
- [Appendix C — Merged Contribution Coverage](#appendix-c--merged-contribution-coverage)
- [Appendix D — Glossary & References](#appendix-d--glossary--references)

---

## §0 Reading Guide & Notation

**Languages.** "The kernel" exists in three canonical implementations — C11
(`core/c/weft.{h,c}`), Rust (`core/rust/src/lib.rs`), and TypeScript (`core/ts/weft.ts`) —
plus three VM ports (Kotlin, Swift, Dart) that follow the same conformance suite. Where this
volume shows a single signature, the C header is normative; the Rust and TypeScript mirrors
are shown where their shapes differ. A port is "Weft" because it passes the suite, not
because of its lineage — this is the project's central governance decision and it determines
how every API table below should be read.

**Invariant numbering.** The repository's historical identifiers (kernel I1–I6, fan-out
FI1–FI3, the Four Laws, AXIOM T) are unified in this volume into the eight-invariant taxonomy
**I1–I8** requested for the whitepaper. §3 presents each invariant with both its Volume I
name and its repository identifier(s), so every claim in this document can be traced to a
source file, a test, or a proof obligation without translation loss.

**Pseudocode convention.** Protocol steps are shown in the repository's own pseudo-protocol
style (`02-KERNEL.md`): `atomic.op(ordering)` annotations are normative, comments cite the
governing spec section. Code blocks lifted from the tree are verbatim and cite their path;
any elision is marked `…`.

**Mathematics.** Formulas use inline notation (`sum(dropped) == lastSeq - freshClaims`);
TLA+ fragments are quoted verbatim from `formal/` with the module name. Probability
estimates quoted from TLC are its own fingerprint-collision statistics — they bound the
chance that two distinct states collided into one fingerprint and were therefore *missed*,
not the chance that the checker is wrong about an explored state.

**What "proven" means here.** This volume is deliberate about the epistemic ladder its
evidence occupies: *exhaustive-formal* (TLC over a finite state space — every schedule the
model can express), *bounded-exhaustive* (Loom over every interleaving under a preemption
bound), *adversarial-empirical* (torture, TSAN/ASAN, the deterministic chaos engine at
10⁷+ nightly iterations), and *conformance* (the litmus catalog). Each tier's blind spot is
named where the tier is cited (§5.1). A claim is never stronger than the weakest tier that
carries it.

---

## §1 Executive Summary & Problem Statement

### 1.1 The problem: high-frequency streams in multi-language environments

Weft exists because of a mismatch between two clocks. Modern declarative UI frameworks —
Jetpack Compose, SwiftUI, Flutter, React — run a reactive contract, `UI = f(state)`, that is
correct for **cold state**: form inputs, navigation, dialogs, settings toggles changing at
human speed (below ~10 Hz). But a class of application state is **hot**: audio PCM
visualizers, 6-DOF physics, streaming token outputs, high-density order books, sensor
telemetry, dense numeric grids — data that changes at 60–240 Hz or faster. When hot state is
routed through machinery designed for cold state, the machinery collapses in four
characteristic and measurable ways.

**GC pause jitter.** Each reactive update instantiates temporary wrapper objects, snapshot
holders, and layout strings. A 1024-float `FloatArray` reallocated per frame at 120 Hz
produces roughly 480 KB/s of young-generation garbage; on a mid-range Android device this
manifests as 40–80 micro-stutters per second, each 5–15 ms, each dropping a frame
(`docs/WHITEPAPER.md` §1.2). The collector is doing its job — the architecture is forcing
allocation where none is needed. Even in managed runtimes without visible pauses, per-frame
allocation puts the render path in contention with the mutator for the same cache hierarchy
and the same allocator locks.

**Render-thread starvation.** In Compose, reading a `MutableState<Float>` inside a
`@Composable` body registers a dependency edge; at 120 Hz the framework invalidates the
scope, re-executes composition, and re-runs layout across the subtree. A 1024-bar visualizer
that needs an 8.3 ms frame budget spends 40–60 ms in composition + layout and collapses to
11–14 FPS (`docs/WHITEPAPER.md` §1.1). The same failure shape repeats in SwiftUI `@State`,
Flutter `setState`, and React re-renders: every read creates an edge, every invalidation
walks the graph. The render thread starves because the framework does bookkeeping the pixel
never asked for.

**Torn frames.** The naive fix — one shared buffer, writer and reader both unsynchronized —
tears. A 60 Hz writer filling 4 KB while the draw thread reads produces half-old/half-new
frames. The founding spec's first protocol sketch had a subtler version of this defect: a
two-variable design (`latest` + `claimed`, both loaded relaxed) with a formally
unrepresentable visibility window in which the writer could legally compute the *reader's
held buffer* as its next write target (RFC-0001 §3). Real hardware's cache coherence shrinks
the window to nanoseconds, so naive benchmarks pass — until a thermal throttle, a debugger
attach, or a scheduler hiccup stretches a hold past a propagation delay. **A protocol whose
safety depends on propagation latency is not a protocol; it is a timing bet** — and that
sentence became project policy.

**Lock contention.** The textbook fix for tearing — a mutex — blocks, and blocking is
prioritized damage. An audio-thread (`SCHED_FIFO`) writer blocked on a UI thread mid-draw is
priority inversion with audible clicks; a render thread blocked on a producer is a dropped
frame by construction (RFC-0001 §1). Back-pressure variants (double buffering with
semaphores, bounded queues) merely move the stall: the writer's rate becomes coupled to the
reader's draw time.

**The multi-language multiplier.** Each failure mode compounds across the six runtimes a
real product ships on — C++/Rust native engines, Android's ART, iOS's ARC/Swift, the JVM,
Flutter's Dart isolates, and the browser's worker threads. The platform primitives share no
abstraction (`DirectByteBuffer` / `MTLBuffer` / `SharedArrayBuffer` / `Pointer<Float>`), so
every cross-boundary handoff is either a copy, a pin, or a protocol the framework invents
ad hoc — usually wrong in a new way each time. Weft's answer to this multiplier is the
litmus suite as the canonical artifact (§2.1): one protocol, *n* implementations, one
conformance suite, and a byte-exact wire format (§4.3) so that a frame produced by any
language is consumable by any other.

### 1.2 The core thesis

**Display state is a river, not a ledger.** A frame being drawn does not want consistency
and does not want history; it wants *now*. From that one sentence, the entire architecture
derives (`docs/PHILOSOPHY.md` §1): no back-pressure (old frames are garbage, not debt), no
locks (blocking exists to preserve order, and display needs neither), latest-wins rather
than FIFO (dropping intermediate frames is the point, not a compromise), and **three
buffers, not two** — the reader may hold a frame arbitrarily long while the writer always
has a third buffer no party owns.

The thesis, stated as an architecture: **a zero-copy, lock-free, allocation-free,
single-writer multi-reader ring-buffer plane, specified once and implemented per language,
in which the writer hands frames to readers by atomic ownership exchange rather than by
queueing, copying, or blocking.** Two mechanisms carry it:

1. **The Triad kernel** (RFC-0001): one writer, one reader, three off-heap buffers, and a
   single shared atomic `latest`. Publish and claim are each *one* atomic exchange —
   wait-free both sides, tearing impossible by ownership rather than by timing (§4.1).
2. **The fan-out ring** (RFC-0004): a driver-layer seqlock ring that composes *beside* the
   frozen kernel to serve N independent readers (primary canvas, minimap, flight recorder,
   network visualizer) from one writer, with per-reader drop accounting that telescopes
   exactly (§4.2).

The Four Laws are the constitution that makes this thesis enforceable rather than
aspirational (`docs/PHILOSOPHY.md` §2): **Law 1** — the reader is always right, the writer
is never blocked (bounded steps, no spin, no wait; gated by litmus L2/L3/L5); **Law 2** —
zero is a contract, not a goal (0 alloc/frame, 0 locks, 0 GC scans, 0 torn reads — each
zero has a test that fails the build); **Law 3** — mechanism, not policy (Weft owns the
channel, never the rendering opinions); **Law 4** — honesty is a feature (every claim ships
with its boundary; this document's honesty labels are that law in operation).

### 1.3 What Volume I establishes

The volume's six chapters form one argument with three load-bearing results.

**Correctness by construction (§2–§4).** The kernel's safety does not rest on timing,
scheduler behavior, or cache state: ownership transfers by a single atomic exchange per
side, so the set of owners is a permutation of {writer, reader, exchange-slot} at every
instant (Theorem T1, §5.5). The wire format is frozen at 16 bytes with a self-describing
`header_size` growth mechanism, and the three canonical implementations are held to
bit-identical arithmetic by cross-language fixtures and the kernel-freeze diff gate (§2.3).

**Correctness by exhaustion (§5).** Two TLA+ models are checked by TLC with a pinned
prover: `FanoutSeqlock` — the multi-reader ring with the tear window *real* in the model
(reader copies one payload word per step; the writer may invalidate and refill the same
slot between words) — and `ReattachPolicy` — process death under both RFC-0006 policies.
Re-executed for this volume at the cited commit: both report *"Model checking completed. No
error has been found."* — `FanoutSeqlock` exploring a complete state space of 19,443 total
distinct states (6,481 distinct states in the invariant search, diameter 46) with
fingerprint-collision probability 4.6×10⁻¹², and `ReattachPolicy` a complete space of 2,460
total distinct states (615 in the invariant search, diameter 17) at 4.8×10⁻¹⁴
`[RE-VERIFIED 2026-09-17T17:18Z @ 6a9a4db]`. Loom's bounded-exhaustive sweeps cover the
memory-model level the TLA+ abstraction does not address: the kernel model explores every
interleaving of 3 publishes × 3 claims × 3 buffers, and the fan-out model — at preemption
bound 2 (and 3 in the nightly tier) — finds no torn claim, no future violation, and exact
telescoping in every reachable execution `[COMMITTED EVIDENCE: litmus/evidence/loom/,
litmus/evidence/fanout/rust-suite.log]`.

**Correctness under adversarial sampling (§6).** The litmus suite runs 8 protocol tests ×
3 canonical languages = 24 cells, debug and release, ≥30 s wall time each, repeated 5× —
all green `[COMMITTED EVIDENCE: litmus/REPORT.md]`. TSAN executes 5 runs × 8 tests = 40
cleanings with zero reports; the fan-out torture gate pushes 1M frames × 4 readers with
every fresh claim word-validated and the telescoping identity exact in both ordering
regimes; cross-language interop is bit-exact in both directions. The deterministic chaos
engine (10⁷+ iterations nightly, byte-identical verdicts across C/TS/JVM/Dart) and the
guardian watchdog complete the empirical tier — each tier's limits are stated where it is
cited, because a claim is never stronger than the weakest tier that carries it.

The falsifiability commitment is inherited from the founding thesis verbatim: if, on the
benchmark workloads, on mid-range hardware, the Weft plane does not deliver locked refresh
with zero per-frame allocation and zero GC pauses versus a fair best-practice baseline, the
thesis is wrong and the project should say so in public.

---

## §2 The 3-Language Kernel & the Kernel Freeze Contract

### 2.1 The canonical core is the conformance suite, not the code

The project's central governance decision predates every line quoted in this volume:
because the platform primitives share no abstraction, the kernel *must* exist per language,
and therefore what makes an implementation "Weft" cannot be its lineage. It is the litmus
catalog (`litmus/README.md` §2): "One protocol, many implementations, one conformance
suite." A Swift expert contributes a port without reading Kotlin; a protocol change
re-validates every implementation through the same gate, making cross-community drift
mechanically impossible; and the suite exists because the founding draft passed happy-path
tests while being formally unsound — happy-path tests certify luck, litmus tests certify
the property.

The Linux lesson is applied structurally (`ARCHITECTURE.md` §2): exactly one small, sacred,
slow-moving thing — the Triad state machine and its envelope — surrounded by a fast-moving
periphery of driver layers. The change process is tiered: kernel semantics require an RFC
and two kernel-maintainer approvals; the Steward needs an RFC for semantics; Heddles,
demos, tools, and bench move by plain PR. Everything cited in this volume sits in the first
two tiers or is explicitly a sibling driver module.

### 2.2 The bit-exact state: what "the kernel" is, byte for byte

One `Weft` instance is: three off-heap buffers, one shared atomic index, two thread-private
indices, the I6 revocation pair, and five telemetry counters. The C declaration is
normative (`core/c/weft.h` §Public types; field comments cite `02-KERNEL.md` §1):

```c
typedef struct weft {
    uint8_t* buf[3];        // 3 buffers, 64-byte aligned
    size_t buf_size;        // = align64(16 + payload_max + 8, 64)
    size_t payload_max;     // immutable after init

    _Atomic uint32_t latest;   // THE shared atomic; exchanged by publish & claim
    uint32_t w_work;           // WRITER-PRIVATE working index (init 1)
    uint32_t r_work;           // READER-PRIVATE held index (init 2)

    _Atomic bool revoked;      // I6: revocation flag (init false)
    _Atomic uint32_t epoch;    // I6: ACK counter (init 0)

    _Atomic uint64_t t_publish, t_claim, t_drop;      // Relaxed telemetry
    _Atomic uint64_t t_wsteps, t_rsteps;             // L2/L3 step counters
} weft_t;
```

**Per-buffer layout** — identical in all three languages, the deepest compatibility promise
of the project (`core/c/weft.c` §Buffer layout helpers; `03-ENVELOPE.md` §1):

```text
byte 0                    16                         16+payload_max   buf_size-8   buf_size
┌────────────────────────┬────────────────────────────┬───────────────┬────────────┬──────────┐
│      ENVELOPE (16B)    │        PAYLOAD             │   pad to      │  CANARY    │  (pad)   │
│ magic·ver·hsize·seq·len│     payload_max bytes      │  64-boundary  │  u64 = seq │          │
└────────────────────────┴────────────────────────────┴───────────────┴────────────┴──────────┘
   all fields little-endian, no exceptions, all languages, forever
```

- `buf_size = align64(16 + payload_max + 8, 64)` — envelope + payload + 8-byte canary,
  rounded **up** to the 64-byte cache-line boundary; the rounding bytes are the pad words.
- **Canary**: a `u64` little-endian copy of `seq` at `buf_size − 8`, written by `publish`
  *after* the envelope and *before* the exchange. It is L1/L6's tear detector: a claimed
  buffer whose canary ≠ envelope `seq` has been observed mid-write.
- **Cache-line alignment**: each buffer is allocated 64-byte-aligned (`posix_memalign(64, …)`
  in C; `Layout::from_size_align(buf_size, 64)` in Rust). This guarantees the canary never
  shares a cache line with another buffer's envelope and that the envelope's `seq` word is
  not torn *by the platform* even where the ISA would permit it — an alignment-based
  defense that sits *under* the protocol's ordering-based defense (I8, §3.1).

**The TS substrate** places the same state inside one `SharedArrayBuffer` with a 64-byte
control block followed by the three buffers (`core/ts/weft.ts` §Weft):

```text
SharedArrayBuffer
┌──────────────────────────── bytes 0..63 ────────────────────────────┐
│ ctrl : Int32Array ×16 (also BigInt64Array ×8 over the same bytes)  │
│   slot 0  latest      slot 1  w_work     slot 2  r_work            │
│   slot 3  revoked     slot 4  epoch                              (i32)│
│   slots 10-15  t_publish / t_claim / t_drop as dual-i32 halves  (u64)│
│   (slots 5-7 of the BigInt64 view read the same 8-byte pairs)       │
├──────────────────────────── bytes 64.. ─────────────────────────────┤
│ buf[0] (buf_size) │ buf[1] (buf_size) │ buf[2] (buf_size)          │
└─────────────────────────────────────────────────────────────────────┘
```

The dual-i32 telemetry regime is a measured, evidence-closed gap rather than an aesthetic
choice: the original BigInt counters boxed ~90 B per publish (the `1n` literal, the
`Atomics.add` return value, and the step-counter sum each a fresh heap BigInt — measured by
the W6 feed bench and closed by this change), so the hot path bumps `Atomics.add` on the
Int32 halves with carry into the high half once per 2³² increments, while the cold getters
compose the exact `u64` through the BigInt64 view. The stated divergence is bounded by
AXIOM T: the two-step increment is not atomic-as-u64 — a racing cold reader can observe a
transient `(new lo, old hi)` window once per 2³² increments; settled reads are exact
(`core/ts/weft.ts` §TELEMETRY COUNTER REGIME; `docs/PORTS.md` §4).

**Sequence word mappings.** The three index words have exactly one synchronization role
among them. `latest` is the *only* shared mutable state — its RMW is the entire protocol;
`w_work` and `r_work` are thread-private by contract (Rust stores them as `AtomicU32` with
`Relaxed` *only* so `Weft` is `Sync` for scoped-thread sharing — "the atomics are NOT
protocol state," `core/rust/src/lib.rs` §Memory model). Initialization is the permutation
`latest=0, w_work=1, r_work=2` in the implementation; RFC-0001 §4.1 declares any
permutation of {0,1,2} across the three roles valid, and the worked trace in §4.1 of that
RFC exercises a different one — the invariant is exclusivity, not the specific values.

### 2.3 The Kernel Freeze Contract

**What is frozen.** `core/c/weft.{h,c}` and `core/rust/src/lib.rs`'s kernel items are
byte-frozen: every contribution series since PR #5 has proven — via `git diff` against
`origin/main` on exactly those files — that the kernel surface did not move (the worklog
entries for PRs #5–#14 each carry a "kernel freeze 0 diffs" line; the Rust kernel's only
permitted growth was `pub mod` declarations for sibling driver modules). The one sanctioned
kernel API addition of the entire project history is `weft_debug_view` — read-only,
wait-free, allocation-free inspection (WO-P2-TOOLS T1, semver minor).

**Why frozen.** Tier 0 (the envelope) and Tier 1 (kernel/steward APIs) change only through
RFC + two kernel-maintainer approvals (`ARCHITECTURE.md` §6). The freeze is what allows
this volume to make claims of the form "the protocol, as shipped, has property P" without a
moving target underneath: the description, the proofs, and the bytes are pinned to one
revision.

**The driver-layer exception.** New capability lands as *sibling modules* that compose
beside the kernel at zero kernel surface: `fanout.{h,c}` / `fanout.rs` / `fanout.ts`
(RFC-0004), `frame_cursor.*` (RFC-0008), `verified.*` (RFC-0005), `governor.*` (RFC-0009),
and the PR #14 hardware tiers (`shm.*`, `gpu_ring.*`, SIMD batch verification). This is the
"upgrade axes" discipline of `ARCHITECTURE.md` §7 — the future has a directory and a seam,
so it never forces a kernel redesign.

**Arithmetic parity — the bit-exactness mechanism.** Cross-language byte-compatibility is
pinned by three mechanisms, each mechanically gated:

1. **Defined-width arithmetic everywhere.** The litmus payload generator `pat(seq, i)`
   computes `mix32(seq·2654435761 + i·2246822519) & 0xFF` over GF(2³²). C uses `uint32_t`
   (overflow defined); Rust uses `wrapping_mul`/`wrapping_add`; TypeScript uses
   `Math.imul` + `>>> 0`. The three must produce byte-identical sequences — L6's
   differential property depends on it (A5) — and the same mixer family drives the Loom
   model's `tword()`, the C torture gate, and the cross-language interop fixtures, so a
   parity break fails a proof artifact, not just a test (`core/c/weft.c` §Shared payload
   pattern; `core/rust/tests/loom_fanout.rs`).
2. **Byte-compatible wire layouts.** The fan-out ring formula `ring_bytes = 16 + 8M +
   M·payload_bytes` is identical in TS, C, and Rust, and `fixtures/xlang-fanout/` proves
   both directions (TS producer → C consumer, C producer → TS consumer) with bit-exact
   payload validation `[COMMITTED EVIDENCE: litmus/evidence/fanout/xlang-interop.log]`.
3. **Structural validators.** `tools/port_validator.py` carries per-port rule packs
   (existence, spec-citing headers, ordering markers, API surface), and
   `ci/scripts/run_binding_parity.sh` hashes the mirror pairs (core ↔ package twins), so a
   port cannot silently drift from its reference implementation.

**Cross-language arithmetic boundaries are declared, not assumed.** Ring control words are
`u64` in the native ports but surface as `Number` in TS — exact below 2⁵³, unreachable in
any real session at 10⁶ publishes/s (`core/c/fanout.h` §RING LAYOUT). The JVM port is a
*semantics* port (`AtomicLongArray` ctrl, heap `FloatArray` slots — the JVM has no
SharedArrayBuffer; cross-language interop is the JNI path); the pure-Dart kernel is a
single-isolate reference implementation whose honesty wall is quoted in `docs/PORTS.md` §3:
no cross-thread ordering claims transfer from the C/Rust proof. The Swift port uses
`swift-atomics` `.acquiringAndReleasing` — the exact AcqRel map — while its fan-out ring
uses SC stamps because swift-atomics exposes no standalone fence (the P1/P2 pairs cannot be
expressed there; stated as such, `docs/PORTS.md` §6).

### 2.4 Port substrate map (the Atomic seam)

The four porting seams are specified in `ARCHITECTURE.md` §3 (`RawBuffer`, `Atomic`,
`FrameClock`, `DrawScope`); the Atomic seam is the one this volume owns:

| Port | `latest` exchange | Regime vs C reference |
|---|---|---|
| C11 (normative) | `atomic_exchange_explicit(&latest, v, memory_order_acq_rel)` | — |
| Rust | `latest.swap(v, Ordering::AcqRel)` | exact map |
| TypeScript | `Atomics.exchange(ctrl, 0, v)` on `Int32Array` over SAB | SeqCst — the web gives no weaker choice; **stronger, not weaker** (`docs/WHITEPAPER.md` §7.1) |
| Kotlin/JVM | `AtomicReference.getAndSet` (kernel) / VarHandle access modes (ring) | SC-only JVM ≥ AcqRel; ring maps the C regime via `setRelease`/`getAcquire`/`getOpaque` |
| Swift | `ManagedAtomic.exchange(.acquiringAndReleasing)` | exact map (kernel); ring stamps SC (no fence primitive) |
| Dart | plain assignment | single-isolate reference only; production goes via `dart:ffi` to C |

The JVM/TS "stronger ordering" doctrine deserves its one-sentence justification, because
it recurs throughout §3: sequential consistency is a strictly stronger constraint than
acquire-release — every SC execution is a valid AcqRel execution — so a port whose
primitives force SC cannot *remove* a property the AcqRel proof establishes. The doctrine
cuts both ways and the project uses both directions: where the platform forces strength
(TS, JVM), the port declares it and pays for it; where the platform permits weakness, the
port *fences* deliberately (the C/Rust ring's P1/P2) and buys the difference back in
throughput (§3.2).

---

## §3 Concurrency Invariants & Memory Model (I1–I8)

This section formalizes the eight core invariants of the Weft concurrency architecture and
the memory-ordering contract that carries them. The taxonomy below unifies the repository's
historical identifiers — kernel invariants I1–I6 (RFC-0001 §5), fan-out invariants FI1–FI3
(RFC-0004), the Four Laws (`docs/PHILOSOPHY.md` §2), and AXIOM T — into the Volume I
numbering. Each invariant is stated formally, tied to its enforcement mechanism, and mapped
to the proof or test that pins it.

### 3.1 The invariant catalog

**I1 — Monotonicity.** *Within a session, sequence numbers never regress, and the
publication point never runs ahead of the writer.*

$$
\textsc{Monotonicity:}\quad
\mathsf{latestSeq}_t \;\le\; \mathsf{wSeq}_t - 1
\;\;\wedge\;\;
\forall k:\ \mathsf{slotSeq}_k \text{ strictly increasing across overwrites}
$$

In the kernel, each buffer's envelope `seq` is written by the single writer before
publication, so a claimed frame's `seq` is a valid ordering key for freshness policy
(§4.1 `r_seq`). In the ring, frame numbers start at 1 and the writer's `wSeq` only
increments; per-slot stamps are strictly increasing across overwrites, which is the
property that makes an *unchanged* stamp meaningful (I2). Attach semantics preserve
monotonicity across producer handoff: `weft_fanout_attach_writer` continues `w_seq` from
the ring's `latestSeq` rather than restarting at 1, so per-slot stamp monotonicity survives
the handoff (§4.2). The TLA+ encoding is `LatestMonotonic == latest <= wSeq - 1` plus the
`NoFuture` bound on readers (`formal/fanout/FanoutSeqlock.tla`); the FrameCursor's
decreasing-seq reset rule (FC5) handles the one legitimate regression — a *new* session on
the same stream — by restarting accounting rather than misreading it as a burst.

**I2 — Non-Tearing Seqlock Bracket.** *A payload is only observable through a bracket that
makes every overwrite detectable; a reader never accepts a frame assembled from two
writers' passes.*

$$
\textsc{Bracket:}\quad
\mathsf{copy}(F) \text{ accepted} \;\Rightarrow\; \forall w \in F:\ \mathsf{writer}(w) = F
$$

The kernel enforces this by **ownership, not brackets**: the exchange transfers exclusive
ownership (Theorem T1, §5.5), so a claimed buffer cannot be mid-write by construction —
the bracket discipline is the *fan-out ring's* mechanism, where slots are shared and a
reader copies rather than swaps. There, `begin()` invalidates the target slot's stamp
(stores 0) *before* the fill cursor is returned (FI1a), and `publish()` re-stamps the slot
(stores `wSeq`) *after* the fill (FI1b) — the stamp-then-fill bracket. A reader validates
the stamp before its copy (`sB == L`) and re-validates after (`sA == L`); by per-slot
monotonicity (I1), an unchanged stamp proves no overwrite began during the copy (FI2). The
TLA+ form is the safety theorem `NoTornAccepted`: every word of every accepted snapshot
carries the accepted frame's tag — no schedule can make a reader accept a torn frame
(§5.2). This invariant closes the D-17 spike's undetectable tear window: the spike's
`claimLatest` had no seq-before/seq-after validation, so a mid-copy overwrite was silently
accepted; the production implementation brackets both sides.

**I3 — Single-Writer Exclusivity.** *At every instant, at most one writer operates on the
structure, and the writer's working state is private by contract.*

$$
\textsc{SingleWriter:}\quad
\#\{\text{active writers}\} \le 1 \;\;\text{(by contract, enforced at the API layer)}
$$

The kernel's `w_work` is writer-private; `weft_publish` is callable "from the writer thread
only" (`core/c/weft.h` §Writer). The ring encodes the same contract in the type where the
language allows it: Rust's `WeftFanout::begin/fill/publish` take `&mut self` — the
single-writer contract is *in the signature* (`core/rust/src/fanout.rs` §WeftFanout).
Attaching a second broadcaster to a live ring is a caller error, documented and — where
detectable — refused (the C `attach_writer` refuses to attach over a live broadcaster so an
init-allocated ring can never be silently orphaned). The invariant's necessity is the
founding lesson: the withdrawn two-variable protocol derived exclusivity from two relaxed
loads, and the derivation was unsound (RFC-0001 §3). The formal models go further — two
concurrent writers are *unrepresentable* in `FanoutSeqlock.tla` (there is one writer
process), which is the honest encoding: exclusivity is assumed by the models because it is
guaranteed by the API contract, and the litmus L6 ownership battery checks the implemented
consequence (no non-owner ever writes a buffer).

**I4 — Telescoping Reader Identity.** *Per reader, dropped frames account exactly: the sum
of reported drops plus successful fresh claims equals the last observed sequence number.*

$$
\textsc{Telescoping:}\quad
\forall r:\ \sum_{t}\mathsf{dropped}_r(t) \;=\; \mathsf{lastSeq}_r - \mathsf{freshClaims}_r
$$

This is FI3 (RFC-0004 §Reference), checked as the TLA+ invariant
`Telescoping == \A i \in Readers : rDropped[i] = rLast[i] - rFresh[i]`, asserted in the
Loom model at every reachable execution, and asserted in every torture gate (1M-frame C
torture prints `identity=OK` per reader in both ordering regimes
`[COMMITTED EVIDENCE: litmus/evidence/fanout/c-torture-o2.log]`). The kernel-side analog
is the *stale-return rule* (04-LITMUS §0.6): a claim with no intervening publish lawfully
returns the reader's own previously-released buffer carrying its old seq — legal, counted
as `stale_returns` telemetry, never a freshness violation. The identity is the reason drop
accounting can be trusted for user-facing policy (the RFC-0009 governor consumes
`claim.dropped` as its staleness input) and for the RFC-0011 guardian's crash telemetry.

**I5 — Zero-Allocation Hot Path.** *Publish, claim, begin, fill, and view allocate
nothing; every object they touch is pre-allocated with stable identity.*

This is Law 2 ("zero is a contract, not a goal"), and its enforcement is mechanical: the
B5 memory-contract gate asserts `alloc_bytes_delta = 0` and `rss_growth_pages = 0` across
10⁶ frames in C and Rust `[MEASURED x86_64-sandbox sha256:16b5c663]`; the TS port's
equivalent is the allocation-audit discipline (R8/G4 zero-byte windows, the W6 feed bench's
per-window allocation measurement, and the dual-i32 telemetry regime that closed the
measured ~90 B/publish BigInt boxing — §2.2). The fan-out claim record is *identity-stable
and mutated in place* so the hot path allocates nothing even when returning a result
(`weft_fanout_claim` returns a pointer to the reader-owned record). "Zero is chosen over
low because zero is binary, measurable, and assertable: a budget of 'low' drifts, an
assertion of 'zero' fails loudly" (`docs/PHILOSOPHY.md` §2).

**I6 — Safe Revocation/Reclaim.** *A released Weft is never written afterwards, across any
FFI boundary, without relying on any runtime to panic the writer out of a use-after-free.*

$$
\textsc{ReclaimSafety:}\quad
\mathsf{poison} \;\text{happens-after}\; \mathsf{ACK} \;\text{happens-after}\; \mathsf{writer's\ final\ byte\ write}
$$

The handshake (02 §6, A1): the releaser stores `revoked = true` with **Release**; the
writer checks it at the *top* of its next publish with a **Relaxed** load (advisory —
correctness does not depend on seeing it *this* publish; the *next* one will) and ACKs
with `epoch.fetch_add(1, AcqRel)`; the releaser polls `epoch` with **Acquire** until it
advances past the pre-revoke value, bounded by a timeout. Only then may pages be poisoned
or freed. The ACK is load-bearing: between the writer's revocation check and its envelope
write there is a window, and freeing in that window is a use-after-free across FFI —
"poison-before-ACK is the bug; poison-after-ACK is the protocol" (`core/c/weft.c`
§weft_reclaim). The happens-before chain is Theorem T5 (§5.5); L7 pins it empirically with
a spinning native writer and poisoned pages. The reattach extension (RFC-0006) carries the
same discipline across process death, and `ReattachPolicy.tla` proves the stronger
statements `NoStaleAccess` (the SIGSEGV class unrepresentable), `NoBlindAttach`, and
`NoLeakOnRealloc` (§5.3).

**I7 — Bounded Drop Accounting.** *Every path is bounded; a skip, a miss, or an exhausted
retry is counted in reader statistics — never silent, never a spin.*

Law 1 in arithmetic form: the fan-out claim loop is bounded by `MAX_CLAIM_ATTEMPTS = 4`
(the retry only happens when a *newer* frame completed during the claim, and the newer
frame's own slot is self-consistent, so convergence is immediate — "4 is generous, not
tuned," `core/ts/fanout.ts`); exhaustion keeps the reader's last consistent frame and
increments `torn_exhausted`. A mid-overwrite tick with no newer frame is a *graceful skip*
(`skipped_mid_overwrite`), and drops telescope exactly (I4). The kernel's contribution to
this invariant is wait-freedom itself: publish and claim are fixed instruction sequences
with one RMW each (L2/L3 step bounds of 2, asserted per run), so there is no retry loop to
bound in the first place. The L5 progress test sweeps reader holds 0–100 ms plus full
suspension and requires writer throughput flat within tolerance — no back-pressure exists
to hide behind.

**I8 — Cache Coherence.** *The protocol's observability guarantees ride the cache-coherent
shared-memory substrate; layout and ordering are chosen so that coherence makes the
brackets work and never silently breaks them.*

Three concrete commitments. (i) **Alignment**: buffers and ring regions are 64-byte
aligned (§2.2), so no envelope field shares a cache line with a foreign writer's data and
no platform-permitted word tearing can split a protocol field. (ii) **Atomicity of the
index words**: `latest`/`latestSeq`/`slotSeq` are single atomic words — the entire
protocol state that two threads observe concurrently is a handful of machine words
exchanged by RMW, which is exactly the substrate cache coherence (MESI-class protocols)
guarantees to be sequentially consistent at the variable granularity. (iii) **Ordering
below the brackets**: payload words in the C/Rust rings are *Relaxed atomic* u32 accesses
on both sides — race-free in the strict C11 sense (a plain access concurrent with the
opposing side is a race by definition, whatever the brackets; the relaxed atomics carry the
bracket discipline at zero x86 cost) and this is what keeps the ring TSAN-clean
(`core/c/fanout.h` §MEMORY ORDERING; TSAN torture: 100k frames, ~31M claims, zero race
reports `[COMMITTED EVIDENCE: litmus/evidence/fanout/c-torture-tsan.log]`). The fence
placements (P1/P2, §3.2) are stated in terms of the *architecture-level* visibility
semantics they rely on — x86-TSO's in-order store retirement and AArch64's DMB ISH
propagation — and the divergence between the TS port's SeqCst-everything and the native
ports' fenced acq/rel is measured (~11% publish-throughput delta under identical 4-reader
contention, environment-tagged) rather than argued away.

### 3.2 Memory-ordering taxonomy: Acquire–Release vs SeqCst across three languages

**The kernel matrix.** The kernel's ordering contract is small enough to audit in one
table, and the audit *is* the mechanism: `litmus/catalog.yaml` carries the
`ordering_matrix` verbatim from `02-KERNEL.md` §5, the kernels cite it and do not restate
it, and "no `memory_order_seq_cst` anywhere in the kernel (06 §2)" is a review-rejectable
property (`core/c/weft.h` header). The matrix:

| Operation | C11 | Rust | TypeScript | Role |
|---|---|---|---|---|
| `latest` exchange (publish & claim) | `exchange(…, acq_rel)` | `swap(…, AcqRel)` | `Atomics.exchange` (SeqCst) | THE ownership transfer — publishes writer's payload/envelope/canary (Release half) and acquires the returned buffer's final state (Acquire half) |
| `revoked` writer load | `load(relaxed)` | `load(Relaxed)` | `Atomics.load` (SeqCst) | advisory I6 check |
| `revoked` releaser store | `store(true, release)` | `store(Release)` | `Atomics.store` (SeqCst) | pairs with the ACK chain |
| `epoch` ACK | `fetch_add(1, acq_rel)` | `fetch_add(AcqRel)` | `Atomics.add` (SeqCst) | "I will never write again" |
| `epoch` reclaim poll | `load(acquire)` | `load(Acquire)` | `Atomics.load` (SeqCst) | observes the ACK; licenses poison/free |
| telemetry (`t_*`) | `fetch_add(relaxed)` | `fetch_add(Relaxed)` | dual-i32 `Atomics.add` | statistics only, never synchronization (AXIOM T) |

The taxonomy's philosophy in one line: **pay only for the ordering the proof needs.** The
kernel needs exactly one synchronizing edge per direction of transfer — the AcqRel
exchange — plus the I6 chain. Everything else is Relaxed *by design*, and the reason is
not performance theater: every unnecessary ordering constraint is an unnecessary claim
about every future port's memory model, and the project's porting surface (§2.4) is the
widest thing it owns.

**Where SeqCst is required, not wasteful.** The fan-out ring is the instructive case
because its payload is *shared* (copied, not swapped), so acquire-release alone cannot
carry the bracket. Two properties force fences (`core/c/fanout.h` §MEMORY ORDERING):

- **P1 (writer side)**: no payload word of a new fill may become visible to a reader
  before the invalidate stamp does. A **Release** store orders *prior* accesses below it —
  it cannot prevent *subsequent* fill stores from becoming visible early. The SeqCst store
  of the invalidate **plus a SeqCst fence** before the cursor is returned does express it,
  on every targeted implementation (x86-TSO: stores retire in order; AArch64: DMB ISH
  propagation order).
- **P2 (reader side)**: if the copy observed any word of an overwrite, the revalidation
  load must observe the invalidate-or-newer stamp. **Acquire** stops *later* operations
  from hoisting above it — it cannot stop the copy from sinking below the check. One
  **SeqCst fence between copy and revalidation** closes exactly that gap.

The TS port pays SeqCst on every stamp because JS Atomics offer no weaker mode — the
documented "stronger but noisier" stance (06-PITFALLS §4) — and the native ports implement
the *fenced acq/rel* default with the all-SeqCst regime available as a compile-time A/B
(`-DWEFT_FANOUT_SEQ_CST=1`) so both regimes are torture-gated while only the cheaper one
ships. The measured cost of the stronger regime is honest and cited: ~11% lower publish
throughput under identical 4-reader contention (`docs/PORTS.md` §5). The ordering map is
normative per port in `docs/PORTS.md` §§5–6, including the JVM's VarHandle mapping
(`setVolatile`+`fullFence` for P1, `setRelease` stamps, `getAcquire` loads, `getOpaque`
payload words) and Swift's declared SC-stamps divergence (no standalone fence primitive).

**Rust and C11 divergence rule.** Absolute numbers may differ across languages; what must
hold identically is the *structural* set: the ordering matrix itself, the step bounds, and
the schedule identity wherever the PRNG drives it (A5/A6). A port that cannot express the
matrix (Dart's single isolate) inherits no ordering claims at all and says so — the
honesty load-bearing wall (`docs/PORTS.md` §3).

### 3.3 AXIOM T — telemetry is not a correctness reference

> Telemetry counters (claims_per_s, publish counters, max_published, and any future
> counter) are advisory. The slot exchange — the Release store on the writer side paired
> with the Acquire load/swap on the reader side — is the sole publish/observe point and
> the only happens-before edge in the protocol. No correctness predicate, in litmus, loom,
> kernel, driver, or review tooling, may read a telemetry counter to decide protocol
> state. — ratified WO-P1-CLOSURE §2

The axiom exists because two independent Phase-0 defect findings traced to the same root
cause: a lagging telemetry store mistaken for the publish point (the L4 freshness
false-RED and the loom v1 false-RED are the same defect class). Telemetry lags the
exchange by construction. The axiom is normative everywhere this volume cites a counter —
torture `identity=OK` lines are *post-join* reads (legal), the governor consumes drop
counts as *policy input* (legal — it decides what to draw, not what was published), and
the debug views of §4.1 are advisory samples by the same axiom. It is also the boundary
that makes the TS dual-i32 regime (§2.2) acceptable: a counter whose transient carry
window is observable once per 2³² increments is harmless precisely because no correctness
predicate may read it.

---

## §4 Complete Core Kernel & Protocol API Reference

This section is the complete reference for the two protocol surfaces Volume I owns: the
low-level Triad kernel (`weft_init` … `weft_debug_view`) and the RFC-0004 fan-out
protocol (`begin`/`publish`/`claim`/`view`/`stats`/`ring_bytes`). The C header
`core/c/weft.h` / `core/c/fanout.h` is normative; Rust (`core/rust/src/lib.rs`,
`fanout.rs`) and TypeScript (`core/ts/weft.ts`, `fanout.ts`) mirrors are shown where their
shapes differ. Signatures are verbatim from the cited revision; every entry lists
pre-conditions (caller obligations), post-conditions (guaranteed effects), and the memory
ordering of every atomic it performs. Two global contracts govern everything below:

- **The forbidden patterns** (02 §2.2 — any one in review rejects the PR): a second atomic
  participating in buffer-ownership decisions; candidate-set / exclusion-set / "pick a
  free slot" logic; CAS retry loops in publish/claim (wait-freedom = zero loops); claim
  returning failure (a claim always yields a buffer); copy-on-read inside claim (readers
  observe the LIVE buffer, A3); allocation of any kind in publish/claim.
- **AXIOM T** (§3.3): no counter returned by any telemetry/debug accessor may be used as a
  correctness reference.

### 4.1 Low-Level Kernel Core API

#### 4.1.1 Lifecycle

```c
int  weft_init(weft_t* w, size_t payload_max);   // C — 0 on success, -1 on alloc failure
void weft_destroy(weft_t* w);                    // C — idempotent
```
```rust
pub fn new(payload_max: usize) -> Option<Self>   // Rust — None on alloc failure; Drop frees
```
```ts
constructor(payloadMax: number)                  // TS — throws on nothing; SAB-backed
```

**Semantics.** Allocates the three buffers, each `buf_size = align64(16 + payload_max + 8,
64)` bytes, 64-byte aligned (C: `posix_memalign(64, …)`; Rust: `Layout::from_size_align`;
TS: one SAB of `64 + 3·buf_size`). Writes a **null envelope** (magic, version 1,
header_size 16, seq 0, payload_len = payload_max) into all three buffers and fills the null
frame's payload with `pat(0, i)` so the reader's first claim — which may return any of the
three — observes a *valid, verifiable* frame (04-LITMUS §0.6: "a kernel that zero-fills the
null frame makes L6 false-red"). Initializes `latest = 0, w_work = 1, r_work = 2`, `revoked
= false`, `epoch = 0`, all counters zero.

**Pre-conditions.** `payload_max` is the immutable per-frame payload capacity; choose it
once. C: `w` points to writable storage. **Post-conditions.** The ownership permutation
holds (§3.1 I3); a claim before any publish returns the null frame with `seq = 0`.
**Allocation.** Permitted here and only here (Law 2). **Failure.** C cleans up partial
allocations and returns −1; allocation failure is the *only* failure mode of construction.

`destroy` (C) / `Drop` (Rust) frees the three buffers. **Pre-condition (caller error,
documented not defended):** if `revoke` was ever called, `reclaim` must have returned 0
(ACK observed) before destroy while a writer may still run; otherwise the caller has
created the use-after-free I6 exists to prevent. If revoke was never called, plain free is
correct — no writer exists. Idempotent in C.

#### 4.1.2 Writer side (writer thread only)

```c
uint8_t* weft_w_begin(weft_t* w);                              // live payload cursor
int      weft_w_write_payload(weft_t* w, const uint8_t* src, size_t len);  // bulk copy
weft_pub_result_t weft_publish(weft_t* w, uint32_t seq, uint32_t payload_len);
```
```rust
pub unsafe fn w_write_payload(&self, src: *const u8, len: usize) -> Result<(), ()>
pub unsafe fn publish(&self, seq: u32, payload_len: u32) -> PubResult   // Ok | DroppedRevoked
```
```ts
wBegin(): Uint8Array;  wBeginFloat32(): Float32Array;  wWritePayload(src: Uint8Array): number
publish(seq: number, payloadLen: number): PubResult
```

**`w_begin`** returns a live cursor into the writer's working payload region
`buf[w_work] + 16` (TS: the cached per-slot `Uint8Array`/`Float32Array` views, allocated
once at construction so `wBegin()` never allocates). **Not a snapshot** — writes through it
become visible to the reader after the next `publish`. The cursor stays valid until the
next publish rotates `w_work`. Pre-condition: caller is the single registered writer (I3);
must not be called after revoke + ACK.

**`w_write_payload`** copies `len` bytes to offset 16 of the working buffer; returns 0 / −1
C-style (Rust: `Ok`/`Err`) if `len > payload_max`, writing nothing. Rust's variant is
`unsafe` with a `SAFETY:` comment citing RFC-0001 §4 ("writer owns `w_work` exclusively
between its exchanges") — the only `unsafe` in the Rust kernel outside `r_read_slice`.

**`weft_publish`** — THE writer protocol, verbatim from `core/c/weft.c` (02 §2 + §6):

```c
weft_pub_result_t weft_publish(weft_t* w, uint32_t seq, uint32_t payload_len) {
    if (atomic_load_explicit(&w->revoked, memory_order_relaxed)) {   // 1. revoked FIRST
        atomic_fetch_add_explicit(&w->epoch, 1, memory_order_acq_rel);   // ACK
        atomic_fetch_add_explicit(&w->t_drop, 1, memory_order_relaxed);
        return WEFT_PUB_DROPPED_REVOKED;
    }
    weft_envelope_encode_v1(w->buf[w->w_work], seq, payload_len);    // 2. envelope
    uint64_t canary_val = (uint64_t)seq;                            // 3. canary = seq
    memcpy(w->buf[w->w_work] + canary_offset(w->buf_size), &canary_val, 8);
    uint32_t old = atomic_exchange_explicit(&w->latest, w->w_work,
                                            memory_order_acq_rel);  // 4. THE atomic
    w->w_work = old;                                                // 5. recycled via latest
    atomic_fetch_add_explicit(&w->t_publish, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&w->t_wsteps, 1, memory_order_relaxed);
    return WEFT_PUB_OK;                                             // 6.
}
```

**Pre-conditions:** single writer; `payload_len ≤ payload_max`; payload bytes the reader
should see were written through `w_begin`/`w_write_payload` *before* this call (the
Exchange's Release half publishes them). **Post-conditions:** on `PUB_OK`, the previously
published frame is now the writer's scratch (`w_work = old`) and the new frame is reachable
through `latest`; on `DROPPED_REVOKED`, nothing was written — the caller MUST NOT touch
buffer bytes again (this is the I6 release edge to the caller; the Phase-0 TSAN finding
that taught it is quoted in §5.2 of `docs/WHITEPAPER.md`). **Steps:** one Relaxed load, one envelope write, one canary write, one AcqRel exchange —
the L2 step-count bound (≤ 2, §6.2) asserts exactly this shape. **Ordering:** the AcqRel exchange is the only synchronizing
operation — Release publishes payload + envelope + canary; Acquire takes the returned
buffer's final state. TS implements the same steps with SeqCst Atomics (stronger, §3.2)
and a dual-u32 canary write reproducing the exact `BigInt(seq)` bit pattern, including
sign-extension for negative seq, with zero allocation.

> **Rust parity note** (quoted from the source, `core/rust/src/lib.rs`): publish previously
> re-filled the payload with `pat(seq, i)` — a litmus convenience the C kernel does not do.
> It silently overwrote payload written through `w_write_payload` and put ~560 ns of
> `pat()` inside the measured publish. Fixed 2026-09-16; the writer fills via cursor before
> publish, exactly as C. Payload bytes beyond what the writer wrote keep their previous
> contents — the same stale-tail contract as C.

#### 4.1.3 Reader side (reader thread only)

```c
uint32_t weft_r_claim(weft_t* w);                       // NEVER fails; returns held index
uint32_t weft_r_seq(weft_t* w);        uint16_t weft_r_header_size(weft_t* w);
uint32_t weft_r_magic(weft_t* w);      uint32_t weft_r_payload_len(weft_t* w);
size_t    weft_r_read_slice(weft_t* w, uint8_t* dst, size_t offset, size_t dst_len);
const uint8_t* weft_r_live_ptr(weft_t* w, size_t offset);
```
```rust
pub fn claim(&self) -> u32                     // Rust: r_seq/r_magic/r_payload_len live
pub unsafe fn r_read_slice(&self, dst: *mut u8, offset: usize, dst_len: usize) -> usize
pub fn r_live_ptr(&self, offset: usize) -> *const u8
```
```ts
claim(): number;  rSeq(): number;  rHeaderSize(): number;  rMagic(): number;  rPayloadLen(): number
rReadSlice(offset: number, len: number): Uint8Array     // view, not copy (allocates the view)
rLive(): Uint8Array;  rLiveFloat32(): Float32Array       // cached payload views, zero-alloc
```

**`claim`** — THE reader protocol: `mine = latest.exchange(r_work, AcqRel); r_work = mine;
return mine`. Never fails, never retries, cannot block; before any publish it returns the
null frame (seq 0). The Acquire half sees the writer's published payload/envelope/canary;
the Release half hands the reader's previous buffer back with its final state. **A3 (live
read):** the claimed buffer is read in place — `r_seq`/`r_magic`/`r_header_size`/
`r_payload_len` read the *held* buffer at call time, not a snapshot frozen at claim; the
buffer is exclusively the reader's until its next claim (RFC-0001 §4.3). `rReadSlice`
(TS) returns a **view** over the SAB — a fresh view object per call, which is why
draw-phase code must use the cached `rLive()`/`rLiveFloat32()` views instead (Law 2); C's
`weft_r_read_slice` copies `min(dst_len, buf_size − offset)` bytes and returns the count —
the window may include envelope and canary bytes because `offset` is an absolute buffer
offset. `r_live_ptr` returns `buf[r_work] + offset` for verify-in-place (L1/L6), or NULL
when `offset ≥ buf_size`.

**Reader-faster-than-writer is legal** (04-LITMUS §0.6): a claim with no intervening
publish returns the reader's own previously-released buffer with its old seq — the display
policy above the kernel skips frames with `seq ≤ last_rendered`; litmus counts them as
`stale_returns` telemetry.

#### 4.1.4 I6 — revocation handshake

```c
void weft_revoke(weft_t* w);                                     // step 1
int  weft_reclaim(weft_t* w, uint32_t pre_revoke_epoch, uint32_t timeout_ms);  // steps 2–3
```
```rust
pub fn revoke(&self);  pub fn reclaim(&self, pre_revoke_epoch: u32, timeout_ms: u32) -> Result<(), ()>
```
```ts
revoke(): void;  reclaim(preRevokeEpoch: number, timeoutMs: number): boolean
```

**`revoke`** (releaser thread): `revoked.store(true, Release)` — pairs with the ACK chain
into reclaim's Acquire poll, establishing the happens-before edge that makes poison/free
safe. **`reclaim`** (releaser thread): polls `epoch` (Acquire) until it differs from the
*pre-revoke* value the caller captured before revoking, bounded by `timeout_ms` (C paces
with 100 µs nanosleeps; Rust 100 µs thread sleeps; TS parks via `Atomics.wait` where the
platform allows and falls back to a bounded busy loop on the main thread). Returns 0 /
`Ok` / `true` on ACK received — after which poison (memset 0xDE) or free is safe (L7
verifies with poisoned pages and a spinning native writer); −1 / `Err` / `false` on
timeout, meaning the writer has not quiesced and the buffers must *not* be touched.
**Worst-case in-flight writes at revocation: one** (the writer checks revoked at the top of
every publish, so a writer running at any rate ACKs within one publish — RFC-0001 §6).

#### 4.1.5 Telemetry & debug

```c
uint64_t weft_t_publish/weft_t_claim/weft_t_drop/weft_t_wsteps/weft_t_rsteps(weft_t* w);
uint32_t weft_epoch(weft_t* w);   bool weft_revoked(weft_t* w);
void weft_debug_view(const weft_t* w, weft_debug_view_t* out);
```

Counters are Relaxed loads — statistics only (AXIOM T). `t_wsteps`/`t_rsteps` are the L2/L3
step counters (one protocol RMW per publish/claim; asserted ≤ 2 by litmus). `epoch()` is an
Acquire load because I6 polling is its only sanctioned use. `weft_debug_view` is the ONE
sanctioned kernel API addition (WO-P2-TOOLS T1): read-only, wait-free, allocation-free
inspection returning advisory samples — `latest` (Relaxed), `w_work`/`r_work`
(thread-private, advisory-only sampling), `revoked`, `epoch`, counters, two live buffer
samples with owner codes (0 free / 1 writer / 2 reader / 3 in-exchange), a
`mid_publish_sample` flag set when a header's magic is wrong while seq ≠ 0 (sampled
mid-write — report raw, never fail), and the no-live-owner sentinel `slot_idx = 3`. It
never dereferences freed/poisoned buffers (I6 rule: "a debug tool that causes
use-after-free is a Law-2/Law-4 violation in one move"). TS's `debugView()` samples all
three slots — the SAB is GC-managed, so the freed-buffer hazard class does not exist there
(stated divergence, `core/ts/weft.ts` §debugView).

#### 4.1.6 Envelope codec & shared pure functions

```c
void weft_envelope_encode_v1(uint8_t* dst, uint32_t seq, uint32_t payload_len);
void weft_envelope_encode(uint8_t* dst, uint16_t version, uint16_t header_size,
                          uint32_t seq, uint32_t payload_len);
weft_decode_result_t weft_envelope_decode(const uint8_t* buf, size_t avail,
                                          uint16_t* version, uint16_t* header_size,
                                          uint32_t* seq, uint32_t* payload_len);
uint16_t weft_negotiate(uint16_t writer_version, const uint16_t* reader_versions, size_t n);
uint32_t weft_mix32(uint32_t x);  uint8_t weft_pat(uint32_t seq, uint32_t i);
uint32_t weft_xorshift32(uint32_t* state);
```

Rust: `envelope_encode_v1/envelope_encode/envelope_decode/negotiate/mix32/pat/xorshift32`
(free functions; decode returns `Result<(u16, u16, u32, u32), DecodeResult>`). TS:
`envelopeEncodeV1/envelopeEncode/envelopeDecode/negotiate/mix32/pat/xorshift32` over
`DataView` with `littleEndian = true` throughout. All pure — no threads, no kernel state;
the decoder's seven rules and the 16-byte layout are specified in §4.3. `negotiate`
implements `chosen = max({v ∈ S : v ≤ W})`, empty set → 0 (`BIND_INCOMPATIBLE`). The
litmus generators (`mix32`, `pat`, `xorshift32` — Marsaglia 13/17/5, state 0 reseeded to
`0x9E3779B9`) are part of the *conformance* surface: byte-identical across languages (A5),
they drive L1/L6, the torture gates, the Loom model's `tword()`, and the chaos engine's
seed derivation (§2.3).

### 4.2 RFC-0004 Zero-Copy Fan-Out Protocol API

The fan-out ring serves 1 writer and N independent readers over one shared region — the
driver-layer answer to "primary canvas, minimap, flight recorder, and network visualizer
on one stream" without touching the frozen 1:1 kernel. The layout is byte-compatible
across TS/C/Rust (the interop contract, §2.3), which is what unblocks fan-out for Android
JNI, Flutter FFI, C++ engines, and Rust audio graphs.

#### 4.2.1 Ring layout & geometry

```text
byte 0              latestSeq   _Atomic u64   0 = no frame yet; frames from 1
byte 8              publishes   _Atomic u64   telemetry (one add per publish)
byte 16 + 8k        slotSeq[k]  _Atomic u64   0 = INVALIDATED (fill in progress)
byte 16 + 8M        payload     M slots × payload_bytes (slot k at +k·payload_bytes)
```

```c
size_t weft_fanout_ring_bytes(size_t payload_bytes, unsigned slot_count);
```
```rust
pub fn ring_bytes(payload_bytes: usize, slot_count: usize) -> Option<usize>
```
```ts
// TS constructor validates: payloadBase(slotCount) + slotCount * payloadFloats * 4
```

`ring_bytes = 16 + 8M + M·payload_bytes`. **Geometry contract:** `payload_bytes > 0` and a
multiple of 4 (u32 word granularity — the TS port's `payloadFloats` constraint);
`slot_count ∈ [2, 64]` (`WEFT_FANOUT_MAX_SLOTS`; RFC-0004 recommends 4–8 — the bound keeps
the cached per-slot view arrays inside the reader struct, one line each, no heap). Bad
geometry returns 0 / `None` / throws. Frame *f* lives in slot `(f−1) mod M` for its entire
published life. Cross-language sessions keep ctrl values < 2⁵³ (TS surfaces seq as
`Number`) — declared, not assumed. In TS the slots are `Float32Array` views
(`payloadFloats` per slot); in C/Rust they are u32-word-addressable byte regions — same
bytes, same formula.

#### 4.2.2 Broadcaster (the writer side — single writer by contract)

```c
int       weft_fanout_init(weft_fanout_t* f, size_t payload_bytes, unsigned slot_count);
int       weft_fanout_attach_writer(weft_fanout_t* f, void* ring, size_t ring_bytes,
                                    size_t payload_bytes, unsigned slot_count);
uint8_t*  weft_fanout_begin(weft_fanout_t* f);
int       weft_fanout_fill(weft_fanout_t* f, const void* src, size_t len);
uint64_t  weft_fanout_publish(weft_fanout_t* f);
void      weft_fanout_destroy(weft_fanout_t* f);
weft_fanout_t* weft_fanout_new(size_t payload_bytes, unsigned slot_count);
void      weft_fanout_free(weft_fanout_t* f);
void      weft_fanout_debug_stats(const weft_fanout_t* f, weft_fanout_debug_t* out);
const void*   weft_fanout_ring(const weft_fanout_t* f);
```
```rust
impl WeftFanout {
    pub fn new(payload_bytes: usize, slot_count: usize) -> Option<Self>;
    pub unsafe fn attach_raw(base: *mut u8, ring_len: usize, payload_bytes: usize,
                             slot_count: usize) -> Option<Self>;   // foreign ring (FFI)
    pub fn begin(&mut self);                                        // + fill, fill_f32
    pub fn publish(&mut self) -> u64;
    pub fn create_reader(&self) -> WeftFanoutReader;
    pub fn as_bytes(&self) -> &[u8];  pub fn geometry(&self) -> (usize, usize);
    pub fn debug_stats(&self) -> FanoutDebugStats;
}
```
```ts
class WeftFanoutBroadcaster {
  constructor(payloadFloats: number, slotCount: number = 4);
  begin(): Float32Array;   publish(): number;   createReader(): WeftFanoutReader;
  debugStats(): FanoutDebugStats;   readonly sab: SharedArrayBuffer;
}
```

**`init`/`new`** allocates the single region (`posix_memalign(64)` / one Rust `Layout`
allocation / one SAB), zero-initialized so `latestSeq = 0` (no frame yet) and every
`slotSeq = 0` (all invalidated) — the same invariants a fresh SAB gives the TS port.
Returns −1/`None` on bad geometry or allocation failure. **May** allocate (Law 2 applies
to begin/fill/publish/claim only).

**`attach_writer`/`attach_raw`** binds a broadcaster to *foreign* ring memory (produced by
another port, or shared over FFI/shm). Geometry is validated against `ring_bytes` — a
mismatched pair fails fast instead of tearing. `w_seq` continues from the ring's
`latestSeq` (Acquire) so per-slot stamp monotonicity survives producer handoff (I1).
C refuses to attach over a live broadcaster (an init-allocated ring can never be silently
orphaned); Rust's `attach_raw` is `unsafe` with the full aliasing contract in its
`# Safety` docs. **SINGLE WRITER BY CONTRACT** — attaching a second broadcaster to a live
ring is a caller error: document, don't defend.

**`begin()`** — the FI1 bracket's first half, verbatim from `core/c/fanout.c`:

```c
uint8_t* weft_fanout_begin(weft_fanout_t* f) {
    f->w_seq += 1;
    const unsigned k = (unsigned)((f->w_seq - 1) % f->slot_count);
    atomic_store_explicit(f->ctrl + FAN_IDX_SLOTSEQ + k, 0, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);            // P1: invalidate BEFORE the fill
    f->w_slot = k;
    f->w_cursor = f->ring + fan_payload_base(f->slot_count) + (size_t)k * f->payload_bytes;
    return f->w_cursor;
}
```

Bumps the frame counter, **invalidates** the target slot's stamp (SeqCst store) **plus a
SeqCst fence** before the cursor is returned (property P1, §3.2), and returns the slot's
payload cursor (TS: the cached `Float32Array` view — zero allocation). The cursor stays
valid until the next `begin()`; writes through it are visible to readers only after
`publish()`. Pre-condition: single writer.

**`fill(src, len)`** copies `len` bytes (multiple of 4, ≤ payload_bytes) into the begun
slot via **Relaxed atomic u32 word stores** — the race-free fill path (strict C11 +
TSAN-clean; §3.1 I8). The raw cursor from `begin()` remains available for production
plain/Float32 writes under the bracket discipline (the TS port's stance). Returns words
written, or −1/`None` on bad length or missing `begin()`. **`fill_f32`** (Rust) is the
`&[f32]` form — `to_bits()` per element, same Relaxed stores.

**`publish()`** stamps the slot (Release), flips the publication point `latestSeq`
(Release — the only point at which the frame becomes claimable), bumps `publishes`
(Relaxed telemetry), and returns the frame seq — or **0 if no `begin()` ever ran**: a
*detectable no-op*, not an error (TS parity). Post-condition: the frame's payload is
bracket-complete; any reader targeting an older frame in this slot will now observe the
stamp change (I2).

**`new`/`free`** (C) exist for FFI-finalizer discipline: Dart's `NativeFinalizer` and the
JNI bridge need allocate-and-release to be ONE C call each, "so a foreign runtime's GC
backstop can never free the struct while the ring it owns is still alive" —
`weft_fanout_free` has the `void(void*)` signature `NativeFinalizer` requires.

**`debug_stats`/`ring`** — advisory cold-path snapshots (AXIOM T); `ring()` exposes the
base pointer for FFI runtimes that read the documented ctrl offsets directly
(unsynchronized, advisory only).

#### 4.2.3 Reader (N per ring, each fully independent)

```c
int  weft_fanout_reader_init(weft_fanout_reader_t* r, const void* ring, size_t ring_bytes,
                             size_t payload_bytes, unsigned slot_count);
const weft_fanout_claim_t* weft_fanout_claim(weft_fanout_reader_t* r);
const void* weft_fanout_view(const weft_fanout_reader_t* r);
void weft_fanout_reader_stats(const weft_fanout_reader_t* r, weft_fanout_stats_t* out);
weft_fanout_reader_t* weft_fanout_reader_new(const void* ring, size_t ring_bytes,
                                             size_t payload_bytes, unsigned slot_count);
void weft_fanout_reader_destroy/weft_fanout_reader_free(weft_fanout_reader_t* r);
```
```rust
impl WeftFanoutReader {
    pub unsafe fn attach_raw(base: *const u8, ring_len: usize, /* … */) -> Option<Self>;
    pub fn claim(&mut self) -> &FanoutClaim;         // identity-stable record, mutated in place
    pub fn view(&self) -> &[u32];   pub fn view_bytes(&self) -> &[u8];
    pub fn view_f32_into(&self, dst: &mut [f32]);    pub fn last_seq(&self) -> u64;
    pub fn stats(&self) -> FanoutReaderStats;
}
```
```ts
class WeftFanoutReader {
  constructor(sab: SharedArrayBuffer, payloadFloats: number, slotCount?: number);
  claim(): FanoutClaim;   view(): Float32Array;   stats(): FanoutReaderStats;
}
```

**`reader_init`** validates geometry against the actual region size (a mismatched pair
fails fast instead of tearing), allocates the reader's **own pre-allocated copy buffer**
(`payload_bytes`, 64-aligned; TS: `new Float32Array(payloadFloats)`), caches per-slot
views (zero-alloc claims), and zeroes `last_seq` and all stats. A reader may attach in any
thread that received the region (TS: post the SAB via structured clone — SABs are shared,
not copied). Init may allocate; claim never does.

**`claim()`** — the bounded validate–copy–revalidate discipline (RFC-0004
§Reference; verbatim structure from `core/c/fanout.c`):

```text
L = latestSeq (Acquire)
if L == 0 or L == lastSeq:        not fresh; keep last consistent frame
for attempt in 0..4:              WEFT_FANOUT_MAX_CLAIM_ATTEMPTS
    k = (L-1) mod M;  sB = slotSeq[k] (Acquire)
    if sB != L:                   slot mid-overwrite, or re-stamped by a newer frame
        L2 = latestSeq (Acquire)
        if L2 == L: SKIP this tick (counted — Law 1; never a spin); not fresh
        else:       L = L2 (a newer frame completed — chase it)
    copy slot k -> reader buffer (Relaxed atomic u32 loads, word by word)
    fence(SeqCst)                 // P2: copy ordered before the revalidation load
    sA = slotSeq[k] (Acquire)
    if sA == L:                   consistent frame L (FI2: unchanged stamp proves
                                  no overwrite began during the copy)
        dropped = L - lastSeq - 1;  advance lastSeq;  fresh = true;  ACCEPT
    else:                         torn copy — retry on the newest completed frame
attempts exhausted:               not fresh, counted (torn_exhausted); keep last frame
```

**Never blocks, never spins unboundedly, never fails.** Returns the reader-owned,
identity-stable claim record — `{ fresh, seq, dropped }` — mutated in place (Law 2: read
it synchronously; do not retain it across claims expecting a snapshot). `dropped`
telescopes exactly (I4). `view()` returns the reader's stable copy buffer, meaningful
after a fresh claim — read it live before the next claim (the A3 discipline).
`view_f32_into` (Rust) reinterprets into a caller-owned `&mut [f32]` with zero allocation.

**`stats()`** — advisory (AXIOM T): `reads`, `fresh`, `drops` (sum of `dropped`),
`skipped_mid_overwrite` (graceful skips), `torn_exhausted` (bounded-retry exhaustions).

#### 4.2.4 Constants

| Constant | Value | Rationale |
|---|---|---|
| `WEFT_FANOUT_MAX_SLOTS` / `FANOUT_MAX_SLOTS` | 64 | bound for cached per-slot view arrays (no heap); RFC-0004 recommends 4–8 |
| `WEFT_FANOUT_MAX_CLAIM_ATTEMPTS` / `MAX_CLAIM_ATTEMPTS` | 4 | a retry only happens when a NEWER frame completed during the claim; its own slot is self-consistent, so convergence is immediate — "generous, not tuned" |

### 4.3 Wire specifications

#### 4.3.1 The 16-byte frame envelope (Tier 0, frozen)

```text
offset  size  field         value / meaning
0       4     magic         ASCII "WEFT" = bytes 57 45 46 54  → LE u32 0x54464557
4       2     version       1 = triad-1
6       2     header_size   16 (self-describing — the growth mechanism)
8       4     seq           frame sequence, u32; 0 = null frame; wraps at 2^32-1
12      4     payload_len   payload bytes following the header
              TOTAL = 16 bytes — little-endian, no exceptions, all languages, forever
```

**Decode rules** (03-ENVELOPE §2, implemented verbatim in all three codecs — §4.1.6):

1. `avail >= 16`, else `DECODE_SHORT`;
2. `magic == "WEFT"`, else `DECODE_BAD_MAGIC`;
3. read `version`, `header_size`;
4. `header_size >= 16` and `header_size <= avail`, else `DECODE_BAD_HEADER`;
5. **the payload begins at `header_size` — NEVER at 16** (hardcoding 16 fails L8b by
   construction);
6. `payload_len <= avail − header_size`, else `DECODE_SHORT`;
7. unknown trailing fields (between 16 and `header_size`) are **skipped without
   validation** — the encoder fills them with `0xAA` by convention (L8b).

**Evolution rules:** new fields → larger `header_size`, same 16-byte prefix, bumped
`version`; old readers skip what they do not know (rule 7). A semantic change to an
existing field ships as a new version, never an in-place edit. "The envelope never changes
shape; it only gains versions" — the Apache Arrow lesson (`ARCHITECTURE.md` §4). The
extended header sketch in that document (dtype/dims/shape fields for tensor payloads) is
the forward-looking triad-2 growth path through exactly this mechanism; the shipped v1
envelope is the 16 bytes above.

**Bind-time version negotiation** (03-ENVELOPE §3): the reader offers a set `S` of
supported versions; the writer declares `W`; `chosen = max({v ∈ S : v ≤ W})`; an empty set
yields 0 = `BIND_INCOMPATIBLE`. (The §5 illustrative table's row 3 `(W=2, S={1}) →
BIND_INCOMPATIBLE` was a Phase-0-identified typo — the formula is normative and produces
1; corrected in the v1.1 amendments.)

#### 4.3.2 The 64-byte control slot (TS kernel SAB)

The TS kernel's control block (§2.2 diagram) is itself a wire-adjacent specification:
when a Weft SAB is posted to another thread, the receiving side constructs `Weft`-shaped
views over the documented offsets — `latest` at i32 slot 0, `w_work` 1, `r_work` 2,
`revoked` 3, `epoch` 4, and the three u64 telemetry counters as i32 slot pairs 10–15
(bytes 40–63), with the BigInt64 view (u64 slots 5–7) reading the same 8-byte pairs.
Cross-language kernel sessions are NOT an interoperability surface (the kernel is
per-process by design; cross-process and cross-language fan-out travel on the *ring*
format below, or the .weftrec record format) — the control-block map is documented for
tooling, the debug views, and the guardian's wire manifest, which reads these offsets from
the *real* kernel's bytes rather than echoed constants (`tools/guardian/wire-manifest.json`).

#### 4.3.3 The ring control block & the little-endian bracket discipline

The RFC-0004 control block (§4.2.1) is the canonical cross-language, cross-process wire
format: `latestSeq` at byte 0, `publishes` at byte 8, `slotSeq[k]` at `16 + 8k`, payload
arena at `16 + 8M` — all stamp words u64 little-endian, all payload words u32
little-endian (the Dart port's copy is an explicit LE word loop precisely so host
endianness can never leak through). Two bracket disciplines are normative on this wire:

- **Kernel bracket**: envelope → canary → AcqRel exchange (§4.1.2) — ownership transfer;
  the reader's visibility edge is the exchange itself.
- **Ring bracket**: invalidate (SeqCst store + fence, P1) → fill (Relaxed u32 words) →
  stamp (Release) → publication point (Release) — copy-then-revalidate with the P2 fence
  between copy and revalidation (§4.2.3).

A producer that follows either bracket is readable by any compliant consumer, in any
language, in-process or over shared memory — this is the property the SHM ring sessions of
PR #14 (RFC-0011-ipc-shm-ring-sessions) build on, and the property the xlang fixtures
prove bit-exactly in both directions.

### 4.4 Complete worked examples (all three languages)

**C — kernel round-trip with I6 teardown (`core/c/weft.h` contract):**

```c
weft_t w;
if (weft_init(&w, 4096) != 0) { /* allocation failure — the only failure mode */ }

/* writer thread */
uint8_t* cur = weft_w_begin(&w);            // live payload cursor, buf[w_work] + 16
fill_my_frame(cur, 4096);                   // your bytes, in place, zero-copy
weft_publish(&w, seq++, 4096);              // envelope + canary + THE exchange

/* reader thread (draw phase) */
uint32_t held = weft_r_claim(&w);            // never fails; returns held index
uint32_t s = weft_r_seq(&w);                 // live envelope fields of the held buffer
const uint8_t* p = weft_r_live_ptr(&w, 16);  // live payload pointer (verify-in-place)

/* teardown (releaser thread) */
uint32_t pre = weft_epoch(&w);
weft_revoke(&w);
if (weft_reclaim(&w, pre, 2000) == 0) {      // ACK observed — writer quiescent
    weft_destroy(&w);                        // now, and only now, free
}
```

**Rust — fan-out: one writer, three independent consumers (`core/rust/src/fanout.rs`):**

```rust
use weft_core::fanout::WeftFanout;

let mut b = WeftFanout::new(256 * 4, 4)?;    // 4 slots × 256 f32 words
let mut r_minimap = b.create_reader();        // N readers, each independent
let mut r_network = b.create_reader();

loop {
    b.begin();                                // slot stamp invalidated (P1); &mut self = single writer
    b.fill_f32(&samples);                     // Relaxed atomic word stores under the bracket
    b.publish();                              // stamp + publication point (Release)
    // … on each consumer's own thread/cadence:
    let claim = r_minimap.claim();            // { fresh, seq, dropped } — mutated in place
    if claim.fresh { r_minimap.view_f32_into(&mut scratch); draw(&scratch); }
}
```

**TypeScript — fan-out across threads (SAB posted via structured clone):**

```ts
import { WeftFanoutBroadcaster, WeftFanoutReader } from "@weft/core";

const b = new WeftFanoutBroadcaster(256);     // 4-slot ring by default
postMessage(b.sab);                           // SABs are SHARED by structured clone

const slot = b.begin();                       // Float32 view; stamp invalidated first
slot.set(pcm);                                // fill
b.publish();                                  // seqcst stamps (the web's only mode)

// in the receiving worker:
const r = new WeftFanoutReader(sab, 256);     // geometry validated against byteLength
const claim = r.claim();                      // { fresh, seq, dropped }, mutated in place
if (claim.fresh) render(r.view());            // the reader's stable copy buffer
```

---

## §5 Formal Verification & Mathematical Proofs

### 5.1 The verification pyramid

Weft's evidence is organized as a pyramid in which each tier covers the blind spot of the
one above it, and every tier's limits are named where the tier is cited
(`rfcs/0011-exhaustive-state-space-proofs.md` §Rationale):

| Tier | Instrument | Completeness | Blind spot (covered by the tier below/beside) |
|---|---|---|---|
| Exhaustive-formal | TLA+ / TLC over finite state spaces | **every schedule the model can express**, incl. liveness under weak fairness | the model is an abstraction: memory ordering, real arithmetic, language runtime |
| Bounded-exhaustive | Rust `loom` under a preemption bound | every interleaving under bound 2 (3 nightly) at the *memory-model* level, with real atomics | unbounded schedules; scale (M=2, WORDS=2, FRAMES=3 by design) |
| Adversarial-empirical | torture gates (1M frames × 4 readers, word-validated), TSAN/ASAN, deterministic chaos engine (10⁷+ nightly iterations, byte-identical verdicts across C/TS/JVM/Dart) | production scale + production distribution + *replayable* failures (seeded) | sampling, never exhaustion — but reproducible everywhere |
| Conformance | the litmus catalog (L1–L8 × 3 languages, §6) | the shipped implementations, debug + release, ≥30 s × 5 repetitions per cell | adversarial parameters are a fixed catalog |

The split of duties is deliberate and repeated across artifacts: **Loom owns safety
exhaustiveness, litmus L4 owns liveness** ("Liveness is NOT checked here — owned by
litmus L4" — `core/rust/tests/loom_fanout.rs` header; the kernel model says the same).
TLC is the only tier that proves liveness (under explicit weak fairness), and it does so
inside a finite abstraction. The chaos engine exists because "a tear that fires once in
ten million frames cannot be re-run, because the schedule that triggered it is gone"
— its seeded schedule makes a red verdict replay with the same seed, in every language
(`rfcs/0011` §Motivation).

### 5.2 TLA+ — `FanoutSeqlock` (the multi-reader ring)

**What the model is** (`formal/fanout/FanoutSeqlock.tla`): the RFC-0004 ring at the
granularity of its atomic operations. The writer's `WBegin` (invalidate) / `WFillOne` /
`WStamp` / `WPublish` and each reader's `RPoll` / `RStampB` / `RCopyOne` / `RStampA`
phases are **separate atomic actions**, so TLC explores every interleaving — *including
the tear window*: a reader's word-by-word copy can be interleaved with the writer's
invalidate/refill of the same slot. The modeling decision that makes this honest: a
payload word's value is the FRAME TAG that wrote it (the deterministic `tword(seq, w)`
family the real implementations use), so a copy whose words do not all carry the accepted
tag is a tear, and `NoTornAccepted` asserts none is ever accepted. The first draft made
the stamp-check and copy atomic — which would have made `NoTornAccepted` **vacuous**; the
final model copies one word per action so TLC genuinely explores the tear window (the
lesson is recorded in the spec history, `patches/PATCHES-WAVE4.md`).

**Checked bounds** (`FanoutSeqlock.cfg`): `READERS = 2, SLOTS = 2, WORDS = 2, FRAMES = 3`
— "the smallest shape containing a full ring wrap"; the nightly tier widens to 3 readers /
4 frames. `MaxAttempts = 4` mirrors the shipped constant. `CHECK_DEADLOCK TRUE`.
Fairness: weak fairness on every progress action (`Liveness == … WF_Vars(…)`); the
`.cfg` checks the temporal properties `WriterFinishes` and `ReaderSeesFinal`.

**The invariant conjunction** (verbatim):

```tla
Inv == TypeOK /\ StampBracket /\ LatestMonotonic /\ NoFuture
       /\ NoTornAccepted /\ Telescoping
```

| Invariant | Verbatim statement | Meaning |
|---|---|---|
| `TypeOK` | per-variable domain membership | every variable stays in its declared shape |
| `StampBracket` | `\A k : (slotSeq[k] = 0 /\ wUsed[k]) => (wPhase = "fill" /\ wSlot = k)` | a stamp is 0 ONLY while the writer is mid-fill on that slot (FI1) |
| `LatestMonotonic` | `latest <= wSeq - 1` | the publication point never runs ahead of the writer (I1) |
| `NoFuture` | `\A i : rLast[i] <= FRAMES` | no reader accepts a frame the writer has not published |
| `NoTornAccepted` | `\A i : rLast[i] = 0 \/ \A w : rGood[i][w] = rLast[i]` | **the safety theorem**: every word of every accepted snapshot carries the accepted frame's tag |
| `Telescoping` | `\A i : rDropped[i] = rLast[i] - rFresh[i]` | the RFC-0004 accounting identity (I4) |

**Liveness**: `WriterFinishes == <>(wSeq > FRAMES)` and
`ReaderSeesFinal == \A i : <>(rDone[i])` — the writer publishes every frame and every
reader eventually accepts the final one (no starvation, no deadlock), under the explicit
weak-fairness conjuncts of `Spec`.

**Results** — re-executed for this volume at `6a9a4db` with the pinned prover
(tla2tools 1.8.0, sha256 `9d36716f…` — "a different prover is a RED," the prover itself
is part of the evidence chain) `[RE-VERIFIED 2026-09-17T17:18Z @ 6a9a4db]`:

```text
Model checking completed. No error has been found.
19454 states generated, 6481 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 46.
… complete state space with 19443 total distinct states …
calculated (optimistic): val = 4.6E-12
```

| Metric | Value | Which TLC line it comes from |
|---|---|---|
| States generated | 19,454 | `Progress(46) … states generated` |
| Distinct states (invariant search) | **6,481** | `… distinct states found` (final progress line) |
| Complete state space (incl. temporal product) | **19,443** | `… with 19443 total distinct states` |
| Search depth (state-graph diameter) | **46** | `The depth of the complete state graph search is 46` |
| Fingerprint-collision probability | 4.6 × 10⁻¹² | TLC's optimistic estimate |
| Temporal properties | 3 branches checked, clean | `Checking 3 branches of temporal properties …` |

The two "distinct states" figures are both real and both cited in the repository's own
evidence: 6,481 is the invariant-checking exploration's distinct-state count (the number
the volume assignment cites), 19,443 the complete state space including the
implied-temporal product used for the liveness check (the number `patches/PATCHES-WAVE4.md`
cites); they come from the same single run of the same model at the same bounds, and the
table above gives each with the exact TLC output line it derives from — Law 4 applied to
the proof artifacts themselves.

### 5.3 TLA+ — `ReattachPolicy` (process death)

**What the model is** (`formal/reattach/ReattachPolicy.tla`): the RFC-0006 OS-level state
machine — a host process (writer or reader) may be **killed** at any time; on recreation,
one of two policies applies: `CLEAN_REALLOCATE` (release handles, fresh ring, generation
bump, sequence restarts at 1 — zero stale pointers can exist) or `REHYDRATE_PERSISTED`
(attach to surviving shared memory ONLY after validating its 32-byte header; a failed
validation falls back to clean realloc). The I6-style epoch disciplines the boundary:
frames published into a generation are only ever published/claimed through a handle bound
to that generation, and `RRevalidate` is the epoch-mismatch path that turns a stale attach
into a fresh one. Deaths are bounded (`DEATHS = 2`) so the space is finite; "the
interesting behaviors are all inside two deaths per process."

**The invariant conjunction** (verbatim):

```tla
Inv == TypeOK /\ NoStaleAccess /\ NoBlindAttach /\ NoLeakOnRealloc
       /\ SeqMonotonic /\ DroppedAccounted /\ Telescoping
```

`NoStaleAccess` is the headline: *the SIGSEGV class is unrepresentable* — a reader may
hold a stale attach after the writer's clean realloc (in reality it still holds the
pointer), but it can never CLAIM through one, because `RClaim`'s guard requires the live
generation. `NoBlindAttach`: a running process exists only after its recreate ran a
validation. `NoLeakOnRealloc`: a live process never holds an unreleased dangling handle.
`Telescoping` carries the RFC-0004 identity across process death, per attach session.
Liveness: `ProcessesReattach` (a killed process eventually recreates and reattaches) and
`ReaderCatchesUp` (a live reader eventually accepts the current generation's final frame —
or its own death intervenes, modeled, not hidden).

**Results** — re-executed identically `[RE-VERIFIED 2026-09-17T17:18Z @ 6a9a4db]`:
`Model checking completed. No error has been found.` — 2,063 states generated; **615
distinct states** in the invariant search; **2,460 total distinct states** in the complete
space; **search depth 17**; fingerprint-collision probability 4.8 × 10⁻¹⁴; 4 branches of
temporal properties checked clean. (TLC emits a benign warning that `rHdrOK` is changed
while listed UNCHANGED in `RRecreate` — a known modeling artifact of the `EXCEPT`
notation, not an error; the checker completes clean.)

### 5.4 Rust Loom verification (bounded preemption model checking)

**The kernel model** (`core/rust/tests/loom_model.rs`, v2 per WO-P3 T1 / WO-P1-CLOSURE
§2): exhaustively explores all interleavings of 3 publishes × 3 claims × 3 buffers with
the exchange serialized through a `loom::sync::Mutex<(latest_idx, published_wm)>` so the
no-future watermark is fused into the writer's step with no interleaving point between
them — which a packed atomic cannot express. Four assertions hold across every
interleaving: **(a)** ownership (each buffer ≤ 1 owner, implicit in the serialized
exchange), **(b)** no torn observation (`verify_frame` checks seq + payload + canary per
claim), **(c)** no future (`claimed_seq ≤ published_wm` at claim time, strong form), and
**(d)** join-quiescence (at join, `published_wm == MAX_PUBLISHED_SEQ`). Result: exhaustive
model clean, 1 test / 0 failed `[COMMITTED EVIDENCE: litmus/evidence/loom/loom.txt,
loom-v2.txt]`.

The **model-fidelity note** is quoted here because it is the honest statement of what
Loom does and does not establish: the Mutex is a property of the *model*, not of the
kernel; the exhaustive proof covers the protocol's **state machine** under mutual
exclusion, while the shipped kernel is lock-free (one AcqRel exchange per side) — and
memory-ordering correctness of the lock-free exchange is carried by (i) the ownership
argument of §5.5 T1 and (ii) TSAN 5×8 on the real implementation. "Any suggestion that
the kernel should grow a lock is a misreading of this section"
(`docs/WHITEPAPER.md` §6c).

**The fan-out model** (`core/rust/tests/loom_fanout.rs`): the ring as Loom atomics at the
same layout semantics (`ModelRing`), with `writer_frame` mirroring `WeftFanout::begin +
fill + publish` *exactly* — same orderings, same fence placement — and `model_claim`
mirroring the reader. Scale: `M = 2, WORDS = 2, FRAMES 3, 2 readers × claims`; "the
protocol is width- and scale-independent (the C/TS ports exercise production scales); the
model buys exhaustiveness instead." Assertions: **(a)** no torn frame accepted (any
`fresh == true` claim carries payload words all belonging to that frame), **(b)** no
future (claimed seq ≤ writer's last publish), **(c)** exact telescoping per reader in
every reachable execution, **(d)** join state `publishes == FRAMES`. Liveness is owned by
the C torture gate and litmus L4 — the same split as the kernel model.

**Preemption bounds.** The embedded default is `LOOM_MAX_PREEMPTIONS = 2` — every
interleaving the memory model allows under at most 2 thread preemptions, including
Relaxed-load staleness (the tear source), terminating in ~22 s inside `cargo test`; the
bound-3 sweep is the stronger exploration (~194 s) behind `#[ignore]`, run by the nightly
chaos leg. Both clean `[COMMITTED EVIDENCE: litmus/evidence/fanout/rust-suite.log]`:

```text
### LOOM_MAX_PREEMPTIONS=2 … test result: ok … finished in 21.69s
### LOOM_MAX_PREEMPTIONS=3 … test result: ok … finished in 193.94s
```

**Four deep-models** extend the surface (RFC-0011 §Formal models): *reordered fill*
(the writer's word order reversed — `writer_frame_reversed`), *three readers*, *reader
rejoin after multiple ring wraps*, and the *preemption-bound-3 sweep*. Each narrows a
specific gap between the model and the adversarial reality the torture gates sample.

### 5.5 Mathematical proofs

The arguments below are the "by construction" proofs written out; T1–T2 are the kernel's
safety core (RFC-0001 §4.4, formalized), T3–T4 the ring's accounting and bracket
theorems, T5 the I6 happens-before chain. Exhaustive machine verification of T3, T4 (and
the ring half of T1) is §5.2–5.4; the proofs here are the human-auditable statement of
what those machines checked.

**T1 — Ownership exclusivity (non-tearing, kernel).** *At every point in the total order
of RMWs on `latest`, each of the three buffers has exactly one owner in
{writer, reader, exchange-slot}, and `latest` never names a buffer mid-write or mid-read.*

*Proof.* There is exactly one shared variable, `latest`, and exactly two kinds of
operations mutate the ownership function: the writer's exchange
`old ← latest.exchange(w_work)` and the reader's `mine ← latest.exchange(r_work)`. Both
are single-word atomic RMWs on the *same* location, so the memory system serializes them
into a total order <sub>R</sub>; each RMW is indivisible in that order. Define the owner
map O : {0,1,2} → {W, R, X} (exchange slot). Initially O is a permutation (init state,
§2.2). A writer exchange replaces the pair (O(w_work) = W, O(latest) = X) with
(O(w_work) = X, O(old) = W) — it transfers exactly its working buffer INTO the exchange
slot and takes the exchange slot's buffer OUT as its new working buffer; a reader
exchange is symmetric. Each step preserves "O is a bijection onto {W, R, X}", and no
other operation touches O (w_work/r_work are private; payload writes target the
writer-owned buffer only). Induction over <sub>R</sub>. Mid-write safety: the writer
writes `buf[w_work]` strictly *before* its exchange enters <sub>R</sub>, and `latest`
names a buffer only via an exchange that already retired — so the buffer `latest` names
was fully written when it entered the slot, and the buffer the reader receives was named
by `latest` at the moment its own exchange retired. Torn reads are therefore impossible
by ownership, not by timing. ∎

**T2 — Wait-freedom (both sides).** *`publish` and `claim` complete in a bounded, fixed
number of steps regardless of any concurrent activity.*

*Proof.* `claim` executes exactly one atomic RMW plus private assignments (§4.1.3);
`publish` executes one Relaxed load, two bounded memory writes (envelope: 16 bytes; all
field sizes are compile-time constants — canary: 8 bytes), one atomic RMW, and two
Relaxed counter increments (§4.1.2). Neither contains a loop, a retry, a conditional
branch on *another thread's* state (the revoked check branches on a flag, and both
outcomes return immediately), nor any operation whose duration depends on the opponent.
Single-word RMWs on a coherent location complete in bounded time by the substrate's
guarantee (§3.1 I8). Hence both sides are wait-free with per-side step counts that litmus
L2/L3 assert ≤ 2 protocol RMWs. ∎

**T3 — Telescoping identity (ring).** *For every reader, at every point in its claim
sequence, Σ dropped = lastSeq − freshClaims.*

*Proof.* All of a reader's state transitions occur in its own claim steps (readers are
independent; the ring's shared state is read-only to them). Invariant form: let D, F, L
be the reader's cumulative drops, fresh claims, and last accepted seq. Initially
D = F = L = 0. The only transition that changes any of them is an ACCEPT of frame L′:
D′ = D + (L′ − L − 1), F′ = F + 1, L′ = L. Then D′ − (L′ − F′) = D + L′ − L − 1 − L′ +
F + 1 = D − (L − F). The base case satisfies D = L − F = 0; skips, misses, and exhausted
attempts leave all three unchanged; hence by induction over the reader's own event
sequence the identity holds at every point. (Accepting frames out of order is impossible:
`claim()` only ever targets `latestSeq`, and `L = latestSeq > lastSeq` is a precondition
of every copy attempt — monotonicity of `latestSeq` under the single writer, I1.) This is
the induction TLC checks exhaustively as `Telescoping` and the torture gates check
empirically as `identity=OK`. ∎

**T4 — Bracket soundness (ring, FI1 + FI2 ⇒ no torn accept).** *If a reader accepts a
copy of slot k performed while `slotSeq[k]` read L before the copy and L after the copy,
then every payload word of the copy was written by the writer's fill for frame L.*

*Proof.* Per-slot stamps are strictly monotonic across overwrites (I1: the single writer
re-stamps slot k only with its strictly increasing `wSeq`, and invalidates to 0 before
every fill — FI1). Suppose the reader's copy of slot k observed at least one word from a
different writer pass. Any other pass on slot k is either an *earlier* frame's completed
fill (whose words were overwritten before frame L's stamp was written — impossible for
the copy to see, since the copy read `sB = L` *after* L's stamp was stored, and word
stores of the L-fill precede its stamp) or a *later* overwrite begun after L was stamped.
In the latter case the later pass's first protocol action on slot k is the FI1
invalidate — `slotSeq[k] ← 0` (SeqCst store, fenced before the fill: P1) — so any copy
that observed an overwritten word is ordered, by the P2 SeqCst fence between the copy and
the revalidation load, such that the revalidation must observe the invalidate-or-newer
stamp: `sA ≠ L`, and the copy is rejected (torn), retried on the newest completed frame,
or skipped — never accepted. Contrapositively, `sA = L` after the copy certifies no
overwrite began during the copy, and with `sB = L` before it, the copy window lies
entirely inside frame L's bracket — all words belong to L. (This is `NoTornAccepted`,
proved exhaustively by TLC over the interleaving space in which the copy and the
overwrite genuinely interleave word-by-word — §5.2.) ∎

**T5 — Reclaim safety (I6).** *If the releaser observes `epoch ≠ pre_revoke_epoch`, then
every buffer byte write by the writer happens-before the releaser's subsequent poison/free
access.*

*Proof.* The releaser's `revoked.store(true, Release)` precedes (program order) its
`epoch` polling. The writer's ACK `epoch.fetch_add(1, AcqRel)` is executed only after
`revoked.load(Relaxed)` observed true at the top of a publish; in that publish, no buffer
byte is written (the revoked branch returns before any write), and by the kernel's
contract the writer performs no buffer writes after a `DROPPED_REVOKED` return. The
fetch_add is AcqRel on the same atomic variable the releaser polls with
`load(Acquire)`: when the poll observes the new value, the ACK (and everything the writer
sequenced before it — in particular its final byte write of the pre-revoke publish)
happens-before the observing load, hence happens-before everything the releaser does
after the poll, including the poison memset and `free`. The chain
`writer's final byte write → ACK → poll → poison` is therefore a happens-before chain,
and the use-after-free class is unreachable in every execution consistent with the C11/Rust
memory model. (The Relaxed revoked load is safe because correctness does not depend on
*which* publish first observes it — the next one will — only on the fact that the ACK
follows the last byte write in the writer's program order, which the contract
guarantees.) ∎

### 5.6 The chaos tier and the guardian (empirical-exhaustive complement)

Two PR #12 instruments complete the pyramid and are cited here because they pin the same
invariants by different means. The **deterministic chaos engine**
(`core/c/fanout_chaos.{h,c}` and the TS/JVM/Dart/Swift mirrors) injects preemption,
memory-bus stalls, CPU throttling, and payload-word reordering into concurrent
reader/writer pairs under a pinned PRNG contract (xorshift128 with a fixed draw order:
thread pick, fault-rate check, victim and kind), adjudicating the property ledger
L-C1..L-C6 (no torn accepted frame, no future, exact telescoping, publish completion,
bounded-and-counted resolutions, stamp bracket) at drain end. The stepped verdict is
byte-identical across languages on the same config — `run_chaos_parity.sh` byte-diffs
C vs TS vs JVM vs Dart and every port's CI pins the committed golden fixture — so a red
verdict replays with the same seed, everywhere. The **guardian watchdog**
(`tools/guardian/`) flags ≥3% throughput drops against a pinned baseline, a *single byte*
of wire-layout drift (observed at runtime by a C probe that reads the real kernel's
bytes, not echoed constants — 52 fields in the wire manifest), and any crash signature;
its selftest requires it to *bite* on poisoned fixtures, "a watchdog that stays green on
a bad fixture is itself a red gate" (`rfcs/0011` §Summary).

---

## §6 Litmus Conformance Suite (L1–L8)

### 6.1 The suite is the core

The litmus suite is the project's canonical artifact — a language-agnostic catalog of
protocol scenarios, each with a deterministic thread script, adversarial parameters, a
mechanical verdict, and the invariant it proves (`litmus/README.md` §1). Per-language
runners execute the catalog against each implementation; the runners are the only
platform-specific code in the directory. "A port that passes the full catalog on its
target may truthfully call itself Weft; a port that passes nine of ten tests has failed,
and there is no partial credit." The suite exists because the founding draft's protocol
passed happy-path tests and was still formally unsound — the entire design philosophy of
the project (claims with boundaries, zeros as contracts) is downstream of tests that can
actually fail.

Runner-level requirements for all tests: **debug and release builds both run the catalog**
(debug asserts canaries; release asserts timing bounds — debug instrumentation violates
step-count bounds by construction); **L1's stretched holds are injected by the harness**,
suspending the reader *between its swap and its read-completion* — the actual adversarial
window, not `Thread.sleep` folklore; every test runs **a minimum wall time (30 s) and is
repeated 5×** — "concurrency bugs are probabilistic, and single passes certify nothing."

### 6.2 The catalog (L1–L8)

The machine-readable specification is `litmus/catalog.yaml` (v2); params below are
catalog-owned, not runner-owned:

| Test | Proves | Adversary (catalog params) | Verdict (mechanical) |
|---|---|---|---|
| **L1-tear** | I1 (kernel) — no torn reads | reader hold 5/10/50 ms between swap and read; writer 240 Hz (2× display); payload_max 1024; 600 frames; per-language exposure floor `min_claims` C/Rust 600, TS 200 | `torn == 0` across all holds AND `drain_ok` AND `claims ≥ min_claims` |
| **L2-writer-steps** | I2 — wait-free writer | instrumented publish under reader holds swept 0/10/25/50/100 ms; 2000 publishes at 2000 Hz | `max_wsteps ≤ 2` AND `publishes == 2000` in every config |
| **L3-reader-steps** | I3 — wait-free reader | writer storm 960 Hz (4×); 2000 claims | `max_rsteps ≤ 2` AND `claims == 2000` AND zero claim failures |
| **L4-freshness** | I4 — latest-wins | writer 960 Hz vs reader 240 Hz (4×); 2000 frames | `freshness_violations == 0` AND `future_violations == 0` AND `drain_exact` |
| **L5-progress** | I5 — no back-pressure | reader holds 0/10/50/100 ms + full suspension; 1 s windows at 2000 Hz | `max(rate)/min(rate) ≤ 1.5` across all five configs |
| **L6-ownership** | I1+I5 — exclusivity | randomized writer/reader delays (xorshift32, seed `0x00C0FFEE`), 200 trials × 64 frames, ≤500 µs delays | `violations == 0`; same seed → identical schedule across languages (A5) |
| **L7-revocation** | I6 — no use-after-free | `release()` under a concurrently spinning uncapped native writer; timeout 2000 ms | `reclaim_ok` AND `post_revoke_revoked == 100` AND `poison_intact` |
| **L8-envelope** | Tier 0 stability | none — pure functions + one live Weft, < 1 s | round-trip byte-identical; unknown fields ignored; `triad-1` and hypothetical `triad-2` coexist (4 subchecks) |

### 6.3 Results matrix

24/24 cells green under the v1.1 predicates (8 tests × 3 canonical languages, debug and
release — 48 runner invocations; the historical Phase-0 sign-off recorded 20/24 plus four
documented findings, all resolved by the v1.1 amendments — §6.4)
`[COMMITTED EVIDENCE: litmus/REPORT.md]`:

| Test | C | Rust | TS |
|---|---|---|---|
| L1-tear | ✅ torn=0 | ✅ torn=0 | ✅ torn=0 |
| L2-writer-steps | ✅ wsteps=1 | ✅ wsteps=1 | ✅ wsteps=1 |
| L3-reader-steps | ✅ rsteps=1 | ✅ rsteps=1 | ✅ rsteps=1 |
| L4-freshness | ✅ fresh_viol=0 | ✅ fresh_viol=0 | ✅ fresh_viol=0 |
| L5-progress | ✅ ratio=1.018 | ✅ ratio=1.023 | ✅ ratio=1.0 |
| L6-ownership | ✅ viol=0 | ✅ viol=0 | ✅ viol=0 |
| L7-revocation | ✅ poison=True | ✅ poison=True | ✅ poison=True |
| L8-envelope | ✅ all | ✅ all | ✅ all |

The TSAN leg adds 5 runs × 8 tests = 40 executions with **zero reports** (post-L7
harness fix) `[COMMITTED EVIDENCE: litmus/evidence/tsan/summary.txt]`, and the driver
layer's F-series (below) re-verified for this volume in both ordering regimes —
`PASS: 0 failure(s)` from `fanout-test` (fenced acq/rel) and `fanout-test-seq`
(all-SeqCst) `[RE-VERIFIED 2026-09-17T17:19Z @ 6a9a4db]`.

### 6.4 The honest history: v1.1 amendments and documented findings

The suite's authority comes from its willingness to be wrong in public. Three Phase-0
findings are part of the record (`litmus/REPORT.md`):

1. **L4-freshness RED in C and Rust (S < P0 on jitter)** — the v1.0 predicate fired when
   the reader *outpaced* the writer and lawfully re-claimed its own stale buffer. A spec
   strictness, not a protocol bug: the freshest frame at claim time *is* the reader's old
   buffer when no publish intervened. The v1.1 predicate distinguishes new-frame vs
   stale-return; all three languages then green.
2. **L1-tear exposure-floor shortfall (TS)** — claims hovered near the floor of 200 under
   load (236/164/164/171/166 across 5 probes). The ratified `EXPOSURE-RETRY` rule retries
   once on `claims < min_claims`, labels the retry, and **never auto-greens** a shortfall;
   the TS runner additionally gained an adaptive exposure window (schedule repeats to 10 s
   wall cap) so statistical power holds on every runner speed. The floor is an
   exposure-sufficiency bound — "the reader claimed enough times that a tear, if possible,
   would have been caught" — not the property under test; the property is `torn == 0`.
3. **L8 negotiation table typo** — `(W=2, S={1}) → BIND_INCOMPATIBLE` contradicted the
   normative formula (which yields 1); §5's table row corrected per the formula.

The stale-return rule (04-LITMUS §0.6) that finding 1 codified deserves its own
paragraph because it is often misread: under the exchange protocol, a claim made with no
publish since the reader's previous claim returns the reader's own previously-released
buffer carrying its old seq. This is *designed behavior* — a claim always yields a buffer
(I3) — and is not a freshness violation. The display policy above the kernel skips frames
with `seq ≤ last_rendered`; the harnesses count them as `stale_returns` telemetry. The
same rule is why L1's verdict is `torn == 0 && drain_ok && claims ≥ floor` rather than any
stronger sequencing claim, and why the reader-runs-first interleaving in the Loom model is
a legal schedule whose relevant property is safety, not liveness
(`docs/WHITEPAPER.md` §6c).

### 6.5 The F-series (driver-layer conformance)

The kernel suite is untouched by the fan-out work — "No L-series change — the kernel
litmus suite is canonical for kernel semantics" (RFC-0004 §Litmus impact). Driver-layer
conformance is the F-series battery (28 tests in `packages/core/test/fanout.test.ts` at
acceptance; `core/c/fanout_test.c` F1–F9 + FC1–FC6 and the Rust mirrors since PR #5),
covering: geometry rejection and the ring-bytes formula (F1); null-frame first claim (F2);
three-unseen-publishes drop accounting (F3); telescoping identity + convergence + stats
agreement (F4); mid-overwrite graceful skip and recovery (F5); detectable no-op publish
and fill validation (F6); producer handoff — attach continues frame numbering, second
attach refused, cross-producer accounting (F7); debug stats (F8); the FFI allocator pair
and raw-offset reads (F9); and the FrameCursor rules (FC1–FC6, RFC-0008). On top of it:

- **Torture**: 1 writer × 4 readers × 1M frames, every fresh claim word-validated, both
  ordering regimes, exact telescoping per reader, e.g. fenced-acq/rel at 510,560
  publishes/s `[COMMITTED EVIDENCE: litmus/evidence/fanout/c-torture-o2.log]`; Rust
  release torture 1M frames in 1.46 s `PASS`.
- **Sanitizers**: ASAN and TSAN builds of the F-series green; the TSAN torture (100k
  frames, ~31M claims) reports **zero data races** — the relaxed-atomic payload design
  (§3.1 I8) `[COMMITTED EVIDENCE: litmus/evidence/fanout/c-torture-tsan.log]`.
- **Cross-language interop**: TS producer → C consumer and C producer → TS consumer over
  the shipped `@weft/core` dist, bit-exact payload validation both directions
  `[COMMITTED EVIDENCE: litmus/evidence/fanout/xlang-interop.log]`.
- **Cross-thread protocol litmus** (TS): an independent worker-side writer — plain JS
  implementing the RFC layout without importing the class — publishes 100k frames while
  three readers validate every claimed byte; zero torn claims (plus the F-series
  re-verification for this volume, §6.3).

### 6.6 Evidence index

| Artifact | Path |
|---|---|
| Litmus catalog (machine-readable, v2) | `litmus/catalog.yaml` |
| Litmus report (matrix + findings) | `litmus/REPORT.md`, `litmus/results.json` |
| TLC models + configs | `formal/fanout/FanoutSeqlock.{tla,cfg}`, `formal/reattach/ReattachPolicy.{tla,cfg}` |
| TLC re-verification transcripts (this volume) | `litmus/evidence/whitepaper-vol1/tlc-{fanoutseqlock,reattachpolicy}.log`, `tlc-driver-transcript.log` |
| C F-series re-verification (this volume, both regimes) | `litmus/evidence/whitepaper-vol1/c-fseries-reverify.log` |
| Kernel Loom transcripts (v1 + v2) | `litmus/evidence/loom/loom.txt`, `loom-v2.txt` |
| Fan-out Rust suite (F-series + torture + Loom bounds 2/3) | `litmus/evidence/fanout/rust-suite.log` |
| C F-series ×4 builds | `litmus/evidence/fanout/c-fseries.log` |
| C torture ×4 builds (O2/seqcst/ASAN/TSAN) | `litmus/evidence/fanout/c-torture-{o2,seqcst,asan,tsan}.log` |
| Cross-language interop | `litmus/evidence/fanout/xlang-interop.log` |
| TSAN 5×8 summary | `litmus/evidence/tsan/summary.txt` |
| Chaos + formal + guardian wave | `patches/PATCHES-WAVE4.md`, `rfcs/0011-exhaustive-state-space-proofs.md` |
| Bench tables (measured numbers) | `bench/results.json`, `docs/WHITEPAPER-TABLES.md` |

---

## Appendix A — Reproducibility

**Cited revision.** `origin/main @ 6a9a4db` (Merge PR #13). All file paths are relative to
the repository root at that revision.

**Re-verification executed for this volume** (2026-09-17T17:18–17:19Z, sandbox:
Linux x86_64, gcc 14.2.0, OpenJDK for TLC; commands as run):

```bash
# 1. TLA+ models, pinned prover (mirrors ci/scripts/run_formal_shard.sh)
#    tla2tools 1.8.0, sha256 9d36716ffb5e49d1…993fef — mismatch is a RED
( cd formal/fanout    && java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC \
      -config FanoutSeqlock.cfg FanoutSeqlock.tla )   # → No error has been found.
( cd formal/reattach  && java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC \
      -config ReattachPolicy.cfg ReattachPolicy.tla ) # → No error has been found.

# 2. C fan-out F-series, both ordering regimes
make -C core/c fanout-test      && ./core/c/fanout-test       # PASS: 0 failure(s)
make -C core/c fanout-test-seq  && ./core/c/fanout-test-seq   # PASS: 0 failure(s)
```

**Full local battery** (per the repo's own gates):

```bash
make -C core/c fanout-test-asan fanout-test-tsan    # sanitizers
./core/c/fanout-runner torture 1000000 4 256 4      # 1M frames, 4 slots, 256 words, 4 readers
python3 tools/litmus_driver.py --langs c,rust,ts    # 24-cell litmus matrix
( cd core/rust && cargo test --test loom_model && cargo test --test loom_fanout )
cargo test --release --test loom_fanout -- --ignored  # bound-3 sweep (~3 min)
bash ci/scripts/run_chaos_parity.sh                   # C==TS==JVM==Dart byte-diff
python3 tools/guardian/guardian.py selftest           # must bite on poisoned fixtures
```

**Toolchain** (the environment the committed evidence tags): GCC 14.2.0 (Debian
14.2.0-19), Rust 1.98.1, Node v24.19.0, Python 3.12.14, Linux x86_64 (Debian 13,
5.10.134), 2 cores, env label `x86_64-sandbox`; TLC via tla2tools 1.8.0 (sha-pinned
above). **Stranger procedure**: fork, verify toolchains (±minor), run the battery; if a
structural gate fails the port is wrong, if informational numbers differ the hardware
differs — "this is expected and honest" (`docs/WHITEPAPER.md` Appendix A).

## Appendix B — Artifact manifest

| Artifact | Path | Role in this volume |
|---|---|---|
| C kernel (normative) | `core/c/weft.{h,c}` | §2.2, §4.1 — all signatures verbatim |
| C fan-out ring | `core/c/fanout.{h,c}` | §4.2 — layout, protocol, ordering regimes |
| Rust kernel | `core/rust/src/lib.rs` | §2.2, §4.1 — mirror; `Send/Sync` + unsafe contracts |
| Rust fan-out ring | `core/rust/src/fanout.rs` | §4.2 — `Arc<RawRing>`, `&mut self` writer contract |
| TS kernel | `core/ts/weft.ts` | §2.2 (SAB layout, dual-i32 telemetry), §4.1 |
| TS fan-out ring | `core/ts/fanout.ts` | §4.2 — seqcst regime, BigInt64 ctrl |
| FrameCursor (RFC-0008) | `core/c/frame_cursor.{h,c}`, `core/rust/src/frame_cursor.rs` | §3.1 I1 (reset rule), F-series FC1–FC6 |
| TLA+ models | `formal/fanout/`, `formal/reattach/` | §5.2–5.3 — invariants verbatim, TLC stats |
| Loom models | `core/rust/tests/loom_model.rs`, `loom_fanout.rs` | §5.4 — assertions, preemption bounds |
| Litmus catalog + report | `litmus/catalog.yaml`, `litmus/REPORT.md` | §6 — L1–L8 params, matrix, findings |
| Ordering matrices (normative) | `litmus/catalog.yaml` §ordering_matrix, `docs/PORTS.md` §§1–6 | §3.2 |
| Protocol RFCs | `rfcs/0001` (Triad), `rfcs/0004` (fan-out), `rfcs/0006` (reattach), `rfcs/0008` (freshness), `rfcs/0011` (exhaustive proofs) | §3–§6 |
| Constitution & architecture | `docs/PHILOSOPHY.md`, `ARCHITECTURE.md`, `docs/WHITEPAPER.md` | §1–§2 (Laws, tiers, prior measured numbers) |
| Chaos engine + guardian | `core/*/fanout_chaos.*`, `tools/guardian/` | §5.6 |

## Appendix C — Merged contribution coverage

Volume I is compiled from the mainline as of `6a9a4db`, covering all merged contribution
series (the assignment's "other contributors' PRs too"). Provenance of the material this
volume cites:

| PR | Branch / series | Volume I relevance |
|---|---|---|
| #1–#3 | Series 1–3 (npm writer API + heddle hardening; RFC-0004 TS fan-out driver layer; W6 HFT feed workload) | TS driver-layer surface cited in §4.2; F-series ancestry (§6.5); the W6 allocation evidence behind the dual-i32 regime (§2.2) |
| #4 | `contrib/arch-breakthroughs` (maintainer) | RFC-0008 FrameCursor, rLive — §3.1 I1, §6.5 FC-series |
| #5 | `contrib/native-fanout-parity` (Series 4) | §4.2's C/Rust rings, both ordering regimes, the exhaustive Loom model (§5.4), xlang fixtures (§2.3) |
| #6 | `contrib/series-5-6-complete` | VM-port fan-out rings + flight recorder (PORTS §6 rows cited in §2.4) |
| #7 | `contrib/vm-fanout-parity-and-flight-rec` | Kotlin/Swift/Dart ring ports + `.weftrec` v2 (§4.3.3 wire discipline context) |
| #8 | `contrib/wave2-verifiedweft` | VerifiedWeft (RFC-0005) — envelope authentication; Volume II material, boundary noted in §0 |
| #9 | `contrib/nanoseconds-work-order` | The telemetry timestamp work order — ancestry of the dual-i32 regime and AXIOM T discipline (§2.2, §3.3) |
| #10 | `contrib/series6-trust-and-time-travel` | Trust & time-travel (time-travel exception, `docs/PHILOSOPHY.md` §5) — cited in §1.2 scope |
| #11 | `contrib/wave3-runs-everywhere-proven` | RFC-0009 governor + cadence policies + Series-7 D2/D3 wave (governor consumes `claim.dropped`, §3.1 I4; cadence/recycler surfaces deferred to Volume III) |
| #12 | `feat/chaos-torture-and-formal-proofs` (other contributor) | **§5 in full**: the TLA+ models, the four Loom deep-models, the exhaustive-proof CI shards, the chaos engine, the guardian |
| #13 | `feat/cadence-zerogc-lifecycle` (other contributor) | 0-GC lifecycle discipline (§3.1 I5 evidence chain: R8/G4 audits); RN governed UI thread — Volume III |
| #14 | `contrib/hardware-accel` (this author's series) | SIMD batch verification, IPC SHM ring sessions, GPU-resident rings — Volume II; the SHM wire session header builds on §4.3.3's ring bracket discipline |

The boundary is deliberate: hardware acceleration (PR #14) and the presentation/policy
layers (RFC-0009 Series 7, PR #13) constrain — but do not alter — core semantics, and are
documented where they touch Volume I's surface and deferred otherwise.

## Appendix D — Glossary & references

**Terms** (governed vocabulary, `docs/GLOSSARY.md`): *Weft* — the dynamic thread woven
across the static warp (the reactive tree); *heddle* — the loom mechanism that lifts warp
threads so the weft passes (the binding layer); *steward* — lifecycle manager for
off-heap property; *triad* — the 3-buffer/1-atomic ownership protocol; *fan-out ring* —
the RFC-0004 seqlock ring; *bracket* — the invalidate/fill/stamp discipline that makes
overwrites detectable; *null frame* — the valid seq-0 initial state; *stale return* — a
lawful claim of the reader's own previously-released buffer; *telescoping* — the exact
drop-accounting identity; *AXIOM T* — telemetry is not a correctness reference.

**Primary sources** (in-tree, at the cited revision): RFC-0001 (Triad Protocol —
ownership by atomic exchange), RFC-0004 (Multi-Consumer Fan-Out Heddles), RFC-0005
(VerifiedWeft), RFC-0006 (Reattach Policy), RFC-0008 (Freshness Telemetry), RFC-0009
(Freshness Governor), RFC-0011 (Exhaustive State-Space Proofs, Chaos Injection & Crash
Resilience), RFC-0011-ipc-shm-ring-sessions; `docs/PHILOSOPHY.md` (the Four Laws);
`ARCHITECTURE.md` (tiers, seams, freeze contract); `docs/PORTS.md` (normative ordering
maps); `docs/WHITEPAPER.md` v1.0.4 (the Phase-0/1 measured baseline this volume
inherits); `litmus/README.md` and `litmus/catalog.yaml` (the conformance contract);
`patches/PATCHES-WAVE3.md` / `PATCHES-WAVE4.md` (series evidence).

**Prior art**: graphics triple buffering — the ownership-exchange pattern the Triad
Protocol formalizes ("shipped in graphics drivers for decades; Weft's contribution is
specifying it for UI state," RFC-0001 §9); seqlocks (the classic reader-writer counter
bracket the fan-out ring adapts with bounded, counted retries); TLA+/TLC (Lamport);
`loom` (Rust bounded model checking); Jepsen's deterministic schedulers and nemesis
taxonomy (the chaos tier's prior art); Apache Arrow (the frozen-envelope
forward-compatibility model); TSAN/ASAN (the sanitizer leg). *Volume II will carry the
hardware-acceleration prior art (Metal/Vulkan/WebGPU shared memory, SIMD multi-buffer
hashing) in full.*






