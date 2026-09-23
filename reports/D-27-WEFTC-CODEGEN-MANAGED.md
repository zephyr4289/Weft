# PATCHES — Project weftc, Pillar 1: Managed Runtimes & Reactive UI Code Generators

**Branch:** `feat/weftc-codegen-managed` (base: main @ `db24254`)
**Directive:** Mission Briefing — Pillar 1 (TypeScript DataView, Swift, Dart FFI, Python Buffer Protocol & Reactive UI Hooks), Swarm Layer 3, Engineer 3
**Result:** 10 independent patches delivering the four assigned codegen
backends + reactive UI hooks + the verification fabric around them.
**Zero guardrail regressions:** the frozen kernel is only ever *linked*
(never modified), the 4/4 heddle contract is untouched, and every pipeline
in `tools/weftc/tests/run_integration.sh` is green (8 stages, exit 0).

---

## The one-paragraph version

weftc turns a **schema IR** into zero-allocation flyweight view classes for
TypeScript, Swift, Dart and Python. One canonical wire story — the real
triad-1 envelope from `03-ENVELOPE` plus two payload schemas — is rendered
by four generators whose outputs are proven equivalent three ways:
**runtime parity** against golden binary buffers in TS and Python,
**static parity audits** for Swift and Dart (toolchain-less legs), and a
**cross-backend parity matrix** that parses every generated file and
compares its layout/API surface against the IR. Determinism is a CI gate:
regenerating all outputs must be byte-identical (`weftc --check`), and the
whole layer is grounded in the frozen kernel via a C harness that *emits*
the buffer the managed views must read back byte-for-byte.

---

## The laws, enforced (not intended)

| Law | Mechanism in this series | Evidence |
|---|---|---|
| **1 — Zero allocation in steady-state reads** | Flyweight `bind()` pattern (one view instance, rebound per frame); hot-path bodies (getters/setters/`with*` chainers) contain zero allocation sites — statically scanned; u64 fields expose lo/hi u32 split accessors for primitive-only hot paths; Flutter `WeftNotifier.notify` is a fixed-array indexed loop (no growable list, no iterators); React hooks never `setState` | TS 1M-read `--expose-gc` heap probe (median growth < 1 MiB ceiling); static allocation-site scans; 10k-frame → 1-render contract test |
| **2 — Strict little-endian** | Structural: the IR loader **rejects** `endian != "little"`. TS: every multi-byte `DataView` call passes explicit `true` (scan). Swift: all loads/stores convert through `.littleEndian` intrinsics — correct on any host by construction. Dart: every multi-byte `ByteData` access passes `Endian.little` (scan). Python: every `struct` format is `'<…'`; NumPy dtypes carry explicit `'<…'` byte order | LE static scans in all four audit suites; golden buffers hand-verified LE in Python suite |
| **3 — Mechanism, not policy; kernel byte-frozen** | Generators never import the Core AST (IR v1 adapter contract documented); generated TS uses only ArrayBuffer/SharedArrayBuffer/DataView — no node imports, no browser globals (scan); Python struct path is pure stdlib (NumPy optional, guarded); `core/c/weft.{c,h}` untouched — the C harness only links it; `@weft/react-hooks` is structural over views (no codegen dependency) | Law-3 environment scans; `git diff core/c` empty; npm/flutter/apple lanes untouched |
| **4 — Boundary schema hash verification** | Every backend emits a non-throwing `validateHeader(avail?)`: bounds + every `const` field. For the triad-1 envelope it reproduces the kernel's `weft_envelope_decode()` decision table **including** `payload_len > avail − header_size → false` (kernel `WEFT_DECODE_SHORT`) | Kernel parity pinned by `verify_golden.c`; full corruption matrix (magic/version/header_size/payloadLen/avail 16/79/80) in TS + Python suites |

---

## Patch-by-patch

### P1 — IR v1: spec, fixtures, golden buffers, kernel-grounded C harness

- `tools/weftc/schema/ir-v1.schema.json` — fail-closed JSON Schema; the
  handoff surface Engineer 1's Core AST maps onto (adapter contract in
  `schema/README.md` §5: resolve padding → absolute offsets, hex u64,
  reject non-LE; nothing else changes).
- Three canonical fixtures: **WeftEnvelope** (the real 16-byte triad-1
  header), **TelemetryFrame** (64 B, schema-ID handshake, alignment gap,
  vector + f64 pair + digest), **ImuSample** (32 B, header-less path).
- `tests/harness/gen_golden_buffers.py` — deterministic LE buffers +
  `expected.json`. u64 expectations are hex **strings** (JSON numbers lose
  precision past 2^53 — caught by our own suite, not by luck).
- `tests/harness/verify_golden.c` — links the frozen kernel; proves the
  golden envelope equals `weft_envelope_encode_v1()` output byte-for-byte
  and pins the decode decision table (bare envelope ⇒ `WEFT_DECODE_SHORT`).

