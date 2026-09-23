# MANAGED SEAMS V1 — Pillar 6 (weft-adapters) Normative Wire Contracts

Status: **NORMATIVE** (managed-side contract-first, WCN1/SHP1/HPL1 precedent)
Owner: Engineer 3 (Managed Runtimes / Reactive UI / Developer Ergonomics / Client SDKs)
Consumes: Engineer 1's `core/c/include/weft_adapters.h` + ITCH 5.0 / SBE binary
protocols (via FFI / WASM imports / N-API only — **zero core C authored here**),
Engineer 2's `rmw_weft` shared-memory transport + V4L2/DMA-BUF grabber (via
native dynamic dispatch symbols only).

> **Contract-first rule.** `weft_adapters.h` and `rmw_weft` are not merged on
> `main` at the time of writing. Every managed binding is therefore written
> against the byte-frozen seams defined in THIS document, with an explicit
> adapter seam (`attach*`) that accepts Engineer 1/2's native handles when
> they land — no managed-code changes required at integration time.

---

## 1. Managed seam inventory

| Seam  | Purpose                                       | Consumes                                  | Doc § |
|-------|-----------------------------------------------|-------------------------------------------|-------|
| ITCH5 | NASDAQ TotalView-ITCH 5.0 flyweight parsing   | E1 decoder C-ABI (or self-contained fallback) | §2 |
| SBE   | Simple Binary Encoding flyweight decoding     | E1 schema registry                        | §3 |
| MDP1  | Level-2/3 aggregated order-book snapshot wire | our own (parity + UI binding)             | §4 |
| RNG1  | Robotics/vision shared-memory record ring     | E2 `rmw_weft` ring mapping                | §5 |
| FRM1  | Camera frame descriptor (DMA-BUF handle path) | E2 V4L2/DMA-BUF grabber                   | §6 |

Byte order: ITCH 5.0 and SBE wire inputs are **big-endian** (network order)
per their public specs, EXCEPT SBE schemas that explicitly declare
`littleEndian` (SBE default). MDP1 and RNG1 are **little-endian** (our wires,
consistent with SHP1/HPL1).

---

## 2. ITCH5 — ITCH 5.0 managed profile

Framing: each message on the wire is `[u16 be length][payload]`. The parser
MUST advance by the length prefix and MUST skip (never fail) unknown message
types — fail-closed is reserved for *corrupt framing* (length beyond buffer),
which yields `E_TRUNC`.

Managed profile (decoded message types; sizes from the public NASDAQ TotalView
ITCH 5.0 spec; all fields big-endian; prices are integer ticks, 4 implied
decimal places):

| Type | Name                  | Size (B) | Managed fields (offset) |
|------|-----------------------|----------|--------------------------|
| `S`  | SystemEvent           | 36       | event_code u8(11) |
| `A`  | AddOrder              | 36       | ref u64(11) side u8(19) shares u32(20) stock char[8](24) price u32(32) |
| `F`  | AddOrderMPID          | 40       | A fields + attribution char[4](36) |
| `E`  | OrderExecuted         | 31       | ref u64(11) shares u32(19) match u64(23) |
| `C`  | OrderExecutedWithPrice| 36       | ref u64(11) shares u32(19) match u64(23) printable u8(31) price u32(32) |
| `X`  | OrderCancel           | 23       | ref u64(11) cancelled u32(19) |
| `D`  | OrderDelete           | 19       | ref u64(11) |
| `U`  | OrderReplace          | 35       | orig u64(11) new u64(19) shares u32(27) price u32(31) |
| `P`  | TradeNonCross         | 44       | ref u64(11) side u8(19) shares u32(20) stock char[8](24) price u32(32) match u64(36) |

Common prefix (all types): `type u8(0), locate u16(1), tracking u16(3),
ts_ns u48(5)` — timestamp is nanoseconds-since-midnight, 6 bytes.

