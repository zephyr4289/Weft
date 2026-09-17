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


---

## 6. VM-port fan-out rings (RFC 0004 — Series 5: `core/kotlin/Fanout.kt`, `core/swift/Fanout.swift`, `core/dart/fanout.dart`)

The three VM-port kernels (Kotlin §1, Swift §2, Dart §3) ship their own
RFC-0004 rings alongside the C/Rust/TS rings of §5 — same byte-compatible
layout (the interop contract of §5), same protocol, port-specific memory
primitives. The kernel ports stay 1:1 and frozen; these are driver-layer
modules, one per port, each with its own F-series battery and a real
multi-thread torture where the platform can express one.

| Port | Ring memory | Ordering regime (the port's §5 mapping) | Divergence / boundary notes |
|---|---|---|---|
| Kotlin/JVM | ONE direct `ByteBuffer` (`allocateDirect`, LE) — JNI-attachable by `weft_fanout_attach_writer`/`NewDirectByteBuffer`; the JVM analog of posting the TS SAB | The C regime mapped to VarHandle access modes: begin() invalidate = `setVolatile` (SC store) + `VarHandle.fullFence()` (P1); publish() stamps = `setRelease`; claim() stamps = `getAcquire`; payload words = `getOpaque` (the JVM analog of C relaxed u32 — race-free by construction); one `fullFence()` between copy and revalidation (P2) | `publishes` telemetry add is `getAndAdd` (SC — the JVM exposes no relaxed RMW; advisory per AXIOM T, declared divergence). Platform boundary: VarHandle requires JVM 9+ / Android API 33+; below 33 the ring routes through `core/kotlin/FanoutCompat.kt` (`WeftFanoutFactory` gate) — AtomicLongArray SC stamps (volatile get/set = SeqCst, the Swift port's regime) + plain bracket-discipline payload (the TS port's stance), same protocol, same F-series gates (`FanoutCompatTest.kt` matrix runs BOTH regimes on every host); the JNI road to `core/c/fanout.h` (same bytes) remains the cross-port interop path. Torture: writer + 3 reader threads, 100k mixer-validated frames (FanoutTest.kt F10). |
| Swift | ONE 64-byte-aligned `UnsafeMutableRawPointer` region; ctrl as `UInt64.AtomicRepresentation`, payload as `UInt32.AtomicRepresentation` — a C peer or any port's bytes attach with zero copy (`bindMemory` for foreign rings) | The honest Swift hybrid: stamps SEQUENTIALLY CONSISTENT via `UnsafeAtomic` (swift-atomics exposes no standalone fence, so the C port's P1/P2 fence pairs cannot be expressed — SC stores are also Releases, SC loads also Acquires, and the SC access carries the bracket duty on every targeted implementation; the TS port's regime, stated as such); payload words RELAXED-ATOMIC u32 on both sides (the C port's stance) | Stronger than the TS port on payload (atomic words vs plain Float32 stores), differently-proofed than C on stamps. Publishes telemetry add is relaxed (`loadThenWrappingIncrement`). Torture: writer + 3 concurrent DispatchQueue readers, 100k mixer-validated frames (FanoutTests.swift F10). |
| Dart/Flutter | ONE `Uint8List` region, all accessors `Endian.little`; `WeftFanoutReader(bytes)` attaches to any port's bytes (FFI `Pointer<Uint8>.asTypedList` or a copy), geometry-validated | SINGLE-ISOLATE REFERENCE — the honesty load-bearing wall of the Dart port (§3): plain ByteData accesses, valid only because writer and readers run on one event loop; the bracket discipline is retained verbatim (an await between copy halves is legal Dart and the tear freedom survives); no cross-thread ordering claims transfer | The copy is an explicit LE word loop (word VALUES, not byte reinterpretation — host-endianness-proof). Cross-thread fan-out on Flutter goes through dart:ffi to the C ring (§5) — same bytes. Float users: `wordToFloat(view()[i])` (the bit-pattern reinterpretation, zero-alloc scratch). Battery: fanout_test.dart (no torture — declared: the concurrent gates live in C/Kotlin/Swift and the FFI road). |

