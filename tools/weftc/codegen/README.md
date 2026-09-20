# weftc-codegen — Pillar 1 Native Code Generators

**Project weftc, Pillar 1** (native systems & GPU compute code generators): turn
Engineer 1's verified `weft-ir` layout schemas into production-grade native
code where data reads are **0-cost pointer projections**, verified at compile
time with static assertions, and mapped **byte-for-byte into GPU buffers
without host-side repacking**.

Four backends, one verified IR, zero dependencies in the output:

```
weft-ir JSON v1 (Eng 1: AST + Static Layout Engine)
        |
        v
  ┌─────────────┬──────────────┬──────────────┬───────────────┐
  │ --target=c  │ --target=rust│ --target=wgsl│ --target=glsl │
  │ C11 headers │ no_std mods  │ std430/140   │ std430/140    │
  └─────────────┴──────────────┴──────────────┴───────────────┘
```

```sh
make                                        # build the generator (pure C11)
./weftc-codegen --ir tests/fixtures/telemetry_frame.weft.json --verify-only
./weftc-codegen --ir schema.weft.json --target all --out gen/
make test                                   # the full verification gate
```

---

## The four Laws, enforced by construction

| Law | Requirement (briefing) | Where it is enforced |
|---|---|---|
| 1 | zero dynamic allocation on the read path | generated code contains no `malloc`/`free`/`Box`/`Vec` — casts borrow, packed IO is by-value through caller storage; the IR loader rejects before emission |
| 2 | strict no_std / pure C11 | generated C includes ONLY `<stdint.h> <stdbool.h> <stddef.h> <stdalign.h> <assert.h>` (SIMD paths live behind `__AVX2__`/`__ARM_NEON` capability macros and pull nothing on plain targets); generated Rust compiles as `#![no_std]` (`core` only) — proven by the test crate's lib |
| 3 | unaligned access safety | cast helpers validate alignment before projecting; `*_read_packed`/`*_write_packed` are byte assembly (C) / `read_unaligned`+`write_unaligned` (Rust) — no UB on any architecture, including trap-on-unaligned targets |
| 4 | deterministic GPU layouts | WGSL/GLSL emitters emit explicit padding and **refuse totally** (with a precise, machine-checkable reason) any layout they cannot reproduce byte-for-byte; `std140`/`std430` alignment is computed, never guessed |

## Deliverable A — C11 (`--target=c`)

* Standalone single-header `.h` per struct: the shared projection core (error
  taxonomy, endian guard, LE readers/writers, IEEE 754 binary16 codec) is
  embedded behind its own include guard, so any subset of headers compiles
  alone and all of them compile together.
* Compile-time ABI guards, exactly the briefing's pattern:

  ```c
  static_assert(sizeof(weft_telemetry_frame_t) == 64, "weft ABI layout violation: telemetry_frame size");
  static_assert(offsetof(weft_telemetry_frame_t, velocity) == 8, "weft ABI: telemetry_frame.velocity offset mismatch");
  ```

* Zero-copy cast helper — the briefing's signature:

  ```c
  static inline const weft_telemetry_frame_t*
  weft_cast_telemetry_frame(const void* buffer, size_t len, weft_error_t* err);
  ```

  validates byte length, buffer alignment and the schema hash header (three
  compares + one load; the suite reports sandbox-timed ns/op, informational).
* `weft_cast_*_mut` (writer side), `*_read_packed` / `*_write_packed`
  (alignment-free, for DMA/UMEM-packed sources and destinations).
* Bitfield accessors (`weft_mcu_status_get_mode` / `_set_mode`), fixed-array
  `LEN` macros, offset macros (`WEFT_*_OFF_*`).
* **SIMD batch validation** (`weft_validate_batch_<name>`): AVX2 4×u64 /
  8×u32 lanes, NEON on aarch64, scalar fallback everywhere — all paths
  alignment-safe; the test suite runs `-mavx2` and scalar builds and requires
  byte-identical verdicts.
* Explicit struct alignment is portable C11: `_Alignas(N)` on the first
  member raises the struct alignment without perturbing any offset
  (`#[repr(align(N))]` on the Rust side).

## Deliverable B — Rust (`--target=rust`)

* `#[repr(C)]` (+ `#[repr(align(N))]` when the IR demands it) with
  **synthesized pad members** so `repr(C)`'s computed offsets match the
  normative weft-ir offsets exactly (the `mcu_status` 18..20 gap is the
  fixture that catches drift).
