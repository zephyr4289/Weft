# SHP1 — Weft Spectrum Hardware-Profile Wire, Version 1 (Normative)

> Status: **Normative** for Pillar 5 (`weft-spectrum` Managed). Freeze class:
> **byte-frozen** like WCN1/WTR1/HPL1 — any change is a protocol version bump,
> never an in-place edit.
>
> SHP1 is the managed-side **integration seam** for Engineer 1's
> `core/c/include/weft_spectrum.h` (`weft_hw_profile_t` descriptor + runtime
> micro-probing engine). When E1's authoritative header merges, the offset
> table below is checked against it by `ci/scripts/run_spectrum_managed_shard.sh`
> stage 1; SHP1 itself does not change. Until then, the golden fixtures in
> `tests/spectrum/managed/fixtures/` are the single source of truth for
> cross-language decode parity, exactly as HPL1 did for Pillar 4.
>
> All offsets are **explicit little-endian** (Law 2). Managed consumers bind
> zero-copy (`DataView` / `memoryview` / `ByteData` / `UnsafeRawBufferPointer`)
> and never allocate in the steady-state telemetry loop (Law 1).

---

## 1. Record Geometry

One `weft_hw_profile_t` snapshot is ONE contiguous 192-byte record:

```
┌────────────────────────────────────────────────────────────────┐  offset 0
│ SHP1 HEADER + PROFILE FIELDS (188 bytes, pinned)               │
├────────────────────────────────────────────────────────────────┤  offset 188
│ CRC-32 (IEEE 802.3, u32 LE) over bytes [0, 188)                │  offset 192 (end)
└────────────────────────────────────────────────────────────────┘
```

| Symbol        | Value  | Notes                                          |
|---------------|--------|------------------------------------------------|
| `RECORD_SIZE` | `192`  | bytes; u16-asserted inside the record itself   |
| `CRC_OFFSET`  | `188`  | CRC-32 (IEEE 802.3, reflected, init/final xor) |
| `MAGIC`       | `"SHP1"` | bytes `0x53 0x48 0x50 0x31` at offset 0      |

A torn or truncated read is **always detectable**: any field mutation flips
the CRC (Law 4: fail-closed, never silently decode garbage).

## 2. Field Table

| Offset | Type | Field                    | Semantics                                                                 |
|-------:|------|--------------------------|---------------------------------------------------------------------------|
| 0      | 4B   | `magic`                  | `"SHP1"`                                                                  |
| 4      | u16  | `layout_version`         | `1`                                                                       |
| 6      | u16  | `record_size`            | `192`                                                                     |
| 8      | u32  | `feature_flags_lo`       | bits 0–31 of the feature vector (§3)                                      |
| 12     | u32  | `feature_flags_hi`       | bits 32–63 (currently zero; reserved for future features)                 |
| 16     | u32  | `silicon_tier`           | `0` UNKNOWN, `1` FLAGSHIP, `2` MID, `3` BUDGET                            |
| 20     | u32  | `thermal_state`          | `0` NOMINAL, `1` LIGHT, `2` MODERATE, `3` SEVERE, `4` CRITICAL            |
| 24     | u32  | `perf_cores`             | performance core count                                                    |
| 28     | u32  | `eff_cores`              | efficiency core count                                                     |
| 32     | u32  | `gpu_family`             | `0` UNKNOWN, `1` APPLE, `2` DESKTOP_DISCRETE, `3` MOBILE_INTEGRATED, `4` CONSOLE, `5` SOFTWARE |
| 36     | u32  | `cache_line_bytes`       | `64` or `128` (alignment validator input)                                 |
| 40     | u64  | `cpu_max_clock_khz`      | little-endian u64                                                         |
| 48     | u64  | `memory_total_bytes`     | little-endian u64                                                         |
| 56     | u64  | `memory_budget_bytes`    | governor arena budget (little-endian u64)                                 |
| 64     | u32  | `simd_width_bits`        | `128` / `256` / `512`                                                     |
| 68     | u32  | `frame_budget_us`        | per-frame deadline at max cadence (microseconds)                          |
| 72     | u64  | `max_frame_rate_milli_hz`| `240000` = 240 FPS (little-endian u64)                                    |
| 80     | u32  | `battery_permille`       | `0..1000`, `0xFFFF` = unknown (no battery / desktop)                      |
| 84     | u32  | `battery_charging`       | `0` discharging, `1` charging, `2` unknown                                |
| 88     | u32  | `visibility`             | `0` VISIBLE, `1` HIDDEN, `2` UNKNOWN (Page Visibility seam)               |
| 92     | u32  | `dma_lane_count`         | multi-lane async DMA lane count (Engineer 2 drivers)                      |
| 96     | u32  | `vendor_id`              | PCI/USB-style vendor id or synthetic probe id                             |
| 100    | u32  | `device_id`              | PCI/USB-style device id or synthetic probe id                             |
| 104–187| 84B  | reserved                 | MUST be zero; validators reject non-zero reserved bytes                   |
| 188    | u32  | `crc32`                  | CRC-32 over `[0, 188)`                                                    |

