# WO-P4-PORTS — Phase 4: Source-Only Platform Implementations (Kotlin / Swift / Dart / TS-Heddles)

```
Directive:   weft-phase4-directive stream
Doc ID:      WO-P4-PORTS
Version:     v1.0
From:        Staff adjudication
To:          Executor
Status:      ACTIVE — begins after T0 (WO-P2-CLOSURE batch B1–B4)
Depends:     Phase 2 CLOSED (tools + v1.0.2), Kernel FROZEN, contracts v1.3, whitepaper v1.0.2
Budget:      ~37h ≈ 4.6 engineer-days (incl. the ~3h B1–B4 batch)
Kernel:      FROZEN — zero API changes this phase. Ports copy the frozen C reference
             semantics; any doubt = RFC, not improvisation.
```

---

## 0. Authority, context, order of work

Roadmap §4 contract: production-ready source for Kotlin/Android, Swift/iOS, Dart/Flutter,
and the TypeScript Heddle bindings — **structurally validated, not compiled, not
benchmarked in-sandbox** (no toolchain). Each port is a Phase 6+ deliverable waiting for
the constraint to lift. The whitepaper (§8.6) already publishes this honestly; the ports
must not retroactively contradict it.

Roadmap success criteria (mechanical, restated):
1. Every source file passes its structural validator (API surface matches spec, no missing
   functions, panic shielding on every JNI entry, SAFETY comment on every unsafe block).
2. Every source file carries a one-paragraph "why exists" header citing the spec section
   that justifies it.
3. Each port README is marked `STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION`.

**Order of work:** (T0) WO-P2-CLOSURE batch B1–B4 — the citable artifact and the tools
evidence must be final before ports fork off them. Then T1 mapping tables — the gate for
everything else. No port code before its table exists.

**Standing decisions for this phase (staff-ruled, implement as specified):**

1. **The memory-model mapping table is the load-bearing artifact.** Each port inherits
   correctness from the frozen C reference *through* an explicit mapping of every atomic
   operation to the target language's primitives. A port whose exchange does not map to a
   single RMW is not a port; it is a new protocol and needs an RFC.
2. **JVM is SC-only, and that is legal.** `AtomicReference.getAndSet` and `VarHandle`
   volatile modes are sequentially consistent — strictly stronger than the C AcqRel
   exchange. This is the TS §7.1 divergence logic extended: stronger ordering can only add
   safety, never remove it. The port docs say so; no weaker-ordering heroics to "match" C.
3. **Swift uses apple/swift-atomics** (`ManagedAtomic`, ordering `.acquiringAndReleasing`
   for both exchanges). Declare the dependency in the port README; do not hand-roll
   atomics from intrinsics.
4. **Dart cannot express this protocol, and the port must say so.** Dart isolates share no
   memory and the language has no atomics. The pure-Dart kernel is therefore a
   **single-isolate reference implementation**: exchange maps to plain field assignment,
   valid only because writer and reader run on one event loop. Its docs and README carry
   the banner: *"single-isolate reference; no cross-thread ordering claims transfer from
   the C/Rust proof."* Production Flutter usage goes through `dart:ffi` to the C kernel —
   documented in the port README as the **forward interface (specified, not
   implemented)**, the same pattern as the shm-name forward interface in FORMATS.md.
   Do not write the FFI bridge itself; guessing ABIs without a compiler is how phantom
   APIs get born.
5. **I6 on GC/ARC platforms maps as: handshake unchanged, reclaim action substituted.**
   `revoked` flag, per-publish check, ACK (`epoch.fetch_add`), bounded reclaim poll — all
   preserved exactly. What changes is only the reclaim *action*: there is no `free()` on
   GC platforms, so "poison/verify" maps to logical release + state verification (buffer
   marked unusable; L7-style poison checks become state assertions). The mapping table
   carries an explicit I6 row per language; litmus L7 semantics are cited, not re-run.
6. **Steward/Heddle UI layers validate structurally, not behaviorally.** No Compose,
   SwiftUI, or Flutter runtime exists in-sandbox. The validator checks presence, declared
   API names, and spec-section citations for these files — nothing deeper. No behavior
   claims.
7. **No new litmus cells for ported languages.** Litmus remains C/Rust/TS (24 cells).
   Port-language litmus is a Phase 6+ deliverable; do not pre-register cells you cannot
   run.
8. **No performance claims anywhere in Phase 4 artifacts.** Extends whitepaper §8.6:
   ports are source-only, unbenchmarked; Phase 6+ replaces this with real-device numbers.
9. **AXIOM T applies to ports.** Telemetry counters are advisory; no port logic branches
   on them (contracts v1.3).
10. **LOC budgets are ±20% advisory; misses are declared** (standing rule, twice-proven
    necessary).

## 1. Tasks

### T0 — WO-P2-CLOSURE batch B1–B4 (~3h)

Exactly as specified in WO-P2-CLOSURE §3. Exit: v1.0.2 render-verified; contracted 2×30s
soak green; Rust frame_count patched (or repro adjudicated); Rust live-dump delivered.
This batch closes Phase 3 and Phase 2.

