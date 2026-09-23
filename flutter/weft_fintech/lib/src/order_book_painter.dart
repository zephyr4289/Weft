// order_book_painter.dart — zero-GC depth-ladder CustomPainter (Dart side).
//
// Draws the MDP1 snapshot as a 10-level bid/ask depth ladder:
//   - bid bars grow left from the mid gutter, ask bars grow right
//   - spread indicator between best bid / best ask rows
//   - GEOMETRY-ONLY by default: bar rects are computed into ONE
//     preallocated Float32List and painted with ONE reused Paint. The
//     label path (price/size text) is OPT-IN (`paintLabels: true`) and
//     documented as allocating — the same trade-off Pillar 4 recorded
//     for the heddle visualizers.
//
// shouldRepaint is ALWAYS false: repaints are driven exclusively by the
// edge-triggered WeftOrderBookNotifier through CustomPaint(repaint: ...),
// so a 240 Hz feed never invalidates through the widget tree.

library;

import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/widgets.dart';

import 'mdp1_wire.dart';
import 'order_book_widget.dart';

/// Box stride inside the scratch: [left, top, right, bottom, frac].
const int _BOX_STRIDE = 5;

class WeftOrderBookPainter extends CustomPainter {
  final WeftOrderBookController controller;
  final bool paintLabels;

  // Preallocated, reused for the whole widget lifetime (Law 2).
  final Float32List boxes =
      Float32List(MDP1_TOP_LEVELS * 2 * _BOX_STRIDE);
  final Paint barPaint = Paint()..style = PaintingStyle.fill;
  final Paint gutterPaint = Paint()..style = PaintingStyle.fill;
  late final TextPainter _tp = TextPainter()
    ..textDirection = TextDirection.ltr
    ..textWidthBasis = TextWidthBasis.longestLine;

  WeftOrderBookPainter(this.controller, {this.paintLabels = false});

  Mdp1Snapshot get snapshot => controller.snapshot;

  @override
  void paint(ui.Canvas canvas, ui.Size size) {
    // Validity is read AT PAINT TIME (the controller may flip FALLBACK
    // between builds; a stale bool would paint dead state).
    if (!controller.snapshotValid) {
      // FALLBACK band: a structurally invalid record paints one warning
      // bar and nothing else (fail-closed, never throws).
      gutterPaint.color = const ui.Color(0x66FF3B30);
      canvas.drawRect(
          ui.Offset.zero & ui.Size(size.width, 6), gutterPaint);
      return;
    }

    final w = size.width;
    final h = size.height;
    final gutter = w * 0.5;
    final rowH = h / (MDP1_TOP_LEVELS * 2);

    // Largest aggregated size across visible levels normalizes bar length.
    int maxSize = 1;
    for (int i = 0; i < MDP1_TOP_LEVELS; i++) {
      final b = snapshot.bidSize(i);
      final a = snapshot.askSize(i);
      if (b > maxSize) maxSize = b;
      if (a > maxSize) maxSize = a;
    }

    // Best (level 0) rows hug the gutter; deeper levels extend outward.
    for (int i = 0; i < MDP1_TOP_LEVELS; i++) {
      final top = h * 0.5 - (i + 1) * rowH;
      final sizeB = snapshot.bidSize(i);
      final fracB = sizeB / maxSize;
      final oB = i * _BOX_STRIDE;
      boxes[oB] = gutter - (gutter - 24) * fracB;
      boxes[oB + 1] = top;
      boxes[oB + 2] = gutter;
      boxes[oB + 3] = top + rowH - 1;
      boxes[oB + 4] = fracB;

      final topA = h * 0.5 + i * rowH;
      final sizeA = snapshot.askSize(i);
      final fracA = sizeA / maxSize;
      final oA = (MDP1_TOP_LEVELS + i) * _BOX_STRIDE;
      boxes[oA] = gutter;
      boxes[oA + 1] = topA;
      boxes[oA + 2] = gutter + (gutter - 24) * fracA;
      boxes[oA + 3] = topA + rowH - 1;
      boxes[oA + 4] = fracA;
    }

    // 20 bars, one paint call each — zero allocations in this loop.
    for (int i = 0; i < MDP1_TOP_LEVELS; i++) {
      final oB = i * _BOX_STRIDE;
      final frac = boxes[oB + 4];
      barPaint.color = ui.Color.lerp(
          const ui.Color(0x1400E5A0), const ui.Color(0xCC00E5A0), frac)!;
      canvas.drawRRect(
          ui.RRect.fromRectAndRadius(
              ui.Rect.fromLTRB(boxes[oB], boxes[oB + 1], boxes[oB + 2],
                  boxes[oB + 3]),
              const ui.Radius.circular(2)),
          barPaint);

      final oA = (MDP1_TOP_LEVELS + i) * _BOX_STRIDE;
      final fracA = boxes[oA + 4];
      barPaint.color = ui.Color.lerp(
          const ui.Color(0x14FF453A), const ui.Color(0xCCFF453A), fracA)!;
      canvas.drawRRect(
          ui.RRect.fromRectAndRadius(
              ui.Rect.fromLTRB(boxes[oA], boxes[oA + 1], boxes[oA + 2],
                  boxes[oA + 3]),
              const ui.Radius.circular(2)),
          barPaint);
    }

    // Spread indicator: locked/crossed book paints the gutter hot.
    gutterPaint.color = snapshot.locked
        ? const ui.Color(0xFFFFD60A)
        : snapshot.crossed
            ? const ui.Color(0xFFFF3B30)
            : const ui.Color(0x33FFFFFF);
    canvas.drawRect(ui.Rect.fromLTWH(gutter - 1, 0, 2, h), gutterPaint);

    // OPT-IN label path — allocates TextSpans per frame by design; keep
    // off in 240 FPS profiles (Pillar 4 label lesson).
    if (paintLabels) {
      _label(canvas, snapshot.bestBid.toString(),
          ui.Offset(8, h * 0.5 - rowH * 0.9));
      _label(canvas, snapshot.bestAsk.toString(),
          ui.Offset(w - 80, h * 0.5 + rowH * 0.1));
    }
  }

  void _label(ui.Canvas canvas, String text, ui.Offset at) {
    _tp.text = ui.TextSpan(
        text: text,
        style: const ui.TextStyle(fontSize: 11, color: ui.Color(0xD9FFFFFF)));
    _tp.layout(maxWidth: 120);
    _tp.paint(canvas, at);
  }

  @override
  bool shouldRepaint(covariant WeftOrderBookPainter oldDelegate) => false;
}
