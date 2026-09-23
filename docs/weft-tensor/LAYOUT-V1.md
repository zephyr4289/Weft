# Weft Tensor Ring — Layout V1 (WTR1)

Status: **NORMATIVE** for Pillar 2 (Project weft-tensor).
Owner: Engineer 3 (Managed Runtimes / AI APIs / UI Fabric).
Consumers: `packages/weft-tensor` (TypeScript), `python/weft_tensor` (Python DLPack bridge),
`packages/react-tensor`, `packages/flutter-tensor`, `demos/realtime-ai-vision`,
and Engineer 1/2 native adapters (reference only — their DMA ring may differ; this
document defines the *managed* tensor plane that bridges to them).

Every multi-byte scalar in this layout is **little-endian** (Law 2), matching the
frozen kernel envelope (`core/c/weft.c`) and Pillar 1 managed views. All floating
point is IEEE 754. Implementations must reject big-endian declarations explicitly
rather than guessing.

---

## 1. Concept

A **tensor ring** is one contiguous region of shareable memory
(`ArrayBuffer` | `SharedArrayBuffer` | `mmap` | ctypes buffer) laid out as:

```
+-----------------------------+  offset 0
| RingHeader (128 B)          |
+-----------------------------+  offset header_size (128)
| Slot 0: SlotHeader (64 B)   |
|         payload (cap bytes) |
+-----------------------------+  offset header_size + 1 * slot_stride
| Slot 1: SlotHeader (64 B)   |
|         payload (cap bytes) |
+-----------------------------+  ...
| Slot N-1 ...                |
+-----------------------------+
```

- The ring header is written **once** by the producer at creation
  (`producer_seq` updates per commit).
- Each slot is a fixed-stride record: 64-byte slot header + payload capacity.
- `payload_cap = slot_stride - 64`. The **stride** is 64-byte aligned; payloads
  do not have to fill the capacity (`payload_len` in the slot header says how
  many bytes are live).

This is a **seqlock-style single-producer / multi-consumer** ring, consistent
with the Weft Triad heritage: publishing is a single atomic u64 store; readers
are wait-free with a bounded re-read (no locks, no allocation).

## 2. Ring header (128 bytes)

| Off  | Size | Field           | Notes                                                          |
|------|------|-----------------|----------------------------------------------------------------|
| 0    | 4    | magic           | ASCII `"WEFT"` = 0x57 0x45 0x46 0x54                           |
| 4    | 2    | layout_version  | `1`                                                            |
| 6    | 2    | header_size     | `128`                                                          |
| 8    | 4    | slot_count      | `>= 2`                                                         |
| 12   | 4    | slot_stride     | `>= 64 + payload_cap`, 64-byte aligned                         |
| 16   | 1    | dtype_code      | DLPack `DLDataTypeCode` enum (see §2.1)                        |
| 17   | 1    | dtype_bits      | 8 / 16 / 32 / 64                                               |
| 18   | 2    | lanes           | `1` in V1                                                      |
| 20   | 4    | elem_size       | `dtype_bits / 8 * lanes` (bytes)                               |
| 24   | 32   | shape[8]        | `u32` per dimension, 0-padded; rank = index of last non-zero   |
| 56   | 32   | strides[8]      | `u32` per dimension **in elements** (row-major), 0-padded      |
| 88   | 8    | schema_id       | `u64`, producer-defined identity of the tensor schema          |
| 96   | 8    | producer_seq    | **publish word**: count of committed frames (starts at 0)      |
| 104  | 4    | tick_hz         | expected producer rate (e.g. 120 for video, 93 for 512@48k)    |
| 108  | 4    | flags           | bit0 = little_endian (always 1), bit1 = shared_memory          |
| 112  | 4    | header_crc      | CRC-32/IEEE (zlib) over bytes `[0, 96) ++ [104, 112)`          |
| 116  | 12   | reserved        | 0                                                              |

`header_crc` covers only the *static configuration* (it excludes the mutable
`producer_seq` at [96,104) and the `header_crc` field itself). Readers validate
it once at attach (Law 4 boundary); the hot loop only checks slot magic/seq.

### 2.1 dtype_code — DLPack `DLDataTypeCode` (verbatim, zero-mapping parity)

| Code | DLPack       | Meaning            |
|------|--------------|--------------------|
| 0    | `kDLInt`     | signed integer     |
| 1    | `kDLUInt`    | unsigned integer   |
| 2    | `kDLFloat`   | IEEE float         |
| 3    | `kDLBfloat`  | brain float        |
| 4    | `kDLComplex` | complex            |
| 5    | `kDLBool`    | boolean            |

Choosing DLPack's enum as the wire format means the Python bridge copies the
header dtype straight into `DLDataType{code, bits, lanes}` with **no mapping
table and no drift** (Law 2 / Law 4).

### 2.2 Strides

Stored **in elements** (DLPack convention). Byte stride = element stride ×
`elem_size`. A C-contiguous writer MUST write the real strides (e.g. shape
`[2,3]` → strides `[3,1]`); it never relies on "0 means derive". Readers may
*additionally* accept all-zero strides as contiguous for backwards friendliness
but MUST then assert contiguity against the shape.

## 3. Slot header (64 bytes)

