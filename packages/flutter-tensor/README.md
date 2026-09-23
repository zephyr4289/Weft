# weft_flutter_tensor

Real-time AI tensor widgets over **WTR1** (Weft Tensor Ring v1) shared-memory
rings via `dart:ffi` — **zero-GC AI overlay rendering** for Flutter at display
refresh rate (Pillar 2, Deliverable D, Flutter sub-build).

Spec: [`docs/weft-tensor/LAYOUT-V1.md`](../../docs/weft-tensor/LAYOUT-V1.md)
(NORMATIVE). TypeScript reference: [`packages/weft-tensor/src/layout.js`](../../weft-tensor/src/layout.js)
/ [`ring.js`](../../weft-tensor/src/ring.js) — the Dart constants and protocol
are byte-exact parity with that file, enforced mechanically by
`test/static_audit.mjs`.

## What runs where

| Component | Runtime | Notes |
|---|---|---|
| `ring_header.dart` | **Plain Dart VM** (no flutter/ffi imports) | fixture tooling, CI shards, tests, servers |
| `weft_ring.dart`   | Flutter mobile / desktop (dart:ffi)   | attach to native/mmap'd ring memory |
| `weft_video_overlay.dart` + painters | Flutter render tree | Ticker-driven overlay at display refresh |
| Web (Flutter web)  | **not supported** | dart:ffi + 64-bit int semantics are VM-only (fail-closed by design) |

## Install

```yaml
dependencies:
  weft_flutter_tensor:
    path: packages/flutter-tensor
```

## Quick start

```dart
import 'package:weft_flutter_tensor/weft_flutter_tensor.dart';

// Attach (cold path — full Law-4 validation, throws LayoutException on any
// corruption class with TS-identical codes, e.g. WTR1_BAD_MAGIC).
final ring = WeftRing.attachPointer(ptr, byteLength); // dart:ffi memory
// ...or: WeftRing.attachByteData(bd) for in-memory/fixture rings.

final notifier = WeftTensorNotifier(); // zero-alloc Listenable
final scratch = OverlayScratch();      // detector output == painter input

// UI — repaint WITHOUT rebuilding the widget tree:
WeftVideoOverlay(
  ring: ring,
  scratch: scratch,
  onFrame: (view) {
    // validated view (seqlock passed); fill scratch in place, zero alloc
    scratch.clear();
    scratch.pushBox(40, 60, 64, 48, 0.91, 0);
  },
)

// Widget-internal machinery (what WeftVideoOverlay does per tick):
final view = ring.acquireLatest();          // REUSED flyweight or null
if (view != null && view.seq != lastSeq) {
  lastSeq = view.seq;                       // plain int reads
  notifier.frame();                         // -> CustomPaint(repaint: notifier)
}
```

## API

### `WeftRing` (`lib/src/weft_ring.dart`)
- `attachPointer(Pointer<Uint8>, int byteLength)` / `attachByteData(ByteData)`
  — one-time validation (magic, version, header_size, slot_count,
  slot_stride alignment, dtype, elem_size, rank, row-major strides, seq
  bound, little-endian flag, CRC-32/IEEE, capacity).
- `acquireLatest({int? afterSeq})` → `WeftTensorView?` — wait-free seqlock
  read into a **reused** flyweight; bounded retry (`slot_count` attempts),
  `null` on torn/overrun window. **Zero allocation.**
- `acquireFrame(int seq)` — exact-sequence acquire (null if lost/not yet).
- `producerSeq` — publish word (u64 lo/hi, composed exactly, < 2^53 bound).
- `stats` — `{commits, acquireCalls, tornReads, overruns}` counters.
- `beginCommit()` / `finishCommit(len, {...})` / `commit(bytes, {...})` —
  TS-parity producer path (COMMITTED bit written last; `producer_seq` hi
  then lo, **lo word last** = publish fence).

### `WeftTensorView` (`lib/src/weft_tensor_view.dart`)
Flyweight bound in place every acquire: `seq`/`seqLo`/`seqHi`,
`timestampNs` (exact int — Dart VM ints are 64-bit, ideal for u64 ns),
`durationUs`, `fourccCode`, `payloadView` (preallocated slot-capacity
`Uint8List`; live bytes = `payloadLen`), plus header-derived `shape`,
`strides` (elements), `rank`, `dtypeCode`/`dtypeBits`/`elemSize`.

### `OverlayScratch` + `WeftTensorPainter` (`lib/src/weft_tensor_painter.dart`)
- `OverlayScratch` — one `Float32List`, box stride 6
  (`x, y, w, h, score, classId`), `pushBox`/`clear`/`boxAt` — byte-parity
  with TS `render/overlay.js` (stride 6, max 256).
- `coco17Edges` — COCO-17 skeleton edge pairs, value-identical to TS.
- `drawBoxes` / `drawSkeleton` — allocation-free draw helpers over reused
  `Paint`s.
- `WeftTensorPainter(ring, notifier, {boxPaint, textPainter, scratch,
  keypoints})` — acquires the latest frame **in `paint()`** (draw-phase
  deferred read), draws boxes/skeleton from caller-owned scratch.
  `shouldRepaint => false` — the notifier drives repaints.

### `WeftTensorNotifier` (`lib/src/weft_tensor_notifier.dart`)
Fixed-capacity listener storage (adapted from Pillar 1's
`flutter_weft`/`weft_notifier.dart`): `frame()` is an indexed loop — zero
allocation; `addListener` doubling growth is cold-path-only; `removeListener`
swap-removes. `CustomPaint(repaint: notifier)` + `notifier.frame()` replaces
`setState` entirely.

