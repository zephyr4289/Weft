# Weft Flutter Desktop Example

Demonstrates zero-copy high-performance channel rendering in Flutter desktop via `dart:ffi` and the frozen C kernel (`libweft.so`).

## Honesty Disclosure
> [!NOTE]
> Per docs/PORTS.md §3 and Directive 14:
> The pure Dart reference port is a single-isolate reference implementation.
> Cross-thread shared memory requires `dart:ffi` into the C kernel (`libweft.so`).
> Hardware frame pacing and thermal behavior on physical devices are unverified by design in this build suite.
