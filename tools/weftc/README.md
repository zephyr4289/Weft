# weftc — managed codegen (Engineer 3 slice, Pillar 1)

`weftc` turns a **weftc IR v1** schema document into zero-allocation managed
view classes for TypeScript/JavaScript, Swift, Dart (Flutter) and Python.

```
IrSchema / StructLayout AST (Engineer 1, adapter pending)
        │
        ▼   IR v1  (tools/weftc/schema/ir-v1.schema.json)
┌──────────────────────────────────────────────────────────┐
│                     weftc CLI (this dir)                 │
│   --target ts    → DataView flyweight views (.ts/.js/.d.ts)
│   --target swift → UnsafeRawBufferPointer flyweights (+SIMD3)
│   --target dart  → dart:ffi Struct + ByteData view layers
│   --target py    → struct/memoryview/NumPy zero-copy views
└──────────────────────────────────────────────────────────┘
```

## Usage

```sh
# one schema, one target
node tools/weftc/weftc.mjs \
  --ir tools/weftc/schema/fixtures/telemetry_frame.json \
  --target ts --out generated/ts

# every fixture, every target
node tools/weftc/weftc.mjs \
  --ir tools/weftc/schema/fixtures \
  --target all --out generated

# CI determinism gate: regenerate in memory, byte-compare, exit 1 on drift
node tools/weftc/weftc.mjs --ir tools/weftc/schema/fixtures \
  --target all --out tools/weftc/codegen --check
```

Targets: `ts`, `swift`, `dart`, `py`, or `all`. Outputs are **deterministic**:
identical IR produces byte-identical files on any machine (stable banners,
no clocks, no RNG) — this is what makes `--check` a valid CI gate.

## The four laws, as enforced by the generators

| Law | Enforcement |
|-----|-------------|
| **1. Zero allocation in the steady-state read loop** | Flyweight pattern: instantiate once, `bind(buffer, offset)` per frame — one view object, zero per-frame garbage. Hot-path bodies (getters/setters/`with*` chainers) are static-scanned for allocation sites and probed at runtime with a 1M-read heap-stability test (TS). u64 fields additionally expose lo/hi u32 split accessors (primitive-only paths in TS/Swift/Dart). |
| **2. Strict little-endian** | Structural: the IR loader *rejects* `endian != "little"`. TS: every multi-byte `DataView` call passes explicit `true` (static-scan). Swift: every load/store converts through `.littleEndian` intrinsics — correct on any host by construction. Dart: every multi-byte `ByteData` access passes `Endian.little` (static-scan). Python: every `struct` format is `'<…'`; NumPy dtypes carry explicit `'<…'` byte order. |
| **3. Mechanism, not policy / universality** | Generated TS uses only `ArrayBuffer`/`SharedArrayBuffer`/`DataView` — no node imports, no browser globals (static-scanned). Python uses the stdlib `struct`/`memoryview` path; NumPy is optional and guarded. Swift/Dart emit standard-library-only code. The frozen kernel (`core/c/weft.{c,h}`) is only *linked* by the C harness, never modified. |
| **4. Boundary schema hash verification** | Every backend emits a non-throwing `validateHeader(avail?)` handshake: bounds + every `const` field (magic, schema ID, version). For triad-1 envelope schemas it mirrors the kernel's `weft_envelope_decode()` decision table **including** the payload-availability rule (`payload_len > avail − header_size → false`, i.e. kernel `WEFT_DECODE_SHORT`). The decision table is pinned against the real kernel by `tests/harness/verify_golden.c`. |

## Layout of this directory

```
tools/weftc/
├── weftc.mjs                  # unified CLI (--ir/--target/--out/--check)
├── schema/
│   ├── ir-v1.schema.json      # JSON Schema for the IR (fail-closed)
│   ├── fixtures/              # canonical schemas: WeftEnvelope (the real
│   │                          # triad-1 header), TelemetryFrame, ImuSample
│   └── README.md              # IR spec + Engineer 1 adapter contract
├── codegen/
│   ├── lib/                   # IR loader, type model, names, emitter (+tests)
│   ├── ts/                    # generator + golden/ outputs (+20-test suite)
│   ├── swift/                 # generator + golden/ outputs (+static audit)
│   ├── dart/                  # generator + golden/ outputs (+static audit)
│   └── python/                # generator + golden/ outputs (+21-test suite)
├── audit/                     # cross-backend parity audit (P10)
└── tests/
    ├── golden/                # .bin buffers + expected.json (parity truth)
    └── harness/               # deterministic buffer generator + C verifier
                               # that links the FROZEN kernel
```

## Golden fixtures

Three canonical schemas exercise every backend feature:

- **WeftEnvelope** (16 B) — the real triad-1 wire header from `03-ENVELOPE`:
  magic `WEFT`, version u16, header_size u16, seq u32, payload_len u32.
- **TelemetryFrame** (64 B) — payload with schema-ID handshake, f32[3]
  vector, f64 pair, u8[8] digest; includes a 4-byte alignment gap.
- **ImuSample** (32 B) — header-less lean frame (bounds-only validation).

Golden `.bin` buffers are rendered by `tests/harness/gen_golden_buffers.py`
(deterministic, all floats dyadic ⇒ bit-exact everywhere; u64 expectation
values stored as hex strings because JSON numbers lose precision past 2^53)
and cross-checked against the frozen kernel by `tests/harness/verify_golden.c`.

## Wire-integration pattern

```ts
import { WeftEnvelopeView } from './weft-envelope-view.js';
import { TelemetryFrameView } from './telemetry-frame-view.js';

const env = new WeftEnvelopeView();     // one instance per thread
const frame = new TelemetryFrameView(); // one instance per thread

function onRingBytes(buf, offset, avail) {   // hot path — zero allocations
  if (!env.bind(buf, offset).validateHeader(avail)) return;
  if (!frame.bind(buf, env.headerSize).validateHeader(avail - env.headerSize)) return;
  sink += frame.timestampNsLo;               // primitive-only field reads
  sink += frame.pressurePa;
}
```

The identical pattern exists in all four languages — that is the point of the
parity fixtures: one schema, four byte-exact projections.
