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
| Divergence note | Already documented in WHITEPAPER §7.1: "SC Atomics ≥ C relaxed — where the web port is *stronger*, and why that is not cheating." |
| SAB/COOP-COEP | Cited per §8.2: SAB requires COOP/COEP; default web path is one-copy Transferable. Each binding cites §8.2 where shared arrays are involved. |