* Zero-allocation projection API:

  ```rust
  impl TelemetryFrame {
      pub const SCHEMA_ID: u64 = 0x8F4C_1120_A9B3_0012;
      pub fn from_bytes(bytes: &[u8]) -> Result<&Self, WeftError>;        // zero-copy
      pub fn from_bytes_mut(bytes: &mut [u8]) -> Result<&mut Self, WeftError>;
      pub fn read_packed(bytes: &[u8]) -> Result<Self, WeftError>;        // alignment-free
      pub fn write_packed(&self, dst: &mut [u8]) -> Result<(), WeftError>;
      pub fn as_bytes(&self) -> &[u8; Self::SIZE];                        // direct upload source
      pub const fn with_velocity(mut self, v: f32) -> Self { ... }        // zero-overhead builders
  }
  ```

* `no_std`, `core` only. `WeftF16` is a dependency-free binary16 newtype —
  round-to-nearest-even, bit-identical with the C codec (both sides prove the
  exhaustive 65536-pattern identity).
* Optional `bytemuck` (`Pod`/`Zeroable`) and `zerocopy` 0.8
  (`Immutable`/`TryFromBytes`/`IntoBytes`) impls behind cargo features —
  never compiled unless the consumer opts in and brings the dependency.

## Deliverable C — GPU (`--target=wgsl`, `--target=glsl`)

* WGSL storage (std430-equivalent) and uniform (std140-equivalent) struct
  variants **byte-exact with the host layout**: explicit pad members
  (`weft_padN`) are computed so the CPU buffer uploads via
  `queue.writeBuffer()` / `vkMapMemory` / an SSBO upload **without any
  host-side reformatting**.