Flight recorder (`tools/weft-fanout-rec`, .weftrec v2 — FORMATS.md §1.5): the
capture/replay consumer named in RFC 0004's motivation, attaching to any
port's ring over POSIX shm. Evidence: `litmus/evidence/fanout/flight-recorder.log`
(both C ordering regimes + ASAN + TSAN green).

Structural enforcement: `tools/port_validator.py` carries a fan-out rule
pack per VM port (existence + spec-citing header + the ordering markers of
this section + the shared begin/publish/claim/view/stats/ring-bytes API
surface); `ci/scripts/run_binding_parity.sh` hashes the two new mirror
pairs (`core/kotlin/Fanout.kt` ↔ `android/weft-core/.../Fanout.kt`,
`core/dart/fanout.dart` ↔ `packages/flutter_weft/lib/src/reference/fanout.dart`).


---

## 7. Freshness governor (RFC 0009 — `core/c/governor.{h,c}`, `core/rust/src/governor.rs`, `packages/core/src/governor.ts`)

The governor is PURE CONTROL LOGIC — driver layer with no memory model at
all (no shared state, no atomics, no kernel contact; it reads a u32 the
consumer hands it). Its cross-language contract is therefore not an ordering
matrix but an ARITHMETIC matrix: the same (framesBehind, nowMs) trace must
produce the identical action sequence in every language.

| Row | Content |
|---|---|
| Action set (closed) | `FastPath` (behind <= fast_path_behind, default 1) · `Skip(n)` (<= skip_behind, default 4; n = behind - fast_path_behind, counted in `decided_drops` — Law 4) · `Snapshot` (<= snapshot_behind, default 16) · `Reseed` (rate-limited: one per `reseed_cooldown_ms`, default 250; a suppressed Reseed degrades to `Snapshot` — the documented fallback). A fifth action is a new RFC. |
| Kind discriminants | PROTOCOL values 0/1/2/3 (TS `GovernorActionKind`, C `weft_gov_kind_t`, Rust `GovernorActionKind`) — G5 packs them into trace bytes; renumbering is a protocol break. |
| Time injection | `step(framesBehind, nowMs)` takes the clock as a PARAMETER (TS `number`, C `i64`, Rust `i64`) — step is a pure function of (behind, now_ms) + internal state, which is what makes the G5 trace parity deterministic across languages. Callers pass any monotonic ms clock. |
| Zero allocation (G4) | C: no `malloc` anywhere in the path (struct-resident state; identity-stable action record — the `weft_frame_cursor_t` pattern). Rust: `Copy` action returned by value, `&mut self`. TS: state is 4 counters + 1 timestamp; `step()` returns the governor's OWN pre-allocated action object, mutated in place (`act`), never a fresh allocation. |
| Input source | ANY monotonic per-consumer staleness count: `weft_frame_cursor_update()` frames_behind (triad kernel), or a fan-out reader's `claim.dropped` (§5 rings — same semantics, per RFC-0008). Composed by the app per RFC-0009's open-question lean: the governor never touches a Weft, a ring, or a Triad. |
| Conformance | G1 ladder (behind 0..64 -> documented action), G2 monotone, G3 reseed flap (10k spikes, <= ceil(N/cooldown) Reseeds, >= cooldown spacing), G3b suppressed->Snapshot, G4 zero-alloc, G5 parity — one deterministic xorshift32 trace (04-LITMUS §0.2), three emitters, byte-compared (`fixtures/xlang-governor/`). |
| Demos wiring | `demos/web/src/components/FeedFanoutViews.tsx` — each fan-out view owns its governor: `claim.dropped -> gov.step() -> decideDraw()` (the shared policy module `demos/web/src/modes/governorPolicy.ts`); the measured savings proof is `demos/web/scripts/governor_bench.ts` (B4 display-adversarial matrix, naive vs governor: saved memcpy + saved fences with a per-row convergence gate). |

Divergence notes: none — the three implementations are arithmetic-identical
by construction (same thresholds, same cooldown comparison, same fallback),
and G5's byte comparison is the standing proof. The consumer-side draw
policy (what each app DOES with an action) is deliberately NOT ported —
it is app policy by RFC-0009's "advisory only" lean; the demo module is a
reference implementation, not a contract.

