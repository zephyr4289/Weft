# weft_flutter

High-performance zero-copy Triad Protocol channel bindings for Flutter and Dart via `dart:ffi`.

## Features
- **Zero Steady-State Allocation**: Pure C-kernel memory sharing without Dart heap allocation during render loops.
- **Paint-Phase Deferred Reads**: `WeftPainter` claims and reads data exclusively in Flutter's paint pass, bypassing layout and build invalidations.
- **Cross-Platform Support**: Linux, Android, macOS, Windows, and iOS.

## Honesty Disclosure
> [!NOTE]
> Per docs/PORTS.md §3 and Directive 14:
> The pure Dart reference port is a single-isolate reference implementation.
> Cross-thread shared memory requires `dart:ffi` into the native C kernel (`libweft.so`).
> Hardware frame pacing and thermal behavior on physical devices are unverified by design in this build suite.