* `vec3` → 16-byte alignment, uniform array strides rounded to 16, u64/i64
  and f16 exclusions — all computed by `gpu/gpu_layout.c` and **independently
  re-derived by `tests/gpu_layout_check.c` from the emitted text** (double
  entry: the emitter and the checker are separate implementations that must
  agree with the C headers' `offsetof` ground truth).
* Law 4 in action — a layout that cannot be reproduced is **refused totally**:

  ```
  // WEFT-GPU-NOT-EXACT: mcu_status — storage variant WITHHELD (Law 4)
  // reason: field vbus_mv (scalar): WGSL and GLSL expose no 16-bit integer
  //   buffer types; widen to u32 (or keep this struct CPU-side)
  ```

  The refusal set is part of the gate: the runner asserts the exact expected
  warnings fire — an emitter that silently starts emitting a broken struct
  (or silently stops emitting a good one) fails CI.
* `<name>_validate.wgsl` / `<name>_validate.comp` — generated proof shaders
  that consume an array of host-written frames straight from a
  storage buffer and validate every schema hash GPU-side. The `.comp` files
  compile to SPIR-V under glslangValidator (CI leg; declared-skip when the
  tool is absent).

## Deliverable D — end-to-end native ABI consistency

`tests/run_tests.sh` is the single gate. It proves, in order:

1. **Determinism** — two generator runs over the same IR are byte-identical.
2. **Golden identity** — committed goldens (`tests/golden/`) match fresh
   output; reviewing a PR means reviewing the generated code diff directly.
3. **Law-4 matrix** — the exact expected refusal set fires (and no more).
4. **C roundtrip × 3 flavors** (default / `-mavx2` / ASAN+UBSAN): 65 gates —
   compile-time ABI asserts, cast happy path + all three refusal modes,
   packed read/write through deliberately misaligned buffers, bitfield
   roundtrips, the exhaustive f16 identity, SIMD batch validation. The AVX2
   and scalar builds must produce **byte-identical stage-1 bins**.
5. **Rust** — the `#![no_std]` lib compiles (bare-metal proof) and the
   integration tests read the C-written bins **bit-exact**, then mutate via
   const-fn builders and write stage-2 bins.
6. **C re-reads Rust's bins** — three execution domains, one buffer, zero
   copies, every field asserted on both sides independently.
7. **GPU double-entry** — WGSL+GLSL offsets independently recomputed from the
   emitted text vs the C headers' `offsetof`.
8. **glslang (optional)** — proof shaders compile to SPIR-V.

Five fixtures cover the feature matrix:

| fixture | exercises |
|---|---|
| `telemetry_frame` | the briefing's example ABI (sizeof 64, `velocity@8`, schema `0x8F4C1120A9B30012`); vec3/vec4; WGSL+GLSL storage-exact, uniform refused (u64 header) |
| `mcu_status` | bitfields, f16 sensors, u16 arrays, a 2-byte IR gap (pad synthesis!); GPU-refused both languages |
| `camera_exposure` | u32 schema header, explicit align-16, `mat4x4<f32>` over `[f32;16]`, `[vec4<f32>;2]` arrays; **uniform/std140-exact in both languages** |
| `sensor_event` | embedded struct (`axis_sample`), nested packed IO, batch validation star; GPU-refused (i16) |
| `audio_peak` | `[f16;4]` bank; WGSL storage-exact via `enable f16`; uniform + GLSL refused |

---

## The weft-ir JSON v1 contract (Engineer 1 handoff)

The compiler core emits this per module; the generator verifies every layout
law **before** emitting anything (a violating IR is rejected with a
file:line-level message, never partially emitted):

```jsonc
{
  "weft_ir": 1,                      // required, exactly 1
  "module": "telemetry",             // lower_snake, reserved words rejected
  "types": [
    {
      "kind": "struct",              // v1: structs only
      "name": "telemetry_frame",     // lower_snake, unique, not a type token
      "root": true,                  // default true; root = schema-headered, castable
      "schema_id": "0x8F4C1120A9B30012",  // or "auto" (FNV-1a-64 of the signature — see below)
      "schema_id_width": 64,         // 64 (u64 header) or 32 (u32 header), default 64
      "align": 8,                    // declared struct alignment (>= natural; > natural = repr(align))
      "size": 64,                    // must equal roundUp(align, last field end) exactly
      "fields": [
        { "name": "velocity",        // valid in C+Rust+WGSL+GLSL namespaces (checked)
          "type": "f32",             // u8..u64, i8..i64, f16, f32, f64, bool,
                                     // vec2/3/4<f32|f16>, "[T; N]", struct name
          "offset": 8,               // normative; verified: aligned, ordered, in-bounds
          "doc": "m/s",              // optional
          "gpu_type": "mat4x4<f32>", // optional; v1 whitelist, host type [f32;16]
          "bits": [                  // optional; integer scalars only
            { "name": "mode", "lo": 0, "hi": 1 }
          ] }
      ]
    }
  ]
}
```

**Layout laws the loader enforces** (all violations = hard error):
offsets ascending and non-overlapping; every field C-aligned at its offset;
field end within `size`; declared `align` is a power of two ≥ the natural
member alignment; `size == roundUp(align, end)` — tail padding must be exact;
root structs lead with `schema_id` (u64 or u32 per `schema_id_width`) at
offset 0; array counts in `[1, 65536]`; bitfields within field width with
struct-unique accessor names; no struct cycles; names reserved in any of the
four target languages are rejected up front.

**Schema ids.** `auto` computes FNV-1a-64 over the canonical layout signature
(`weft/v1|struct=...|field=name:type@off[+bits][+gpu]...`). FNV is a
deterministic *layout fingerprint*, not a cryptographic hash — for the
cryptographic 64-bit schema hash the trust chain requires, Eng 1's core
passes an explicit `schema_id` (the generator consumes but never invents
cryptographic claims). Banner comments always state which mode produced the
id.

**Emission contract.** Files are named `<struct>.h` / `<struct>.rs` /
`<struct>.wgsl` / `<struct>.glsl` (+ `<struct>_validate.wgsl` / `.comp` for
byte-exact roots) plus the shared core (`weft_projection_core.h` /
`.rs`, overridable via `--core-name`). Structs that embed other structs
include their dependencies (C: `#include`; Rust: `use super::`;
WGSL/GLSL: dependency closure inlined into the same file — WGSL has no
include system).

## Repository layout

```
tools/weftc/codegen/
  weftc_codegen.h/.c   shared core: arena, strbuf, strict JSON, naming, signature
  weft_ir.c            IR loader — every layout law, refusal with context
  main.c               CLI (--ir --target --out --core-name --rust-traits --verify-only)
  c/emit_c.c           C11 backend
  rust/emit_rust.c     Rust backend
  gpu/gpu_layout.*     WGSL/GLSL alignment engine (Law 4 machinery)
  gpu/emit_wgsl.c      WGSL backend (+ proof shaders)
  gpu/emit_glsl.c      GLSL backend (+ .comp proof shaders)
  tests/fixtures/      the five weft-ir schemas (the reference corpus)
  tests/golden/        committed generator output (review generated code directly)
  tests/c_roundtrip.c  C-side ABI ground truth + cross-language stage bins
  tests/gpu_layout_check.c  INDEPENDENT WGSL/GLSL layout verifier (double entry)
  tests/rust/          no_std lib + bit-exact roundtrip suite (cargo, zero deps)
  tests/run_tests.sh   the gate (also run by ci/scripts/run_weftc_codegen_shard.sh)
```

## Honesty

* Cast-helper ns/op figures are `[SANDBOX-TIMED]` and informational, never a
  gate; the structural claim is "three compares + one load", auditable by
  reading the emitted function.
* The glslang SPIR-V leg is `DECLARED-SKIP` where the tool is absent; CI
  runners install `glslang-tools`.
* `auto` schema ids are non-cryptographic fingerprints (stated in every
  banner that uses one).
* WGSL/GLSL validity is verified by an independent parser in this suite
  (layout double-entry) and by glslang for GLSL; a live WebGPU/Vulkan
  execution leg is future work (Pillar 2+ hardware matrix).