Reserved enforcement is part of decode: a decoder that finds non-zero bytes in
`[104, 188)` raises the profile error (Law 4 taxonomy `E_RESERVED_DIRTY`, §6).

## 3. Feature Flag Bits (`feature_flags_lo`)

| Bit | Name                 | Meaning                                            |
|----:|----------------------|----------------------------------------------------|
| 0   | `WASM_SIMD128`       | WASM `v128` SIMD available (web/Node runtimes)     |
| 1   | `SHARED_ARRAY_BUFFER`| `SharedArrayBuffer` constructible + cross-origin isolated |
| 2   | `WEBGPU`             | `navigator.gpu` (or Node WebGPU) adapter present   |
| 3   | `WEBGL2`             | WebGL2 context creatable                           |
| 4   | `AVX512`             | x86 AVX-512                                        |
| 5   | `AVX2`               | x86 AVX2                                           |
| 6   | `SSE42`              | x86 SSE4.2                                         |
| 7   | `NEON`               | ARM NEON                                           |
| 8   | `SVE2`               | ARM SVE2                                           |
| 9   | `RVV`                | RISC-V Vector extensions                           |
| 10  | `METAL_3`            | Apple Metal 3                                      |
| 11  | `CUDA`               | NVIDIA CUDA                                        |
| 12  | `APPLE_MPS`          | Apple Metal Performance Shaders                    |
| 13  | `OPENVINO`           | Intel OpenVINO                                     |
| 14  | `FASTRPC_DSP`        | Qualcomm FastRPC DSP (Engineer 2 driver seam)      |
| 15  | `NEUROPILOT`         | MediaTek NeuroPilot (Engineer 2 driver seam)       |
| 16  | `MULTILANE_DMA`      | multi-lane async DMA available                     |
| 17  | `BIG_LITTLE`         | heterogeneous big.LITTLE / P+E core topology       |
| 18  | `THERMAL_SENSOR`     | thermal pressure telemetry readable                |
| 19  | `DLPACK_EXPORT`      | zero-copy DLPack capsule export (Python arena)     |

Bits 20–63: reserved, MUST be zero (`E_RESERVED_DIRTY` on violation).

## 4. Cadence Governor Table (Normative, Cross-Language)

The Reactive Cadence Governor is a **pure integer state machine**. All four
managed runtimes (TypeScript, Swift, Dart, Python) implement EXACTLY this
table; `tests/spectrum/managed/` freezes a deterministic input vector and
asserts identical cap sequences everywhere (parity mandate).

### 4.1 Constants

| Constant            | Value                | Meaning                                   |
|---------------------|----------------------|-------------------------------------------|
| `CADENCE_LADDER`    | `[240, 120, 60, 30]` | Hz rungs, index 0 = profile max           |
| `SUSTAINED_TICKS`   | `10`                 | 10 telemetry ticks (~1 s at 10 Hz) before one down-step |
| `RECOVERY_TICKS`    | `50`                 | ~5 s cool before one up-step              |
| `BACKGROUND_CAP`    | `30`                 | Hz cap while `visibility == HIDDEN`       |
| `LOW_BATTERY_PERMILLE` | `150`             | ≤ 15 % and discharging → cap 60 Hz        |
| `MAX_TIER_STAGES`   | `2`                  | Tier N → N+1 → N+2 (two down-tier steps)  |

### 4.2 Transition rules (evaluated per tick, first match wins)

1. `visibility == HIDDEN` → cap = `BACKGROUND_CAP` (no ladder movement).
2. `thermal ≥ SEVERE` for `SUSTAINED_TICKS` consecutive ticks → step down one
   rung; reset sustained counter.
3. `thermal == MODERATE` for `2 × SUSTAINED_TICKS` consecutive ticks → step
   down one rung; reset sustained counter.
