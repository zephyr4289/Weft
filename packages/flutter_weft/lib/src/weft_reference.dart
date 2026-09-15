// weft_reference.dart — Triad Protocol kernel (Dart/Flutter reference port)
//
// WHY EXISTS: Implements the Triad Protocol as a SINGLE-ISOLATE REFERENCE
// implementation. Dart isolates share no memory and the language has no
// atomics. The exchange maps to plain field assignment, valid only because
// writer and reader run on one event loop. Per WO-P4 decision 4 and
// docs/PORTS.md §3. Production Flutter usage goes through dart:ffi to the
// C kernel — specified, NOT implemented.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.
// SINGLE-ISOLATE REFERENCE: no cross-thread ordering claims transfer from
// the C/Rust proof. This is the honesty load-bearing wall of the Dart port.

export 'reference/weft.dart';
export 'reference/steward.dart';
export 'reference/heddle.dart';
