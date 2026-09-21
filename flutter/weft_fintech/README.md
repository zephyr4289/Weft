# weft_fintech (Flutter)

Weft FinTech Flutter connectors — **`WeftOrderBookWidget`**, a 240 FPS
MDP1 depth ladder driven through a zero-copy controller seam. Part of
Pillar 6 (weft-adapters), deliverable A (mobile lane).

## What it is

```
producer (native ring attach / demo synthesizer)
    │  offer(Uint8List mdp1Record)   ← reference pass, NO copy
    ▼
WeftOrderBookController ── consume() on ONE Ticker
    │  validate() fail-closed (short → magic → version → CRC)
    │  edge-trigger: notifier.notifyIfChanged(seq)
    ▼
CustomPaint(repaint: notifier) ── WeftOrderBookPainter
       20 bars from ONE Float32List scratch, ONE reused Paint
       shouldRepaint ⇒ false (repaints are notifier-driven only)
```

- **Zero rebuild storms**: the widget builds ONCE; feed updates never
  call `setState`. The edge-triggered notifier marks the painting dirty
  only when the MDP1 `seq` actually advanced.
- **Zero-GC geometry path**: `paint()` allocates nothing — bar rects are
  computed into a preallocated `Float32List` and painted with reused
  `Paint` objects. The label path (`paintLabels: true`) is opt-in and
  documented as allocating (Pillar 4 label lesson).
- **Book state never crosses into widget state**: the snapshot is a
  flyweight (`Mdp1Snapshot.retarget`) over the producer's bytes — the
  same property the React `<WeftOrderBook />` proves on web (Law 4,
  W4-06).

## MDP1 wire

304-byte little-endian record (see
`docs/adapters/MANAGED-SEAMS-V1.md` §4 and `packages/fintech/src/mdp1.js`):

| Offset | Field |
|-------:|-------|
| 0      | magic `"MDP1"` |
| 4      | version u16 |
| 6      | flags u16 (`F_BOOK_VALID` / `F_CROSSED` / `F_LOCKED`) |
| 8      | seq u64 |
| 16     | last_ts_ns u64 |
| 24     | best_bid u32 |
| 28     | best_ask u32 |
| 32     | bid_levels[10] ×12 {price u32, size u32, orders u32} |
| 152    | ask_levels[10] ×12 |
| 272    | msg_count u64 · trade_count u64 · last_match u64 |
| 296    | crc32 u32 (poly `0xEDB88320`, over `[0,296)`) |
| 300    | reserved u32 |

Every multi-byte access in this package passes `Endian.little`
explicitly; u64 fields compose as `lo + hi * 2^32` (exact Dart ints).

## What runs where

| Piece | Runs on | Verified by |
|-------|---------|-------------|
| `lib/src/mdp1_wire.dart` | any Dart VM | `test/mdp1_logic_test.dart` (pure Dart, `dart run`) |
| notifier / painter / widget | Flutter runtime | `test/static_audit.mjs` (structural) + flutter CI compile lane |
| constants parity | — | audit diffs 11 constants vs `packages/fintech/src/mdp1.js` |
| CRC KATs | — | re-derived from `node:zlib` inside the audit |

## Usage

```dart
final controller = WeftOrderBookController();

// producer side (e.g. a ring-attach pump, or the demo synthesizer):
controller.offer(mdp1Bytes);

Widget build(context) => WeftOrderBookWidget(controller: controller);
```

## Verification honesty

No Flutter toolchain exists in the managed sandbox: `mdp1_logic_test.dart`
is a dependency-free pure-Dart harness, and `test/static_audit.mjs`
mechanically pins the compile-checkable properties (Law-2 Endian scan,
paint/consume purity, `shouldRepaint ⇒ false`, one-ticker lifecycle,
no-`setState` frame path, constants parity, zlib-derived CRC KATs). The
Dart AOT/JIT compile itself is gated on the flutter CI lane.
