// weft_reference.dart — Triad Protocol kernel (Dart/Flutter reference port)
//
// WHY EXISTS: Implements the Triad Protocol as a SINGLE-ISOLATE REFERENCE
// implementation per docs/PORTS.md §3 and WO-P4 decision 4.
// Single-isolate reference: no cross-thread ordering claims transfer from the C/Rust proof.
// Production Flutter usage goes through dart:ffi to the C kernel.
//
// STATUS: SOURCE-ONLY REFERENCE IMPLEMENTATION.
// Verification is build/unit/CI-only per owner pivot; cross-device hardware verification permanently withdrawn.

export 'reference/weft.dart';
export 'reference/steward.dart';
export 'reference/heddle.dart';
