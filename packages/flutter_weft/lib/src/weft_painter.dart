// weft_painter.dart — CustomPainter with draw-phase deferred read
//
// WHY EXISTS: Defers reading the Weft channel strictly to Flutter's Paint phase
// (bypassing Build and Layout) per WHITEPAPER §8.4 and DIRECTIVE-14 T14.3.

import 'dart:ffi';
import 'package:flutter/widgets.dart';
import 'weft_ffi.dart';

typedef WeftPaintCallback = void Function(Canvas canvas, Size size, Pointer<Uint8> buffer);

/// CustomPainter that claims and reads a Weft channel strictly inside [paint].
class WeftPainter extends CustomPainter {
  final WeftFFI weft;
  final WeftPaintCallback onPaint;
  final Pointer<Uint8> _readBuffer;
  int _lastSeq = 0;
  int _droppedFrames = 0;

  WeftPainter({
    required this.weft,
    required this.onPaint,
    required Pointer<Uint8> readBuffer,
    super.repaint,
  }) : _readBuffer = readBuffer;

  int get droppedFrames => _droppedFrames;

  @override
  void paint(Canvas canvas, Size size) {
    // 1. Claim freshest buffer from C kernel (Zero Dart allocations)
    final slot = weft.claim();
    final bytesRead = weft.readSlice(_readBuffer, 16, weft.payloadMax);

    if (bytesRead > 0) {
      onPaint(canvas, size, _readBuffer);
    } else {
      _droppedFrames++;
    }
  }

  @override
  bool shouldRepaint(covariant WeftPainter oldDelegate) {
    return true;
  }
}