**Flyweight law.** A message view binds once to a buffer and is re-bound by
offset; per-message decode MUST NOT allocate (no slices, no strings, no
objects). Strings (stock symbols) are read as raw 8 bytes into a caller-owned
scratch (Latin-1 bytes; decode to string only on cold paths).

**Zero-extend u48 timestamp:** `hi = u16@(5)`, `lo = u32@(7)`,
`ts = hi * 2**32 + lo` (integer math only; below 2^53 in fixtures → exact as
JS double, exact as Python int).

## 3. SBE — managed flyweight decoding

Decoder is schema-driven. A schema is a JSON descriptor (fixtures carry ours;
E1's registry plugs in via `attachSchemaRegistry`):

```json
{ "semanticVersion": "5.2", "littleEndian": true,
  "types": { "MsgHeader": { "blockLength": 8, "fields": [
      {"name":"blockLength","type":"u16","offset":0},
      {"name":"templateId","type":"u16","offset":2},
      {"name":"schemaId","type":"u16","offset":4},
      {"name":"version","type":"u16","offset":6} ] } },
  "messages": { "1001": { "name": "BookRefresh", "blockLength": 24, "fields": [
      {"name":"seq","type":"u64","offset":0},
      {"name":"tsNs","type":"u64","offset":8},
      {"name":"bidPrice","type":"i64","offset":16} ] } } }
```

Rules: `u8,u16,u32,u64,i8,i16,i32,i64` fixed fields only for V1; per-message
`blockLength` gates the fixed body; trailing extension fields beyond
`blockLength` are skipped by length (SBE versioning). Flyweight: bind once,
`unpack(templateId)` re-reads in place; zero allocation in steady state.

## 4. MDP1 — aggregated book snapshot wire (our contract)

Purpose: (a) cross-language parity artifact, (b) the object `<WeftOrderBook />`
binds to (zero-copy, see §7). Fixed 304 bytes, **little-endian**:

```
off   size  field
0     4     magic "MDP1"
4     2     version u16 = 1
6     2     flags u16   (bit0 = book_valid, bit1 = crossed, bit2 = locked)
8     8     seq u64     (messages applied since attach)
16    8     last_ts_ns u64
24    4     best_bid_raw u32   (0 = empty side)
28    4     best_ask_raw u32
32    120   bid_levels[10] × {price_raw u32, agg_size u32, orders u32}
152   120   ask_levels[10] × {price_raw u32, agg_size u32, orders u32}
272   8     msg_count u64
280   8     trade_count u64
288   8     last_match u64
296   4     crc32 u32   (CRC-32/ISO-HDLC, poly 0xEDB88320 reflected,
                         init 0xFFFFFFFF, final xor 0xFFFFFFFF,
                         over bytes [0, 296))
300   4     reserved (0)
```

Empty level = `{0,0,0}`. Levels sorted best-first (bids descending, asks
ascending). All-integer state — **no floats on the parity path** (Law:
cross-language bit-determinism).

Snapshot cadence is caller-driven (`book.writeSnapshotInto(scratch)`), never
per-message. HUD/telemetry reads MDP1 directly — the React component NEVER
receives book state through props or state (§7).

## 5. RNG1 — robotics record ring (our contract)

Shared-memory ring consumed by robotics bindings; `rmw_weft` maps onto it at
integration via `attachRing(memory, opts)`. Layout, **little-endian**:

**Ring header — 128 B at offset 0:**

```
off   size  field
0     4     magic "RNG1"
4     2     version u16 = 1
6     2     header_size u16 = 128
8     4     slot_size u32        (power of two, >= 256)
12    4     slot_count u32       (power of two)
16    8     write_seq u64        (seqlock: odd = write in flight)
24    8     committed_seq u64    (last fully published record seq)
32    8     drop_count u64       (writer overruns)
40    8     overwrite_count u64
48    16    reserved (0)
64    32    topic_table[8] × u32 topic_id (0 = free)
96    32    reserved (0)
```

**Slot — `slot_size` B each, at offset `128 + i * slot_size`:**

```
off   size  field
0     8     seq u64      (matches global record seq when committed; odd = torn)
8     4     len u32      (payload bytes, <= slot_size - 64)
12    4     topic_id u32
16    8     ts_ns u64
24    4     fmt u32      (see §6/§5.1)
28    4     flags u32
32    32    reserved (0)
64    ...   payload
```

Reader protocol (drop-not-block — Law 4 taxonomy family): read `seq` (acquire)
→ if odd or `seq != expected continuous window` → **torn/skipped, count and
continue**; read payload; re-read `seq` (acquire) → changed → torn. A reader
NEVER blocks, allocates, or throws on a torn record.

### 5.1 fmt codes

| fmt | name        | payload                                                  |
|-----|-------------|----------------------------------------------------------|
| 1   | IMU6DOF     | 8×f64 LE: ts_ns, qw, qx, qy, qz, gyro_x, gyro_y, gyro_z  |
| 2   | POINTS_F32  | N×3×f32 LE xyz triples (N = len/12)                      |
| 3   | FRAME_DESC  | FRM1 descriptor (§6)                                     |
| 4   | BOXES_F32   | M×6×f32 LE: x,y,z,w,h,score (detector output)            |

## 6. FRM1 — camera frame descriptor

Large frames are never inlined into the ring. A `FRAME_DESC` payload is a
handle (DMA-BUF fd / IOSurface id / platform handle from Engineer 2's
grabber) plus geometry; the managed side wraps the *mapped memory* the
handle points at:

```
off  size  field
0    4     magic "FRM1"
4    4     width u32
8    4     height u32
12   4     stride u32
16   4     format u32  (1 = RGB8, 2 = BGR8, 3 = NV12, 4 = GRAY8)
20   4     handle_ns   (platform-specific: fd / id)
24   4     handle_lo
28   4     flags u32
```

`camera.get_tensor()` returns a zero-copy DLPack producer over the mapped
arena slot. **Pointer identity is the acceptance proof**: the tensor's data
pointer MUST equal the arena slot address (asserted in tests; numpy lane in
sandbox, torch lane in CI workflow).

## 7. Law 4 — UI zero-re-render & zero-GC taxonomy (adapters edition)

| Code | Violation                                                       |
|------|------------------------------------------------------------------|
| W4-01| `setState` / `notifyListeners` / object publish in message path  |
| W4-02| Per-message or per-frame allocation in steady state              |
| W4-03| Blocking the UI thread on ring/feeder read (poll→spin bounded)   |
| W4-04| Unhandled throw escaping tier degradation (must degrade silently)|
| W4-05| Torn record rendered without drop accounting                     |
| W4-06| React reconciliation triggered by telemetry/book updates         |

`<WeftOrderBook />`, `<WeftPointCloudViewer />`, `<WeftAttitudeIndicator />`,
`WeftOrderBookWidget` (Flutter) and `WeftOrderBookView` (SwiftUI) bind to
MDP1/RNG1 buffers through typed-array/DataView direct channels; React
components render exactly once and mutate canvas/DOM-node internals from the
rAF loop. Transparent degradation: renderer context loss (`E_CONTEXT_LOST`)
or feeder failure downgrades to a static snapshot + FALLBACK banner; never an
uncaught exception (W4-04).

## 8. Integration seams (Engineer 1 / 2 attach points)

- `attachAdaptersNative(handle)` — accepts E1's `weft_adapters.h` C-ABI
  (FFI/WASM imports/N-API). Until then all managed decoders are
  self-contained pure implementations of §2/§3 — byte-identical either way
  (parity fixtures prove it).
- `attachRing(memory)` — accepts E2's `rmw_weft` mapping (SAB/`ArrayBuffer`
  or Python `memoryview`). Managed readers are mapping-agnostic.
- `attachFrameFeeder(feeder)` — accepts E2's V4L2/DMA-BUF grabber; the
  feeder supplies mapped memory + FRM1 descriptors.