---

## 8. VM-port VerifiedWeft (RFC 0005 — Series 6: `core/kotlin/Verified.kt`, `core/swift/Verified.swift`, `core/dart/verified.dart`)

RFC 0005's authenticated frames reached the three canonical kernels in PR
#6 (C, Rust, TS — wire format, key schedule, and result codes all byte- and
number-identical). Series 6 completes the six-port story: every VM port
carries the same `deriveKey`/`VwSigner`/`VwVerifier`/record-codec/batch
surface, fed by the shared fixture (`fixtures/xlang-verifiedweft/hmac-vectors.json`),
so a record produced by any port verifies in any other bit-exactly. The HW
story is per-port honest — each port names its accelerator road explicitly
instead of papering over the difference:

| Port | HMAC engine | HW acceleration road (Series 6) | Declared divergences / boundaries |
|---|---|---|---|
| Kotlin/JVM | `javax.crypto.Mac("HmacSHA256")` | THE PLATFORM IS THE ACCELERATOR: OpenJDK's OpenSSL-backed provider (SHA-NI on x86-64) / Android's Conscrypt-BoringSSL (ARMv8 CE). One `init` = the key schedule; N `doFinal` calls reuse it (pre-keyed by construction) | `vwCtEq` delegates to `MessageDigest.isEqual` (the platform's content-independent compare — the same platform-delegation story as `Mac`). Unsigned geometry decode via Long math (hostile high-bit `payload_len` lands in `ERR_SHORT`, never wraps — pinned by V5 in every port). Battery: VerifiedTest.kt, 8/8 on JVM 21/kotlinc 2.0.21 (standalone kotlinc + junit-console; android-packages gradle CI is the cover). |
| Swift | `CryptoKit HMAC<SHA256>` | THE PLATFORM IS THE ACCELERATOR: CoreCrypto compiles SHA-256 to ARMv8 CE (FEAT_SHA256) on Apple silicon / SHA-NI on Intel Macs | CryptoKit exposes no streaming HMAC — `VwSigner`/`VwVerifier` hold the `SymmetricKey` and pay CoreCrypto's per-call pad derivation (cheap because the SHA itself is hardware); the API contract mirrors the other ports, the optimization boundary is declared here. `vwCtEq` is a manual constant-time double-walk (no public platform compare). Battery: VerifiedTests.swift (XCTest — apple-packages CI; CryptoKit is Apple-only, so not compilable on the Linux shard runner). |
| Dart/Flutter | PURE DART (this file) | THE HONESTY WALL: no HW road — dart:io has no HMAC, package:crypto is itself pure Dart; the zero-dep rule of this port (frozen pubspec) costs nothing in honesty. This port is the six-port SCALAR REFERENCE: byte-identical on the wire, slower on the CPU | HW rates come via dart:ffi to the C verifier (`packages/flutter_weft/src/weft_ffi.dart` is the existing road). `vwCtEq` is a manual constant-time double-walk. Battery: verified_test.dart (flutter-packages CI; core reference additionally runs under plain `dart` — 59/59 on Dart 3.13, evidence `litmus/evidence/verified/dart-vseries.log`). |

Structural enforcement (same shape as the fan-out packs): the Series-6
`port_validator.py` rule pack checks each VM port's verified module for the
domain-separation marker (`Weft-VerifiedWeft-v1:key`), the envelope-length
contract, and the shared API surface (deriveKey/signer/verifier/ctEq/
recordEncode/batchDecodeVerify/err-tag) — case- and underscore-insensitive
so the check pins the SYMBOL, not the port's spelling convention.
`run_binding_parity.sh` hashes the two new mirror pairs
(`core/kotlin/Verified.kt` ↔ `android/weft-core/.../Verified.kt`,
`core/dart/verified.dart` ↔ `packages/flutter_weft/lib/src/reference/verified.dart`).
CI: `run_verifiedweft_shard.sh` now carries seven gates (C ×2 regimes,
Rust, TS, xlang, Kotlin-when-kotlinc, Swift/Dart declared to their package
workflows) — the shard's declared-skip discipline matches the repo's
per-port honesty culture.
