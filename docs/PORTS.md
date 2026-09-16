# PORTS — Memory-Model Mapping Tables

> **Normative for all Phase 4 ports (WO-P4-PORTS T1).** Each port inherits correctness from the frozen C reference *through* an explicit mapping of every atomic operation to the target language's primitives. A port whose exchange does not map to a single RMW is not a port; it is a new protocol and needs an RFC.

---

## 1. Kotlin/JVM

| Row | Content |
|---|---|
| `latest` exchange | `AtomicReference<Int>.getAndSet(newValue)` — single RMW. JVM is SC-only; SC ≥ C AcqRel (strictly stronger). Legal per WO-P4 decision 2. Ordering constant: `SeqCst` (JVM default; no weaker mode available without VarHandle on API 33+). |
| `w_work` / `r_work` | `volatile var` (JVM happens-before via volatile read/write). Thread-private by contract; volatile for visibility to debug-view accessor. Refresh path: assigned from `getAndSet` return value (same as C `w_work = old`). |
| `revoked` | `AtomicBoolean`. Writer load: `get()` (plain — advisory per AXIOM T). Releaser store: `set(true)` — JVM volatile semantics (Release-equivalent under SC). |
| `epoch` ACK | `AtomicInteger.getAndAdd(1)` — single RMW, SC ordering (≥ AcqRel). The ACK publishes "I will never write again." |
| Telemetry counters | `AtomicLong.getAndAdd(1)` — SC (advisory per AXIOM T; post-join reads only). |
| Envelope decode | `ByteBuffer.wrap(buf).order(LITTLE_ENDIAN)` — `getInt()`, `getShort()`, `getLong()`. Decode rules §4.2 cited: payload offset = `header_size`, not 16. |
| I6 mapping | Handshake unchanged: `revoke()` → `revoked.set(true)`. Writer checks at top of `publish()`. `epoch.getAndAdd(1)` ACK. Reclaim polls `epoch.get()`. Reclaim action: no `free()` on JVM — buffer marked `released`; further access returns zero/false. L7-style poison checks become state assertions (`assertReleased()`). |
| Divergence note | JVM SC ≥ C AcqRel. The port is *stronger* than the C reference, not weaker. No weaker-ordering heroics to "match" C. Per §7.1 (WHITEPAPER): "the web port is stronger, and why that is not cheating." Same logic extends to JVM. |

---

## 2. Swift/iOS

| Row | Content |
|---|---|
| `latest` exchange | `ManagedAtomic<UInt32>.exchange(newValue, ordering: .acquiringAndReleasing)` — single RMW. Dependency: `apple/swift-atomics` (WO-P4 decision 3). Ordering `.acquiringAndReleasing` maps exactly to C `AcqRel`. |
| `w_work` / `r_work` | Plain `var` (instance property; thread-private by contract). For debug-view visibility, read via `Atomic<UInt32>` if cross-thread access is needed; otherwise plain `var` with a documented advisory note. Refresh path: assigned from `exchange` return value. |
| `revoked` | `ManagedAtomic<Bool>`. Writer load: `.load(ordering: .relaxed)`. Releaser store: `.store(true, ordering: .releasing)` — maps to C `Release`. |
| `epoch` ACK | `ManagedAtomic<UInt32>.wrappingIncrement(by: 1, ordering: .acquiringAndReleasing)` — maps to C `fetch_add(AcqRel)`. |
| Telemetry counters | `ManagedAtomic<UInt64>.wrappingIncrement(by: 1, ordering: .relaxed)` — advisory per AXIOM T. |
| Envelope decode | Manual LE decode via `UnsafeRawPointer.load(as: UInt32.self)` with `endianness` parameter, or `Data.withUnsafeBytes { ptr in ptr.loadUnaligned(as: UInt32.self) }` + manual byte-swap if non-LE platform. Decode rules §4.2 cited. |
| I6 mapping | Handshake unchanged. Reclaim action: no `free()` under ARC — buffer reference set to `nil` (ARC releases); poison checks become `Precondition` assertions on the buffer state. L7 semantics cited, not re-run. |
| Divergence note | `.acquiringAndReleasing` is the exact equivalent of C AcqRel — no divergence. Swift-atomics dependency declared in README. |