4. `battery_permille ≤ LOW_BATTERY_PERMILLE` and not charging → cap = `60`
   (does not move the ladder; released the tick condition clears).
5. `heap_pressure` event (allocation failure / device-lost signal) →
   `tier_stage = min(tier_stage + 1, MAX_TIER_STAGES)`; effective tier becomes
   `min(silicon_tier + tier_stage, 3)`; effective memory budget halves per
   stage. Ladder re-seeds from the effective tier's profile cap.
6. `thermal ≤ LIGHT` sustained for `RECOVERY_TICKS` → step up one rung (never
   above the effective tier's profile max) and reset the cool counter.

Any up-step requires the cool counter to fully re-accumulate (anti-flapping).
The governor state is a single reusable flyweight — `tick()` allocates nothing
(Law 1). All counters are plain integers; all outputs are integers; no strings
are produced on the hot path (reason codes are enum ints, §6).

## 5. Managed Binding Contract (Engineer Seams)

- **Engineer 1** authors `weft_hw_profile_t` + the micro-probing engine in
  `core/c`. Managed SDKs consume via: TypeScript (WASM import or
  pre-opened `SharedArrayBuffer` at the E1-agreed base offset), Python
  (`mmap`/`shared_memory` + `memoryview` cast), Swift (`UnsafeRawBufferPointer`
  over the mapped region), Dart (`dart:ffi` `Pointer<Uint8>` + `ByteData`).
- **Engineer 2** authors native acceleration drivers (FastRPC, NeuroPilot,
  Metal 3, CUDA, SVE2/NEON/RVV kernels). Managed bridges call E2's dynamic
  dispatch symbols through FFI; **this pillar authors zero lines of native C**.
- The probe/attach functions are seams: if E1's symbols are absent, decoders
  operate on SHP1 fixtures (tests) or on runtime-detected fallback profiles
  (`E_PROBE_UNAVAILABLE`, never a crash — Law 4).

## 6. Law 4 Error Taxonomy (frozen codes, cross-language)

| Code | Name                    | Raised when                                   | Managed behavior |
|-----:|-------------------------|-----------------------------------------------|------------------|
| 1    | `E_BAD_MAGIC`           | bytes [0,4) ≠ `"SHP1"`                        | surface + fallback profile, no throw |
| 2    | `E_BAD_VERSION`         | `layout_version ≠ 1`                          | surface + fallback profile |
| 3    | `E_BAD_SIZE`            | `record_size ≠ 192` or buffer < 192 B         | surface + fallback profile |
| 4    | `E_CRC_MISMATCH`        | CRC over `[0,188)` ≠ stored CRC               | torn read: surface + keep last-good |
| 5    | `E_RESERVED_DIRTY`      | non-zero byte in reserved regions             | surface + fallback profile |
| 6    | `E_PROBE_UNAVAILABLE`   | E1 probe symbols/imports absent               | runtime-detected fallback profile |
| 7    | `E_DEVICE_LOST`         | WebGPU device lost / Metal device error       | down-tier one stage, re-arm |
| 8    | `E_FFI_TIMEOUT`         | FastRPC/NeuroPilot dispatch timeout (E2 seam) | down-tier one stage, re-arm |
| 9    | `E_HEAP_PRESSURE`       | OOM signal from host runtime                  | tier down-stage + budget halve |
| 10   | `E_LISTENER_LEAK`       | listener added twice / removed unknown        | dev-surface only, never hot path |
| 11   | `E_ALIGN_INVALID`       | buffer violates cache-line alignment          | Python arena: re-slab, never copy-crash |
| 12   | `E_HUD_CONTEXT_LOST`    | canvas/layer context destroyed                | HUD falls back to DOM rows |
| 13   | `E_UNMARSHAL_FAILED`    | language-level unmarshal failure              | surface + fallback profile |
| 14   | `E_TIER_EXHAUSTED`      | already at MAX_TIER_STAGES                    | hold, surface, keep serving |
| 15   | `E_HUD_RECOVERED`       | HUD render path healed after failure          | banner clears (informational) |

## 7. Freeze Statement

Everything above — offsets, bit assignments, governor constants, error codes —
is **byte/behavior-frozen for SHP1 v1**. Golden fixtures
(`tests/spectrum/managed/fixtures/*.bin`) are generated by
`tests/spectrum/managed/fixtures/generate.mjs` and MUST re-generate
byte-identically (CI determinism stage). A v2 would use `layout_version = 2`
and a new fixture family; SHP1 v1 consumers never break.