### P2 — Codegen core lib

- `codegen/lib/ir.mjs` — fail-closed loader: unknown keys, big-endian,
  overlapping fields, misalignment, out-of-bounds, malformed consts are
  all **hard errors** (u64 consts parse losslessly from hex → BigInt).
- `codegen/lib/types.mjs` — the scalar ABI table + per-language accessor
  maps (DataView / struct / NumPy / Swift / dart:ffi): one source of truth.
- `codegen/lib/names.mjs` — casing pipeline + per-language reserved-word
  avoidance (`class` in TS, `extension` in Swift, `with` in Dart…).
- `codegen/lib/emitter.mjs` — deterministic banners (stable repo-relative
  IR paths, no clocks, no RNG). 9-test validation matrix.

### P3 — TypeScript backend (`--target=ts`)

- Emits `<name>-view.{ts,js,d.ts}` per struct + index barrel. The `.d.ts`
  carries `@byteOffset 0x…` JSDoc annotations on every member (directive §4
  creative vector).
- Flyweight API: `bind(buffer, byteOffset)` rebinds in place — accepts raw
  ArrayBuffer/**SharedArrayBuffer** and any TypedArray/DataView/Node-Buffer
  window (pool-correct absolute offsets). `validateHeader(avail?)`;
  getters/setters; fluent `with*` chainers returning `this`; u64
  `Lo/Hi` split accessors; const fields are read-only.
- 20-test suite: golden parity, Law-4 matrix, Law-1 static scan + heap
  probe, Law-2 LE scan, Law-3 environment scan, SAB + Buffer-pool binds,
  rebind flyweight proof, codegen determinism.

### P4 — Python backend (`--target=py`)

- `bind(buf, offset)` over `memoryview` (zero data copies); `struct.Struct`
  with explicit `'<…'` formats; per-index array accessors + bulk `_all()`
  tuples; u64 exact as Python ints (`_lo/_hi` splits + `with_*` chainers).
- `DTYPE` classmethod: NumPy structured dtype with **explicit offsets +
  itemsize** — gap-exact with the IR (the 4-byte telemetry gap at 36..40
  is preserved, not packed away). `as_numpy()` returns an `np.void` view
  **aliasing** the buffer: mutation visible both directions (tested).
- 21-test suite incl. readonly-bytes `TypeError`, u64-beyond-double-mantissa
  exactness, dtype-offsets-vs-IR, LE format scan.

### P5 + P6 — Swift & Dart backends (`--target=swift`, `--target=dart`)

- Swift: `public final class` flyweights over `UnsafeRawBufferPointer`;
  all multi-byte access decodes through `.littleEndian` intrinsics (any
  host, by construction); **`SIMD3<Float>`** mapping for `f32[3]`
  (`velocitySIMD` / `setVelocity(_:)` — directive §2.B); u64 lo/hi UInt32.
- Dart: dual layer per directive §2.C — `final class <Name>Ffi extends
  ffi.Struct` (offset-order declarations, `@ffi.Array.multi` inline arrays)
  plus the high-level `ByteData` flyweight for Flutter hot paths; u64 hex
  literals wrap to exact 64-bit bit patterns; parenthesized const
  expressions (operator-precedence audit caught `!=` vs `|`).
- Both gated by **static audit suites** (8 + 9 tests): structure vs IR,
  offset tables, LE discipline (including an exact-count raw-store check
  for single-byte members), Law-4 surface, reserved exclusion, byte-exact
  determinism. Runtime legs ride the `apple-packages`/`flutter-packages`
  lanes; the sandbox verifies what it can, honestly.

### P7 — weftc CLI + determinism gate

- `tools/weftc/weftc.mjs --ir <file|dir> --target ts,swift,dart,py|all
  --out <dir> [--check]`. Directory mode generates every fixture in one
  pass and emits the TS barrel.
- `--check` regenerates in memory and byte-compares: **exit 1 on drift**,
  with a per-file drift list. Proven in anger: it caught the missing index
  barrel; then verified byte-identical across all 12 outputs.

### P8 — `@weft/react-hooks` (directive §2.E)

- `useWeftBuffer(source, onFrame)`: subscribes without setState — **10,000
  frames → render count stays 1** (contract test). Latest-ref callback
  (fresh closure per render, zero resubscribes). StrictMode double-mount
  safe (lifecycle test).
- `useWeftCanvas(source, draw)`: frames coalesce into rAF ticks (newest
  wins), loop parks when idle or `document.hidden`, resumes on
  visibilitychange, cancels pending rAF on unmount — all tested against a
  **dep-aware React shim** (the package has zero runtime dependencies;
  `index.js` is a 2-line binding to real react).
- `createFrameSource` / `attachWebSocket` / `createSampleRing` /
  `drawFrameGraph`: reference-stable `(buffer, byteOffset, avail)` pub/sub,
  binary-only WS producer, preallocated Float32 oscilloscope ring,
  auto-scaling painter. Structural typing: any weftc view plugs in.

### P9 — Flutter `WeftNotifier` (directive §2.E)

- `WeftNotifier implements Listenable` with **fixed-capacity listener
  storage**: `frame()` is a plain indexed loop — zero allocation at notify
  time (Flutter's ChangeNotifier grows a list per notify; at 120–240 FPS
  that is exactly the GC pressure this pillar removes). Doubling growth is
  cold-path-only; removal is swap-remove; add is idempotent.
- `WeftWindowCache` (identity-cached `ByteData` re-derivation) +
  `WeftFramePump<V>` (one reused view, validator-before-bind, then
  `notifier.frame()` → `CustomPaint(repaint: notifier)` repaints without a
  single widget rebuild). Exported from `weft_flutter.dart`; static-audit
  suite rides the `flutter-packages` lane.

### P10 — Cross-backend parity audit

- `tools/weftc/audit/parity_audit.mjs`: parses **all four** backends'
  committed outputs and compares against the IR — byteLength, schemaId
  statics, every field's offset constant, accessor presence,
  reserved-field exclusion, `validateHeader`, LE marker counts. 3 fixtures
  × 4 backends = **12 rows, all green**; exit 1 on any disagreement.
- Drift-aware: an `irOverride` hook lets tests feed a doctored IR (wrong
  byteLength → all four rows fail; shifted velocity offset → flagged).
  The audit **enforces** agreement; it does not merely print it.

### P11 — E2E kernel integration

- `tests/harness/emit_frame_stack.c` — the "C harness produces the bytes"
  leg: builds the real Weft buffer with the frozen kernel's encoder
  (envelope + payload + canary), self-checks via `weft_envelope_decode()`
  before writing. Its output is **byte-identical** to the Python golden
  generator's (`cmp` in the pipeline).
- TS + Python integration tests execute the harness at runtime, validate
  the envelope, bind the payload at `header_size`, read every field
  bit-exactly, and assert the canary rule.
- `tests/run_integration.sh` — the 8-stage pipeline, `set -euo pipefail`
  throughout: lib → TS(+heap probe) → Python → Swift/Dart audits → parity
  matrix → `weftc --check` → C harness → E2E legs. **Exit 0 observed.**

### P12 — CI shard + registration (this patch)

- `ci/scripts/run_weftc_codegen_shard.sh` — wraps the 8-stage pipeline
  with artifact logging (`ci/run-artifacts/shard-weftc-codegen.log`).
- Registered in `.github/workflows/extreme-test.yml` (`needs_rust: false`,
  filter `any_code`) and in `ci/README.md` shard ownership — same-PR
  registration per the routing contract.

---

## Verification summary (this tree, this run)

| Suite | Tests | Result |
|---|---|---|
| IR loader + codegen lib (node --test) | 9 | ✅ |
| TS backend (node --test, `--expose-gc`) | 20 | ✅ |
| Python backend (unittest) | 21 | ✅ |
| Swift static audit | 8 | ✅ |
| Dart static audit | 9 | ✅ |
| Parity audit matrix (3 fixtures × 4 backends) | 12 rows | ✅ green |
| Codegen determinism (`weftc --check`) | 12 files | ✅ byte-identical |
| C kernel harness (verify + emit) | — | ✅ exit 0, byte-identical |
| E2E round-trip (TS + Python legs) | 4 | ✅ |
| React hooks (dep-aware shim) | 11 | ✅ |
| **Total automated gates** | **~90** | **all green** |

## Guardrails

- `core/c/weft.{c,h}`: **untouched** (harness links only) — kernel-frozen law intact.
- `heddles/{react,vue,svelte,react-native}`: untouched — 4/4 heddle contract intact.
- `packages/react`: untouched (the new `@weft/react-hooks` is additive; `packages/*` auto-joins the workspace).
- Binding parity, litmus, chaos and all other shards are untouched by these paths; the new shard is path-routed (`tools/weftc/**`, `packages/react-hooks/**` ride existing `any_code` filters).

## Handoff notes for Engineer 1 (Core AST adapter)

When `tools/weftc/core` lands: map `IrSchema`/`StructLayout` → IR v1
(`schema/README.md` §5) — resolve padding to absolute offsets, materialize
reserved fields, hex-encode u64 schema IDs, assert `endian: "little"`, and
reject (with diagnostics) anything IR v1 cannot express (bitfields,
unions). The managed backends never import the Core AST — the IR is the
wall; extending IR v1 means bumping `irVersion`, adding fixtures, and
updating all four backends + the parity audit in the same PR.
