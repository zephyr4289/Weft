// weft_reference.dart — Triad Protocol kernel (Dart/Flutter reference port)
//
// WHY EXISTS: Implements the Triad Protocol as a SINGLE-ISOLATE REFERENCE
// implementation per docs/PORTS.md §3 and WO-P4 decision 4.
// Single-isolate reference; no cross-thread ordering claims transfer from the C/Rust proof. The pure-Dart kernel is a protocol reference implementation valid for single-threaded usage (e.g., a Worker isolate that runs both writer and reader on its own event loop). Production Flutter usage goes through `dart:ffi` to the C kernel. This is the honesty load-bearing wall of the Dart port.
//
// STATUS: SOURCE-ONLY REFERENCE IMPLEMENTATION.
// Verification is build/unit/CI-only per owner pivot; cross-device hardware verification permanently withdrawn.

export 'reference/weft.dart';
export 'reference/steward.dart';
export 'reference/heddle.dart';
export 'reference/frame_cursor.dart';
export 'reference/fanout.dart';
