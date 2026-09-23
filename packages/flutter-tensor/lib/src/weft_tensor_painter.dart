// weft_tensor_painter.dart — zero-GC AI overlay painting (boxes / skeleton).
//
// Pillar 2 Deliverable D. Pattern: CustomPainter with a DRAW-PHASE deferred
// read — `paint()` acquires the latest WTR1 frame itself (the repaint signal
// is the zero-alloc [WeftTensorNotifier], NOT setState). Boxes live in a
// CALLER-OWNED [OverlayScratch] (one preallocated Float32List, stride 6)
// that the detector writes and the painter reads — no {x,y,w,h} object churn,
// no arrays-of-objects, no GC (Law 1), mirroring TS render/overlay.js.
//
// Allocation honesty (Law 1 scope): buffers, views, scratch, listeners and
// paints are all reused. The ONLY unavoidable per-box churn is Flutter's
// Rect/Offset VALUE records passed to Canvas (young-gen, cheap, no view/list
// churn) — Flutter's Canvas API has no in-place rect primitive. The optional
// label path (TextPainter) DOES allocate (string + text layout) and is
// therefore opt-in, documented, and off the default path.
//
// Law 2 (strict LE): the painter never reads multi-byte scalars itself; all
// wire access happened in ring_header.dart / weft_ring.dart with explicit
// Endian.little. Law 4 (boundary validation): paint() only ever sees views
// that passed the full seqlock validation in [WeftRing.acquireLatest].
library weft_flutter_tensor.src.weft_tensor_painter;

import 'dart:typed_data';

import 'package:flutter/widgets.dart';

import 'weft_ring.dart';
import 'weft_tensor_notifier.dart';

/// Default box / skeleton stroke color (cyan — TS parity '#00e5ff').
const Color weftOverlayColor = Color(0xFF00E5FF);

/// Max boxes by default (parity with TS render/overlay.js MAX_BOXES).
const int weftMaxBoxes = 256;

/// Stride of one box record in [OverlayScratch.boxes]: x, y, w, h, score, classId.
const int weftBoxStride = 6;

/// Keypoint skeleton edges in COCO-17 order, pairs packed into ONE flat
/// const list — value-identical to TS render/overlay.js COCO17_EDGES.
const List<int> coco17Edges = <int>[
  0, 1, 0, 2, 1, 3, 2, 4, // face
  5, 6, 5, 7, 7, 9, 6, 8, 8, 10, // arms
  5, 11, 6, 12, 11, 12, // torso
  11, 13, 13, 15, 12, 14, 14, 16, // legs
];

/// Detection scratch — the detector's output buffer AND the painter's input.
///
/// Reused every frame; [count] says how many records are live. Layout is one
/// flat Float32List (stride [weftBoxStride]): the detector appends with
/// [pushBox], the painter indexes [boxes] directly (zero allocation, Law 1).
class OverlayScratch {
  OverlayScratch({this.maxBoxes = weftMaxBoxes})
      : boxes = Float32List(maxBoxes * weftBoxStride);

  final int maxBoxes;

  /// Flat box storage: record i lives at [i * weftBoxStride, +6).
  final Float32List boxes;

  /// Live record count.
  int count = 0;

  /// Reset for a new frame's detections (O(1), zero allocation).
  void clear() => count = 0;

  /// Append one box. Returns false when full (never throws in the hot loop).
  bool pushBox(double x, double y, double w, double h, double score, double classId) {
    if (count >= maxBoxes) return false;
    final o = count * weftBoxStride;
    final b = boxes;
    b[o] = x;
    b[o + 1] = y;
    b[o + 2] = w;
    b[o + 3] = h;
    b[o + 4] = score;
    b[o + 5] = classId;
    count++;
    return true;
  }

  /// Copy record [i] into a CALLER-OWNED 6-slot buffer (readable API without
  /// allocation); hot painters index [boxes] directly instead.
  void boxAt(int i, Float32List out6) {
    final o = i * weftBoxStride;
    final b = boxes;
    out6[0] = b[o];
    out6[1] = b[o + 1];
    out6[2] = b[o + 2];
    out6[3] = b[o + 3];
    out6[4] = b[o + 4];
    out6[5] = b[o + 5];
  }
}

/// Draw all boxes in [scratch] with the CALLER-OWNED, REUSED [paint].
/// Zero allocation beyond Flutter's Rect value records (see library docs).
/// Returns the number of boxes drawn.
int drawBoxes(Canvas canvas, OverlayScratch scratch, Paint paint) {
  final b = scratch.boxes;
  final n = scratch.count;
  for (var i = 0; i < n; i++) {
    final o = i * weftBoxStride;
    final x = b[o];
    final y = b[o + 1];
    canvas.drawRect(Rect.fromLTRB(x, y, x + b[o + 2], y + b[o + 3]), paint);
  }
  return n;
}

