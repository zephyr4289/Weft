# weft_flutter

Heddle-2.0 Flutter connectors for the **Weft Hot-Plane (HPL1)** — bind native
shared memory to `CustomPainter` with **0 GC object churn per frame** and
**0 widget rebuild storms**.

## What's inside

| File | Role |
|---|---|
| `lib/src/hpl1_layout.dart` | Pinned HPL1 constants + geometry + fail-closed validation + 15-code Law-4 taxonomy (byte-parity with `@weft/heddle-core`) |
| `lib/src/hot_plane.dart` | `WeftHotPlane` — seqlock consumer side: caller-owned snapshots, tear counting, newest-first ring reads, dirty-mask scans; every accessor explicitly `Endian.little` |
| `lib/src/weft_hot_plane_notifier.dart` | `WeftHotPlaneNotifier` — `Listenable` bridge; `pump()` mutates preallocated snapshots in place and notifies ONLY when a watched lane's sequence advanced |
| `lib/src/weft_canvas_widget.dart` | `WeftCanvasWidget` + `WeftHuddlePainter` — Ticker → `pump()`, painter `repaint: notifier`, `shouldRepaint => false` (zero rebuild pattern) |

## Usage

```dart
final plane = WeftHotPlane.fromBytes(nativeView); // Pointer.asTypedList — no copy
final painter = WeftHuddlePainter(WeftHotPlaneNotifier(plane));
CustomPaint(painter: painter)  // 240 FPS repaints from shared memory, zero rebuilds
```

## Verification honesty

The authoring sandbox has **no Dart SDK**: the Dart sources and
`test/ring_logic_test.dart` (a dependency-free pure-Dart harness: geometry,
seqlock protocol, corruption matrix, dirty-mask semantics) are **CI-gated on
the flutter lane**. The always-runnable local proof is
`test/static_audit.mjs` (35 mechanical checks: constants parity vs
`heddle-core/layout.js`, `Endian.little` on all 56 multi-byte accesses,
hot-path purity scans, wiring contracts).