---

## 3. Dart/Flutter

| Row | Content |
|---|---|
| `latest` exchange | **Plain field assignment** — NOT a single RMW. Dart isolates share no memory; the language has no atomics. The pure-Dart kernel is a **single-isolate reference implementation**: exchange maps to `latest = w_work` (plain assignment), valid only because writer and reader run on one event loop (WO-P4 decision 4). |
| `w_work` / `r_work` | Plain `int` fields. No synchronization needed (single isolate). Refresh path: `w_work = oldLatest` after assignment. |
| `revoked` | Plain `bool`. No ordering needed (single isolate). |
| `epoch` ACK | `epoch += 1` (plain increment). No ordering needed. |
| Telemetry counters | Plain `int` fields. Advisory per AXIOM T (though single-isolate means no concurrent access). |
| Envelope decode | `ByteData.view(buf).getInt32(offset, Endian.little)` — `Endian.little` for all accessors. Decode rules §4.2 cited: payload offset = `header_size`, not 16. |
| I6 mapping | Handshake unchanged. Reclaim action: no `free()` under Dart GC — buffer set to `null`; poison checks become state assertions (`expect(released, isTrue)`). The forward interface for production usage goes through `dart:ffi` to the C kernel — **specified, not implemented** (WO-P4 decision 4). |
| Divergence note | **Single-isolate reference; no cross-thread ordering claims transfer from the C/Rust proof.** The pure-Dart kernel is a protocol reference implementation valid for single-threaded usage (e.g., a Worker isolate that runs both writer and reader on its own event loop). Production Flutter usage goes through `dart:ffi` to the C kernel. This is the honesty load-bearing wall of the Dart port. |

---

## 4. TypeScript Heddles (wraps existing TS kernel — no kernel changes)

| Row | Content |
|---|---|
| `latest` exchange | `Atomics.exchange(Int32Array, index, value)` — single RMW, SC (the web gives no weaker choice). SC ≥ C AcqRel. Already proven in Phase 0 litmus (24/24) and Phase 1 bench (15/15). |
| TS kernel | Unchanged from Phase 0. The four Heddle bindings (`@weft/react`, `@weft/svelte`, `@weft/vue`, `@weft/react-native`) wrap the kernel's `Weft` class — no kernel modifications. |
| Telemetry counters (2026-09-16 regime) | Hot path: dual-i32 halves (`Atomics.add` on the Int32 view, carry into the hi half once per 2³² increments) + Number step counters + dual-u32 canary — **allocation-free** (the BigInt regime boxed ~90 B/publish, measured by the W6 feed bench and closed by this change: `demos/web/evidence/feed-gc-bench.log`, direct per-cycle numbers in `demos/web/evidence/telemetry-microbench.log`). Cold path: the BigInt64 view composes the exact u64 (`tPublish()/tClaim()/tDrop()` still return bigint — the C-port u64 parity is preserved). Stated divergence: the two-step increment is not atomic-as-u64 (transient (new lo, old hi) window during carry, once per 2³² increments) — advisory only, AXIOM T; the C kernel's single u64 `fetch_add` remains the atomic gold standard JS Atomics cannot express. |
| Divergence note | Already documented in WHITEPAPER §7.1: "SC Atomics ≥ C relaxed — where the web port is *stronger*, and why that is not cheating." |
| SAB/COOP-COEP | Cited per §8.2: SAB requires COOP/COEP; default web path is one-copy Transferable. Each binding cites §8.2 where shared arrays are involved. |

---

## 5. Fan-out driver layer (RFC 0004 — `core/c/fanout.{h,c}`, `core/rust/src/fanout.rs`, `core/ts/fanout.ts`, `core/kotlin/Fanout.kt`, JNI/Dart-FFI bridges)