/// Draw a COCO-17 keypoint skeleton from a preallocated [Float32List] of
/// x,y,score triplets (caller-owned). Zero allocation. Parity with TS
/// drawSkeleton2D: edges below [confidence] are skipped.
int drawSkeleton(
  Canvas canvas,
  Float32List keypoints,
  int count,
  Paint paint, {
  double confidence = 0.25,
}) {
  var drawn = 0;
  for (var e = 0; e + 1 < coco17Edges.length; e += 2) {
    final a = coco17Edges[e];
    final b = coco17Edges[e + 1];
    if (a >= count || b >= count) continue;
    final ao = a * 3;
    final bo = b * 3;
    if (keypoints[ao + 2] < confidence || keypoints[bo + 2] < confidence) {
      continue;
    }
    canvas.drawLine(
      Offset(keypoints[ao], keypoints[ao + 1]),
      Offset(keypoints[bo], keypoints[bo + 1]),
      paint,
    );
    drawn++;
  }
  return drawn;
}

/// CustomPainter that renders the AI overlay over a WTR1 ring.
///
/// ```dart
/// final notifier = WeftTensorNotifier();
/// final scratch = OverlayScratch();
/// CustomPaint(
///   repaint: notifier, // zero-alloc repaint signal
///   painter: WeftTensorPainter(ring, notifier, scratch: scratch),
/// );
/// ```
///
/// `shouldRepaint` is ALWAYS false — repaints are driven exclusively by the
/// notifier ([WeftVideoOverlay] calls `notifier.frame()` per acquired frame).
class WeftTensorPainter extends CustomPainter {
  WeftTensorPainter(
    this.ring,
    this.notifier, {
    Paint? boxPaint,
    Paint? skeletonPaint,
    this.textPainter,
    this.scratch,
    this.keypoints,
    this.keypointCount = 0,
    Listenable? repaint,
  })  : boxPaint = boxPaint ?? _defaultStroke(),
        skeletonPaint = skeletonPaint ?? _defaultStroke(),
        super(repaint: repaint ?? notifier);

  final WeftRing ring;

  /// The zero-alloc repaint source. Kept on the painter so the overlay and
  /// painter provably share ONE signal identity (also the default repaint).
  final WeftTensorNotifier notifier;

  /// Reused box stroke paint (caller-owned or the shared default).
  final Paint boxPaint;

  /// Reused skeleton stroke paint.
  final Paint skeletonPaint;

  /// Opt-in label painter. WHEN PROVIDED, labels allocate (TextSpan + text
  /// layout) — keep null for a strictly zero-GC loop (see library docs).
  final TextPainter? textPainter;

  /// Caller-owned detection scratch (detector writes, painter reads).
  final OverlayScratch? scratch;

  /// Caller-owned keypoint storage (x,y,score triplets) — optional skeleton.
  final Float32List? keypoints;

  /// Live keypoint count (mutated in place by the detector between frames).
  int keypointCount;

  static Paint _defaultStroke() {
    final p = Paint();
    p.color = weftOverlayColor;
    p.style = PaintingStyle.stroke;
    p.strokeWidth = 2;
    return p;
  }

  /// Frames acquired at paint time (diagnostics).
  int framesAcquired = 0;
  int framesDropped = 0;

  @override
  void paint(Canvas canvas, Size size) {
    // Draw-phase deferred read: the authoritative acquire happens HERE (a
    // tick may have been coalesced away since the notifier fired). Zero
    // allocation — the ring rebinds its flyweight in place (Law 1).
    final view = ring.acquireLatest();
    if (view == null) {
      framesDropped++;
      return; // nothing new / torn window — previous pixels stay on screen
    }
    framesAcquired++;
    final s = scratch;
    if (s != null && s.count > 0) {
      drawBoxes(canvas, s, boxPaint);
      final tp = textPainter;
      if (tp != null) _drawLabels(canvas, s, tp);
    }
    final kp = keypoints;
    if (kp != null && keypointCount > 0) {
      drawSkeleton(canvas, kp, keypointCount, skeletonPaint);
    }
  }

  /// Label path — DOCUMENTED ALLOCATION SITE (string interpolation +
  /// TextSpan + text layout). Opt-in via [textPainter]; off by default.
  void _drawLabels(Canvas canvas, OverlayScratch scratch, TextPainter tp) {
    final b = scratch.boxes;
    for (var i = 0; i < scratch.count; i++) {
      final o = i * weftBoxStride;
      final cls = b[o + 5].toInt();
      final scorePct = (b[o + 4] * 100).toInt();
      tp.text = TextSpan(text: '$cls $scorePct', style: tp.text?.style);
      tp.layout();
      tp.paint(canvas, Offset(b[o] + 2, b[o + 1] + 2));
    }
  }

  @override
  bool shouldRepaint(covariant WeftTensorPainter oldDelegate) => false;
}