### T1 — Memory-model mapping tables FIRST — gate for T3–T6 (~2h)

`docs/PORTS.md`, one section per target language (Kotlin, Swift, Dart) plus a TS-bindings
summary. Each native-language section is a table with these rows, pinned:

| Row | Content |
|---|---|
| `latest` exchange | exact type + call (single RMW), ordering constant, strictly-stronger argument where SC |
| `w_work` / `r_work` | representation (thread-private words), refresh path |
| `revoked` | type + store/load orderings (Release store / Relaxed load) |
| `epoch` ACK | fetch-add equivalent + ordering (AcqRel) |
| Telemetry counters | advisory representation; AXIOM T restated |
| Envelope decode | little-endian decode path (ByteBuffer LITTLE_ENDIAN / manual LE decode / ByteData Endian.little); decode rules §4.2 cited verbatim |
| I6 mapping | handshake unchanged; reclaim-action substitution per standing decision 5 |
| Divergence note | §7.2 divergence rule applied to this language |

Self-certify before port code: each T3–T5 port's exchange site must be traceable to its
table row. Acceptance includes a table-vs-code conformance audit — a code path not in the
table is a finding.

### T2 — Structural validator (~3h)

`tools/port_validator.py` (Python, extends the 05-CONTRACTS tooling pattern): one driver,
per-language rule packs. Checks per target:

- **API surface**: every public kernel symbol in the frozen C reference has a 1:1
  counterpart with matching arity (`weft_init / publish / claim / revoke / destroy /
  debug_view / envelope decode`). Pinned symbol table lives in the validator.
- **Exchange-site markers**: each publish/claim site contains the mapped call from the T1
  table (e.g. Kotlin `getAndSet`, Swift `.exchange(..., ordering: .acquiringAndReleasing)`).
- **Panic shielding (4a)**: every JNI entry catches Throwable and returns an error code;
  no exception crosses the FFI boundary.
- **SAFETY comments**: every `unsafe` block (4b) carries a SAFETY justification line.
- **Headers**: every file has the "why exists" paragraph citing a spec section (roadmap
  criterion 2).
- **Banners**: each port README carries the STATUS: SOURCE-ONLY banner (criterion 3);
  Dart README additionally carries the single-isolate banner (decision 4).

Output: JSON-line report per run (one object per target, checks array, pass/fail), exit
0/1. The validator failing is a RED per 07-ACCEPTANCE §6 — report it, never weaken a check
to make it pass. Weakening a validator check to go green is the Phase 4 equivalent of
trimming tails.

### T3 — 4a Kotlin/Android (~8h, ~4,700 LOC ±20%)

- Pure Kotlin/JVM kernel (no Android dependency): Triad exchange per T1 table
  (AtomicReference/VarHandle), envelope codec (ByteBuffer, LITTLE_ENDIAN, decode rules
  §4.2), I6 handshake per decision 5, debug view (semver discipline: mirror the C/Rust
  surface from WO-P2 T1).
- Steward: ViewModel integration (source-only).
- Heddle: `Modifier.weftDraw` extension (source-only; cite whitepaper §8.1 Compose
  draw-phase references and Q1/Q3 open questions where relevant).
- JNI bridge to the litmus-passing C kernel (roadmap contract): panic-shielded entries,
  SAFETY-equivalent comments, buffer ownership documented against I6.
- Port README: why-exists, STATUS banner, JVM-SC divergence note (decision 2).

### T4 — 4b Swift/iOS (~7h, ~3,500 LOC ±20%)

- Pure Swift kernel via swift-atomics (decision 3); envelope codec with explicit
  little-endian decode; I6 per decision 5; debug view mirror.
- Steward: @StateObject integration (source-only).
- Heddle: WeftCanvas — Canvas + CADisplayLink at 60 Hz, MTKView path at 120 Hz (cite
  whitepaper §8.3 exactly; the two-path honesty is already published, keep it).
- Port README: why-exists, STATUS banner, swift-atomics dependency declared.

### T5 — 4c Dart/Flutter (~6h, ~3,050 LOC ±20%)

- Pure Dart kernel: single-isolate reference per decision 4; envelope codec
  (ByteData, Endian.little); I6 logical-release mapping; debug view mirror.
- Steward: StatefulWidget integration (source-only).
- Heddle: CustomPainter (source-only).
- Forward-interface section in README: `dart:ffi` binding to the C kernel ABI —
  specified, not implemented (decision 4).
- Port README: why-exists, STATUS banner, **single-isolate banner** (decision 4) — this
  is the honesty load-bearing wall of the Dart port; a reader must not be able to mistake
  the pure-Dart kernel for a concurrency proof.

### T6 — 4d TypeScript Heddles (~5h, ~1,500 LOC ±20%)

Wraps the existing TS kernel (Phase 0e) — no kernel changes:

