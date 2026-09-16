# weft_flutter

High-performance zero-copy Triad Protocol channel bindings for Flutter and Dart via `dart:ffi`.

## Features
- **Zero Steady-State Allocation**: Pure C-kernel memory sharing without Dart heap allocation during render loops.
- **Paint-Phase Deferred Reads**: `WeftPainter` claims and reads data exclusively in Flutter's paint pass, bypassing layout and build invalidations.
- **Fan-Out (RFC 0004)**: `WeftFanoutFFI` + `WeftFanoutReaderFFI` + `WeftFanoutPainter` — one writer, N independent consumers on one stream (primary canvas, minimap, flight recorder, network visualizer), each reader with its own drop accounting. The ring is the SAME byte-compatible C implementation the TS port and the Android JNI surface use; readers may attach to any byte-compatible native memory, and consumers may live in other isolates (reader handle passed as its raw address).
- **Cross-Platform Support**: Linux, Android, macOS, Windows, and iOS.

## Honesty Disclosure
> [!NOTE]
> Per docs/PORTS.md §3 and Directive 14:
> The pure Dart reference port is a single-isolate reference implementation.
> Cross-thread shared memory requires `dart:ffi` into the native C kernel (`libweft.so`).
> Hardware frame pacing and thermal behavior on physical devices are unverified by design in this build suite.
> Fan-out FFI bindings are CI-GATED (D-12/D-14 precedent): the Flutter/Dart
> toolchain is not available in the contributor sandbox — `test/fanout_ffi_test.dart`
> (DF-series + cross-isolate torture) is verified by this repository's CI
> against the same `libweft.so` the workflow compiles from `core/c/weft.c` +
> `core/c/fanout.c`.
