# weftc IR v1 — StructLayout Intermediate Representation

**Status:** normative for the managed backends (`--target=ts|swift|dart|python`)
**Owner:** Engineer 3 (Managed Runtimes, Cross-Platform & UI Architecture Lead)
**Schema:** [`ir-v1.schema.json`](./ir-v1.schema.json) (JSON Schema draft-07)
**Fixtures:** [`fixtures/`](./fixtures) — `weft_envelope.json`, `telemetry_frame.json`, `imu_sample.json`

---

## 1. Why an IR v1 exists

`weftc` is being delivered by three engineers in parallel. Engineer 1 owns the
Core AST & Layout Engine; this directory defines the **handoff surface** that
the managed backends consume. Until Engineer 1's AST lands on `main`, the
canonical fixtures in this directory are the single source of truth and the
backends are exercised against them end-to-end. When the Core AST lands, only
an adapter is needed — the backends do not change.

The contract is deliberately tiny: one JSON document per struct, fully
resolved (absolute byte offsets, no padding inference, no bitfields). Layout
resolution is Engineer 1's job; code generation is ours; the IR is the wall
between them.

## 2. Document shape

```jsonc
{
  "irVersion": 1,                      // pinned; loader rejects others
  "generator": "weftc-core/0.3.0",     // provenance (informational)
  "schema": {
    "name": "TelemetryFrame",          // PascalCase
    "namespace": "weft.telemetry",     // optional
    "schemaId": "0x8F4C1120A9B30012",  // optional u64 handshake (hex string)
    "schemaVersion": 3,                // optional u16
    "byteLength": 64,
    "alignment": 8,                    // 1|2|4|8
    "endian": "little",                // Law 2 — "big" is rejected at load time
    "fields": [
      {
        "name": "velocity",            // camelCase; backends transliterate
        "type": "f32",                 // u8..u64,i8..i64,f32,f64,bool
        "offset": 16,                  // absolute byte offset of element 0
        "count": 3,                    // optional; >1 = inline fixed array
        "const": "WEFT",               // optional; u8[] may carry an ASCII const
        "role": "reserved",            // optional semantic hint (see §3)
        "doc": "Velocity vector, m/s." // flows into generated doc comments
      }
    ]
  }
}
```

## 3. Validation rules (enforced by the loader, not by trust)

1. `irVersion` must be `1`; unknown keys are rejected (fail-closed, TIER4 style).
2. `endian` must be the literal `"little"` — **Law 2** is structural, not a default.
3. Every field must satisfy `offset + sizeof(type) × count ≤ byteLength`.
4. Fields must not overlap: the loader sorts by offset and asserts disjoint
   byte ranges. Overlap = invalid IR, exit nonzero.
5. Natural alignment: `u64/i64/f64` at 8, `u32/i32/f32` at 4, `u16/i16` at 2.
   Misaligned fields are rejected (generated managed views assume aligned
   scalar access; unaligned access would silently cost 3–10× on some cores).
6. `u64` consts may be JSON numbers or `"0x…"` hex strings (JavaScript cannot
   represent all u64 as doubles — hex strings are the lossless form).
7. `const` fields participate in `validateHeader()` (Law 4). `role: reserved`
   fields are excluded from generated fluent setters.

## 4. `validateHeader()` semantics (Law 4)

Every backend emits a non-throwing header handshake on every generated view:

- **Bounds:** the byte window `[offset, offset + byteLength)` must exist
  inside the bound buffer.
- **Const fields:** each `const` field is read and compared (magic as ASCII,
  integers exactly). First mismatch wins; no exception is raised.
- **Header-less schemas** (e.g. `ImuSample`): the emitted `validateHeader()`
  performs the bounds check only. The method is always emitted so callers can
  branch uniformly.

For the triad-1 envelope fixture this reproduces the kernel's
`weft_envelope_decode()` decision table as a boolean: `avail < 16` → false,
bad magic → false, `header_size < 16 || > avail` → false,
`payload_len > avail − header_size` → false. The C harness
(`tests/harness/verify_golden.c`) pins this against the real kernel.

## 5. Adapter contract for the Core AST (Engineer 1)

When `tools/weftc/core` lands, it exposes its `IrSchema`/`StructLayout` AST.
The adapter (`tools/weftc/core/adapter.mjs`, delivered with the core) must
produce a document satisfying `ir-v1.schema.json`:

- resolve padding/alignment → absolute `offset`s
- materialize named pad fields (`role: "reserved"`)
- lowercase-hex `schemaId` strings for u64
- assert `endian: "little"`

The managed backends NEVER import the Core AST directly. If a layout feature
cannot be expressed in IR v1 (bitfields, unions, enums-with-backing-store),
the adapter must reject it with a precise diagnostic rather than approximate.
Extending IR v1 = bump `irVersion`, add fixtures, update all four backends +
parity audit in the same PR.

## 6. Golden buffers

`tests/harness/gen_golden_buffers.py` renders the three fixtures (plus the
kernel-shaped `weft_frame_stack.bin`: envelope + 64-byte payload + canary)
into `tests/golden/*.bin`, with a shared expectation table
`tests/golden/expected.json`. All float values are **dyadic** (exactly
representable in f32/f64), so every language compares bit-exact without
tolerance hacks. `tests/harness/verify_golden.c` re-derives the same bytes in
C and cross-checks the envelope against the frozen kernel's own
`weft_envelope_encode_v1()` / `weft_envelope_decode()`.

Regeneration must be byte-identical — the codegen determinism CI gate
diff-checks both the `.bin` files and every generated backend output.