## Zero-GC guarantees (Law 1)

Allocated **once at attach**, reused forever: the `ByteData` window, per-slot
payload `Uint8List`s, `SlotHeaderInfo` scratch, `WeftTensorView` flyweight,
stats struct, commit handle, notifier listener array, `Paint` objects,
`OverlayScratch` buffer. The acquire → notify → paint loop performs **no
allocations** on any of those. Enforced mechanically by `test/static_audit.mjs`
(checks C8–C10 scan `paint()`, `_onTick`, `frame()` for allocation patterns).

Two honest exceptions, documented in-code:
1. Flutter's `Rect`/`Offset` value records passed to `Canvas.drawRect` — the
   Canvas API has no in-place rect primitive; these are small short-lived
   engine records (no view/list/string churn).
2. The **opt-in** label path (`textPainter`) builds a `TextSpan` + string per
   label. Leave it `null` for a strictly zero-GC loop.

## WTR1 layout reference (Law 2: strict little-endian)

Ring header (128 B): magic `"WEFT"` @0 · `layout_version` u16=1 @4 ·
`header_size` u16=128 @6 · `slot_count` u32 @8 · `slot_stride` u32 @12 ·
`dtype_code` u8 @16 · `dtype_bits` u8 @17 · `lanes` u16 @18 · `elem_size` u32
@20 · `shape[8]` @24 · `strides[8]` (elements) @56 · `schema_id` u64 @88 ·
`producer_seq` u64 @96 (publish word) · `tick_hz` u32 @104 · `flags` u32 @108
(bit0 = little-endian) · `header_crc` u32 @112 (CRC-32/IEEE over bytes
`[0,96) ++ [104,112)`).

Slot header (64 B): magic `"WFRM"` @0 · `payload_len` u32 @4 · `seq` u64 @8 ·
`timestamp_ns` u64 @16 · `duration_us` u32 @24 · `flags` u32 @28 (bit0 =
COMMITTED) · `fourcc` u32 @32 · `rank` u8 @36 · `planes` u8 @37 ·
`plane_offset[3]` @40 · `plane_size[3]` @52. Slot `i` lives at
`128 + i * slot_stride`; payload at `+64`.

Every multi-byte access in this package passes `Endian.little` explicitly —
the audit fails CI otherwise.

## CRC known-answer vectors (derived, not guessed)

Computed with `node:zlib` `crc32` (Node ≥ 20.15, authoritative zlib
CRC-32/IEEE) and embedded in `test/ring_logic_test.dart`:

| Input | Value |
|---|---|
| `crc32([0x57,0x45,0x46,0x54])` (ASCII `"WEFT"`) | `3421166146` (`0xCBEADA42`) |
| fixed KAT2 header, region `[0,96)+[104,112)` | `1553622279` (`0x5C9A6507`) |
| `crc32(0..255 ascending)` | `688229491` (`0x29058C73`) |

`test/static_audit.mjs` **re-derives all three with zlib at audit time** and
diffs the embedded numbers (check C06), so the vectors cannot drift.

## Testing

```bash
node test/static_audit.mjs      # 18 structural checks, no Dart SDK needed
dart run test/ring_logic_test.dart  # pure-VM unit tests (CI-gated; needs Dart ≥ 3.4)
flutter test                    # (CI) once a Flutter toolchain is available
```

`test/ring_logic_test.dart` is a dependency-free standalone harness (no
flutter/test packages) covering: CRC KATs, valid-header parse, the full
fail-closed corruption matrix (`WTR1_BAD_MAGIC`, `WTR1_BAD_VERSION`,
`WTR1_BAD_HEADER_SIZE`, `WTR1_BAD_SLOT_COUNT`, `WTR1_BAD_SLOT_STRIDE`,
`WTR1_BAD_DTYPE`, `WTR1_BAD_ELEM_SIZE`, `WTR1_BAD_RANK`, `WTR1_BAD_STRIDES`,
`WTR1_SEQ_OVERFLOW`, `WTR1_NOT_LITTLE_ENDIAN`, `WTR1_BAD_CRC`, `WTR1_SHORT`,
`WTR1_SHORT_RING`), slot-header validation, and fourcc/dtype helpers.

## Known limitation: pixel (RGBA) blit

`WeftTensorPainter` draws **vector overlays** (boxes/skeletons) with zero
copies. Blitting the frame payload itself (e.g. an RGBA camera plane) to the
screen today requires `dart:ui` `ImageDescriptor`/`ImmutableBuffer`, which
copies the bytes — the **one unavoidable copy**:

```dart
final buffer = await ImmutableBuffer.fromUint8List(view.payloadView); // copies
final descriptor = await ImageDescriptor.raw(buffer, width: w, height: h,
    pixelFormat: PixelFormat.rgba8888);
final image = (await descriptor.instantiateCodec()).getNextFrame();
canvas.drawImage(image.image, Offset.zero, paint);
```

The long-term zero-copy path is a **texture-uploader adapter** (Engineer 2's
native adapters register the ring's slot memory as a GPU texture id; Flutter
composites it via `Texture` widget) — no bytes ever cross into Dart. WTR1's
stable slot geometry (`plane_offset[3]` / `plane_size[3]`) exists precisely so
such adapters can address planes without re-parsing.

## Example

`example/lib/main.dart` — builds a synthetic WTR1 ring in memory (CRC'd
header), commits simulated 120 Hz frames via `Timer`, runs a deterministic
fake detector into `OverlayScratch`, and renders `WeftVideoOverlay`. The only
`setState` is a 1 Hz stats line (lifecycle-level, never per frame).
