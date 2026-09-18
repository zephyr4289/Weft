<!-- Cites: 02-KERNEL, RFC-0001, WO-P4 decision 4, PORTS.md §3 -->
# Dart/Flutter Port — Weft

> **STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION**
>
> **SINGLE-ISOLATE REFERENCE: no cross-thread ordering claims transfer from
> the C/Rust proof.** This is the honesty load-bearing wall of the Dart port.
> A reader must not be able to mistake the pure-Dart kernel for a concurrency proof.

## Why exists

Implements the Triad Protocol as a **single-isolate reference implementation**.
Dart isolates share no memory and the language has no atomics. The exchange
maps to plain field assignment (`latest = wWork`), valid only because writer
and reader run on one event loop (WO-P4 decision 4).

## Forward interface (specified, not implemented)

Production Flutter usage goes through `dart:ffi` to the C kernel:
```dart
// import 'dart:ffi';
// final dylib = DynamicLibrary.open('libweft_core.so');
// final weftInit = dylib.lookupFunction<...>('weft_init');
```
This bridge is not implemented — guessing ABIs without a compiler is how phantom APIs get born.

## Files

| File | Role |
|---|---|
| `weft.dart` | Kernel: single-isolate reference, plain field assignment exchange |
| `steward.dart` | Lifecycle: StatefulWidget-scoped, GC-based release |
| `heddle.dart` | Draw-phase: CustomPainter + dart:ffi forward interface |

## AXIOM T

Telemetry counters are advisory. No port logic branches on a telemetry counter.
