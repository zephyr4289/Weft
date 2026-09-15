// heddle.dart — Draw-phase binding for Flutter (Dart)
//
// WHY EXISTS: Binds a Weft to the Flutter Draw phase via CustomPainter.
// Per 02-KERNEL §4.2: reads a Weft during the Draw phase only. Per WHITEPAPER
// §8.1: Compose draw-phase reference. SINGLE-ISOLATE: same constraints as
// weft.dart.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'weft.dart';

class WeftPainter extends CustomPainter {
  final Weft weft;
  final void Function(Canvas, Uint8List) draw;

  WeftPainter(this.weft, this.draw);

  @override
  void paint(Canvas canvas, Size size) {
    weft.claim();
    final buf = weft.rReadSlice(16, weft.payloadMax);
    draw(canvas, buf);
  }

  @override
  bool shouldRepaint(covariant WeftPainter old) => old.weft != weft;
}

/// Forward interface (dart:ffi binding to the C kernel) — SPECIFIED, NOT IMPLEMENTED.
///
/// Production Flutter usage goes through dart:ffi to the C kernel ABI:
/// ```dart
/// // import 'dart:ffi';
/// // final dylib = DynamicLibrary.open('libweft_core.so');
/// // final weftInit = dylib.lookupFunction<...>('weft_init');
/// // ... etc.
/// ```
/// This bridge is not implemented here — guessing ABIs without a compiler
/// is how phantom APIs get born (WO-P4 decision 4).