| Off | Size | Field          | Notes                                                        |
|-----|------|----------------|--------------------------------------------------------------|
| 0   | 4    | magic          | ASCII `"WFRM"` = 0x57 0x46 0x52 0x4D                         |
| 4   | 4    | payload_len    | live bytes in this slot (`<= payload_cap`)                   |
| 8   | 8    | seq            | `u64`; equals the `producer_seq` value **after** this commit |
| 16  | 8    | timestamp_ns   | `u64` capture timestamp (monotonic epoch, ns)                |
| 24  | 4    | duration_us    | producer hint (e.g. 8333 µs for 120 fps)                     |
| 28  | 4    | flags          | bit0 = COMMITTED (set **last**), others reserved             |
| 32  | 4    | fourcc         | LE ASCII payload format: `"RGBA"`, `"BGRA"`, `"I420"`, `"PCM "`, `"F32 "`, `"RAW "` |
| 36  | 1    | rank           | dimensions used (`<= 8`)                                     |
| 37  | 1    | planes         | `1` in V1 (multi-plane reserved)                             |
| 38  | 2    | reserved       | 0                                                            |
| 40  | 12   | plane_offset[3]| `u32` ×3; V1: `[0]=0`, others 0                              |
| 52  | 12   | plane_size[3]  | `u32` ×3; V1: `[0]=payload_len`, others 0                    |

## 4. Publish protocol (producer commit)

For the frame with sequence number `seq` (1-based; the first committed frame is
`seq = 1`):

1. `slot = (seq - 1) % slot_count`, `base = header_size + slot * slot_stride`
2. Write payload into `[base + 64, base + 64 + payload_len)`.
3. Write the slot header fields **with flags bit0 = 0** (torn-read marker).
4. Write `seq`, `timestamp_ns`, `payload_len`, `fourcc`, ... ; **last**, set
   flags bit0 = 1 (COMMITTED).
5. `Atomics.store` (SeqCst) the u64 `producer_seq <- seq`. This store is the
   publish fence. Python uses the same protocol; on platforms without 64-bit
   atomics (non-SAB ArrayBuffer in the browser), the TS implementation falls
   back to a DataView store + bounded seqlock re-read, which is still correct
   for single-producer publication on the 64-bit LE targets we support.

## 5. Acquire protocol (consumer, wait-free)

`acquireLatest()`:

1. `s = Atomics.load(producer_seq)` (acquire). If `s == lastSeen`, nothing new.
2. `slot = (s - 1) % slot_count`; read slot `seq2`, magic, flags.
3. If `seq2 != s - 1` or magic mismatch or bit0 unset → **bounded retry** from
   step 1 (at most `slot_count` attempts), else return `null` (overrun window).
4. Bind a reused flyweight view to `[base + 64, base + 64 + payload_len)`.

The reader never allocates and never blocks the producer. If the producer laps
the slot mid-read, the seq check detects it (torn read) and the caller simply
gets the next frame — frame loss is reported, never corruption.

## 6. Laws enforcement map (Pillar 2 briefing)

| Law | Meaning (this pillar)                     | Mechanical enforcement                                                                  |
|-----|-------------------------------------------|------------------------------------------------------------------------------------------|
| 1   | Zero allocation in ingestion/render loop  | Per-slot preallocated views at attach; heap-delta probes (`--expose-gc`) over 100k+ ops; alloc-site scan in CI shard |
| 2   | Strict LE + IEEE 754 parity               | Byte-level magic compare; DataView/struct `<` with explicit LE on every access; f16 codec vectors vs numpy; golden fixtures cross-read by TS and Python |
| 3   | Browser / Node / Deno / Electron          | Core = standard ESM + Web primitives only (`DataView`, `Atomics`, `performance.now`); zero `node:` imports in `src/`; runtime matrix detector + tests |
| 4   | Boundary schema validation                | `validateRingHeader` (magic/version/size/crc/shape/strides/dtype) at attach; `validateSlotHeader` on acquire; fail-closed corruption matrix tests |

## 7. Golden fixtures

Deterministic fixtures (committed, reproducible via
`scripts/make_ring_fixture.py` with fixed seeds and dyadic-only floats):

- `fixtures/weft-tensor/ring-v1-f32.bin` — dtype F32, shape `[2,3]`,
  slot_count 4, 10 frames; `payload[i] = (seq*10 + i) * 0.25`,
  `timestamp_ns = seq * 1_000_000`.
- `fixtures/weft-tensor/ring-v1-u8.bin` — dtype U8 (RGBA), shape `[2,2,4]`,
  slot_count 4, 6 frames; `payload[i] = (seq*37 + i*11) & 0xFF`.

Both TS and Python suites read **both** fixtures and assert byte-exact equality
(header fields, payloads, CRC). The integration pipeline additionally generates
a fresh ring from the *TS* producer and validates it from Python, and vice
versa — 8 stages, fail-closed.

## 8. Relationship to the frozen kernel envelope

The kernel envelope (`core/c/weft.h`: magic `WEFT`, u32 version/header_size,
u64 seq, u32 payload_len — all LE) remains byte-frozen and untouched. WTR1 is a
**sibling plane** for tensor payloads: the ring header re-uses the same magic
and LE discipline, slots are the tensor analogue of envelopes, and
`schema_id` plays the role of the IR schema identity from Pillar 1. Native
adapters (Engineer 1/2) can fill WTR1 rings directly; nothing in this pillar
modifies the kernel.