The fan-out ring is DRIVER LAYER, not kernel — but it is a shared-memory
protocol with its own ordering matrix, so it gets a PORTS row of its own.
The ring layout is byte-compatible across the native languages (the interop
contract, proven by `fixtures/xlang-fanout/`); the JVM carries a semantics
port and the mobile runtimes reach the C ring through FFI bridges.

| Row | Content |
|---|---|
| Ring layout | One region: `[latestSeq u64][publishes u64][slotSeq[M] u64][M payload slots]`, payload bytes % 4 == 0, `ring_bytes = 16 + 8M + M*payload_bytes`. Identical formula in TS (`core/ts/fanout.ts`), C (`weft_fanout_ring_bytes`), Rust (`fanout::ring_bytes`). |
| `latestSeq` publication | TS: `Atomics.store(BigInt64Array)` — SeqCst (the web gives no weaker choice). C/Rust (default regime): Release store; readers load Acquire. The publication point pairs Release→Acquire for the happy-path happens-before. |
| Invalidate-before-fill (FI1) | TS: SeqCst store. C/Rust: **SeqCst store + SeqCst fence** before the fill cursor is returned — property P1 (no fill word may become visible before the invalidate stamp; a Release store alone orders PRIOR accesses, not subsequent ones). |
| Payload words | TS: plain Float32 writes (bracket discipline — the documented stance of `core/ts/fanout.ts`). C/Rust: **Relaxed atomic u32 word** stores/loads — race-free in the strict model at zero x86 cost; this is also what keeps the C ring TSAN-clean (a plain payload access concurrent with the opposing side is a race by definition, whatever the brackets). |
| Revalidation (P2) | TS: SeqCst loads around the copy. C/Rust: Acquire loads + **one SeqCst fence between copy and revalidation** — if the copy observed any overwrite word, the revalidation load must observe the invalidate-or-newer stamp. |
| A/B regime | C compiles with `-DWEFT_FANOUT_SEQ_CST=1` for the TS-equivalent all-SeqCst stamps; both regimes are torture-gated (`litmus/evidence/fanout/`). The fenced acq/rel default measured ~11% higher publish throughput under identical 4-reader contention (x86_64 sandbox, environment-tagged). |
| Divergence note | The C/Rust ports are *weaker-ordered but fenced* where the TS port is SeqCst-everything — the reverse of the kernel ports' "stronger, not cheating" stance, and for the same reason: the ordering each needs is the ordering each pays for, and the bracket proof is carried by the two fences (P1/P2), Loom's exhaustive model (Rust), and the TSAN/ASAN/torture gates (C). |
| JVM port (`core/kotlin/Fanout.kt`) | SEMANTICS port, not a byte-layout port (the JVM has no SharedArrayBuffer — stated in the file header): ctrl is an `AtomicLongArray`, slots are heap `FloatArray`s. Stamp accesses are `AtomicLongArray` volatile ops (acquire/release per variable under JSR-133 — the C default regime, and stronger than plain fields); payload words are plain float elements (JLS §17.7: 32-bit accesses do not tear) bracketed by the stamps. The bracket's visibility rides the volatile barriers (JSR-133 cookbook: trailing StoreLoad after the invalidate store, leading LoadLoad before the revalidation load) — the same class of implementation-backed reasoning the TS port's seqcst stance carries. Cross-language interop is the JNI path below, not this port. |
| Android JNI (`weft_jni.c` → `core/c/fanout.c`) | The C ring itself — every ordering property is the C row's, unchanged; the bridge adds only handle passing, direct-ByteBuffer cursors, and per-reader record accessors (the record is reader-owned and stable until that reader's next claim, so claim/claimFresh/claimDropped read back one consistent claim across three JNI transitions). No JVM callbacks from C → no thread attachment anywhere. |
| Flutter/Dart FFI (`packages/flutter_weft`) | The C ring through `dart:ffi` — ordering is the C row's; Dart adds no shared state (handles C-allocated/C-freed, claim record and copy buffer are native memory). Cross-isolate consumers pass the reader handle as its raw address; the writer stays single-isolate by contract (D-14). |

