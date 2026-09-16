// fanout_painter.dart — multi-consumer CustomPainter with draw-phase read
//
// WHY EXISTS: Defers reading the fan-out ring strictly to Flutter's Paint
// phase (bypassing Build and Layout), per WHITEPAPER §8.4 and
// DIRECTIVE-14 T14.3 — the fan-out twin of WeftPainter. The RFC-0004
// motivating scenario on one stream: a primary canvas, a minimap, a flight
// recorder, a network visualizer — N painters, each holding its own
// WeftFanoutReaderFFI over ONE broadcaster, each with its own drop
// accounting (the per-reader HUD this painter exposes).
//
// HOT PATH: paint() is one claim + (on fresh) one view() — zero Dart
// allocation (the claim record is reader-owned native memory; the view is
// the reader's stable copy buffer). shouldRepaint is always true: the
// painter polls latest-wins every paint tick, the same stance as
// WeftPainter.

import 'dart:ffi' hide Size;
import 'package:flutter/widgets.dart';
import 'fanout_ffi.dart';

typedef WeftFanoutPaintCallback = void Function(Canvas canvas, Size size, Pointer<Uint8> view);

/// CustomPainter that claims a fan-out ring strictly inside [paint].
///
/// droppedFrames reports THIS reader's telescoped drops (sum of the claim
/// record's `dropped`): frames the writer completed that this consumer
/// never observed — the RFC-0004 per-reader accounting, exposed as a HUD
/// signal. A non-fresh claim adds nothing (nothing was dropped; nothing
/// newer was available — latest-wins semantics).
class WeftFanoutPainter extends CustomPainter {
  final WeftFanoutReaderFFI reader;
  final WeftFanoutPaintCallback onPaint;

  int _droppedFrames = 0;
  int _framesDrawn = 0;

  WeftFanoutPainter({
    required this.reader,
    required this.onPaint,
    super.repaint,
  });

  /// Frames completed by the writer that this reader never observed
  /// (advisory per AXIOM T; exact per the telescoping identity).
  int get droppedFrames => _droppedFrames;

  /// Fresh frames this painter has actually drawn.
  int get framesDrawn => _framesDrawn;

  @override
  void paint(Canvas canvas, Size size) {
    // 1. Claim freshest completed frame into the reader's buffer
    //    (zero Dart allocation).
    final rec = reader.claim();
    if (rec.ref.fresh != 0) {
      _droppedFrames += rec.ref.dropped;
      _framesDrawn++;
      // 2. Draw-phase read: the live view of the reader's copy buffer.
      onPaint(canvas, size, reader.view());
    }
  }

  @override
  bool shouldRepaint(covariant WeftFanoutPainter oldDelegate) {
    return true;
  }
}