- `@weft/react` — WeftCanvas component; mounts a Worker; transfers OffscreenCanvas.
- `@weft/svelte` — Svelte action equivalent.
- `@weft/vue` — Vue 3 composable.
- `@weft/react-native` — Reanimated SharedValue integration (cite whitepaper §8.4:
  Reanimated is the prior art; RN ports last in the roadmap).

Each binding cites §8.2 (SAB/COOP-COEP) where shared arrays are involved. Structural
validation only (import graph + API names + headers).

### T7 — Validation run (~1h)

Run the T2 validator over all four targets. All green (exit 0), JSON-line report archived
into the evidence tree. Any RED: fix the port or file the finding — per 07-ACCEPTANCE §6.

### T8 — `Weft-Phase4-Ports-Report.pdf` + doc pointers (~2h)

Same typeset pipeline (pandoc + tectonic; PDF-DEFERRED fallback rule applies). Contents:
T1 tables summary + pointer to docs/PORTS.md; per-port design notes (the mapping decisions
that were not mechanical); validator results; LOC vs budget table; the standard honesty
section (what a stranger can and cannot conclude from source-only ports — reuse §8.6
language); deviations field **mandatory**. Update repo README / docs index to reference
the four ports.

## 2. Rules (non-negotiable)

1. Kernel FROZEN. Zero API changes. Doubt = RFC.
2. Mapping tables precede port code (T1 gate). Table-vs-code conformance is audited at
   acceptance.
3. No performance claims in any Phase 4 artifact. No benchmark runs. No "expected to be
   as fast as" prose.
4. AXIOM T: telemetry advisory; no logic branches on counters.
5. Validator checks are canon. Weakening a check to pass = RED.
6. LOC budgets ±20% advisory; every quantitative miss declared in the deviations field
   (standing rule).
7. Failure protocol: 07-ACCEPTANCE §6 — report the RED.

## 3. Success criteria (mechanical)

- [ ] T0 batch B1–B4 evidence complete (Phase 2 + Phase 3 close first)
- [ ] docs/PORTS.md mapping tables exist for Kotlin, Swift, Dart + TS-bindings summary
- [ ] Every exchange site in every port traces to its T1 table row
- [ ] Structural validator green on all 4 targets; JSON-line report archived
- [ ] Every file: "why exists" header citing a spec section
- [ ] Every port README: STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION
- [ ] Dart README: single-isolate banner present
- [ ] JNI entries panic-shielded; every unsafe block has a SAFETY comment
- [ ] Zero performance claims across all port artifacts
- [ ] Report PDF typeset; deviations field filled

## 4. Exit checklist

- [ ] T0 B1–B4 closed
- [ ] T1 tables self-certified before port code
- [ ] T2 validator committed before or with the first port (not retrofitted after)
- [ ] T3 Kotlin port complete + README
- [ ] T4 Swift port complete + README
- [ ] T5 Dart port complete + README + forward interface
- [ ] T6 four TS bindings complete
- [ ] T7 validation run archived
- [ ] T8 report + doc pointers + deviations

## 5. Budget

| Task | Estimate |
|---|---|
| T0 WO-P2-CLOSURE batch B1–B4 | 3h |
| T1 mapping tables (gate) | 2h |
| T2 structural validator | 3h |
| T3 Kotlin/Android (~4,700 LOC) | 8h |
| T4 Swift/iOS (~3,500 LOC) | 7h |
| T5 Dart/Flutter (~3,050 LOC) | 6h |
| T6 TS Heddles (~1,500 LOC) | 5h |
| T7 validation run | 1h |
| T8 report | 2h |
| **Total** | **~37h ≈ 4.6 engineer-days** |

Total port LOC contract: ~12,750 ±20% advisory (roadmap §4 figures, binding).

## 6. Out of scope

Compilation of Kotlin/Swift/Dart in-sandbox (no toolchain — roadmap), on-device or
emulator tests, litmus cells for port languages (Phase 6+), the `dart:ffi` bridge body
(forward interface only), W1–W5 public suite and site (Phase 5 scoping — next work order),
release tarball, any kernel change, any cross-process/shared-memory work (Phase 6+).

## 7. Sign-off block (executor completes)

```
WO-P4-PORTS execution report
- T0 B1–B4:              [ ] v1.0.2 render-verified | [ ] soak 2x30s | [ ] frame_count | [ ] Rust live
- T1 mapping tables:     [ ] Kotlin | [ ] Swift | [ ] Dart | [ ] TS summary
- T2 validator:          [ ] committed before ports | [ ] JSON-line output
- T3 Kotlin:             [ ] kernel + Steward + Heddle + JNI + README
- T4 Swift:              [ ] kernel + Steward + Heddle + README
- T5 Dart:               [ ] kernel + Steward + Heddle + README + forward interface + banner
- T6 TS bindings:        [ ] react | [ ] svelte | [ ] vue | [ ] react-native
- T7 validation:         [ ] 4/4 targets exit 0, report archived
- T8 report PDF:         [ ] + doc pointers

Deviations/findings (mandatory field — 07-ACCEPTANCE §6):
<none | list>

Sign-off:
- Executor: ______________  date: ______
- Staff review: ______________
```
