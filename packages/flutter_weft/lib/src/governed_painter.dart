// governed_painter.dart — Series 7: the Flutter CustomPainter adapter for
// GovernedFanoutConsumer (RFC-0009's composed display consumer).
//
// WHY EXISTS: the work order's "render-thread deep integration" — the
// Flutter paint phase is where the governed consumer belongs (the
// fanout_painter discipline, WHITEPAPER §8.4): ONE tick per paint pass
// (claim -> ladder -> policy -> raster, all zero-allocation — the
// consumer's contract), and the paint callback runs ONLY on present ticks
// (the policy's elided ticks draw nothing — the previous raster stays;
// that IS the steady-cadence contract, LATEST_WINS/PACED/BURST decided).
//
// IMPPELLER-SAFE: the raster is packed u32 words, little-endian (the wire
// contract) in a POOLED Uint8List whose identity is stable for the
// consumer's lifetime — the painter never allocates per frame, never
// saveLayers behind your back, and the bytes are ready for a fragment
// shader uniform or ui.Image raw upload without a copy.
//
// THE LADDER STAYS ADVISORY: [onPaint] receives the action record too —
// the app responds to CLASS edges (Skip: skip decorative work; Snapshot:
// re-sync; Reseed: rebuild) without the painter ever branching on
// telemetry (AXIOM T). consumer.actionChanged carries the edge.
//
// shouldRepaint is always true — Flutter drives paint ticks and the
// POLICY elides inside paint (the same stance as WeftFanoutPainter; the
// decision is made where the data lives, not where the framework guesses).

import 'dart:typed_data';

import 'package:flutter/widgets.dart';

import 'reference/governed_consumer.dart';
import 'reference/governor.dart';

/// Draw callback: the raster (packed u32 LE words, stable identity — do
/// not retain), the presentation decision, and the advisory ladder action.
typedef WeftGovernedPaintCallback = void Function(
    Canvas canvas, Size size, Uint8List raster, PresentDecision decision, GovernorAction action);

/// CustomPainter driving a [GovernedFanoutConsumer] strictly inside
/// [paint] — the composed display consumer, once per paint pass.
class WeftGovernedPainter extends CustomPainter {
  final GovernedFanoutConsumer consumer;
  final WeftGovernedPaintCallback onPaint;

  WeftGovernedPainter({
    required this.consumer,
    required this.onPaint,
    super.repaint,
  });

  @override
  void paint(Canvas canvas, Size size) {
    final d = consumer.tick(); // claim + ladder + policy + raster — zero alloc
    if (d.present) {
      onPaint(canvas, size, consumer.raster, d, consumer.action);
    }
  }

  @override
  bool shouldRepaint(covariant WeftGovernedPainter oldDelegate) => true;
}
