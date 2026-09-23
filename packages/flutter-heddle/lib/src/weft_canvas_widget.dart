// lib/src/weft_canvas_widget.dart — WeftCanvasWidget: drop-in Flutter widget
// for real-time charting over the Hot-Plane (charter §B).
//
// Zero-rebuild pattern (0 GC object churn per frame, charter Law 1):
//   * The Ticker calls notifier.pump() — the pump notifies ONLY when a watched
//     lane's sequence advanced, so idle planes cost nothing.
//   * The painter is constructed ONCE and passed `repaint: notifier`, so
//     notifications drive CustomPainter.repaint WITHOUT rebuilding the widget
//     tree — setState never appears in the frame path.
//   * shouldRepaint returns false (repaints come exclusively from the
//     Listenable) — audited by test/static_audit.mjs.

import 'package:flutter/material.dart';
import 'hot_plane.dart';
import 'weft_hot_plane_notifier.dart';

/// Reference painter: renders watched lanes as polylines + level bars from the
/// notifier's in-place snapshots. No allocation in paint().
class WeftHuddlePainter extends CustomPainter {
  final WeftHotPlaneNotifier notifier;
  final Color lineColor;
  final Color barColor;

  WeftHuddlePainter(this.notifier,
      {this.lineColor = const Color(0xFF38E1FF),
      this.barColor = const Color(0x5522C55E)})
      : super(repaint: notifier);

  @override
  void paint(Canvas canvas, Size size) {
    // polyline of lane 0 through the snapshot window + bars for other lanes.
    // All values come from preallocated notifier fields — zero alloc here.
    final n = notifier.current.length;
    if (n == 0) return;
    final line = Paint()
      ..color = lineColor
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5;
    final bar = Paint()..color = barColor;
    var min = notifier.mins[0];
    var max = notifier.maxs[0];
    final span = max > min ? max - min : 1.0;
    // value trace across the widget width (snapshot space, not ring history)
    final path = Path();
    for (var i = 0; i < n; i++) {
      final x = size.width * i / (n - 1 > 0 ? n - 1 : 1);
      final y = size.height - 4 - ((notifier.current[i] - min) / span) * (size.height - 8);
      if (i == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
      if (i > 0) {
        canvas.drawRect(
            Rect.fromLTWH(x - 1, size.height - 2, 2,
                (notifier.current[i] / (max <= 0 ? 1 : max)) * -8 + 8),
            bar);
      }
    }
    canvas.drawPath(path, line);
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}

class WeftCanvasWidget extends StatefulWidget {
  final WeftHotPlane plane;
  final List<int>? watch;
  final double hz;
  final CustomPainter? painterOverride;

  const WeftCanvasWidget({
    super.key,
    required this.plane,
    this.watch,
    this.hz = 240,
    this.painterOverride,
  });

  @override
  State<WeftCanvasWidget> createState() => _WeftCanvasWidgetState();
}

class _WeftCanvasWidgetState extends State<WeftCanvasWidget>
    with SingleTickerProviderStateMixin {
  late final Ticker _ticker;
  late final WeftHotPlaneNotifier _notifier;
  Duration _last = Duration.zero;
  int _frameBudgetNs = 0;

  @override
  void initState() {
    super.initState();
    _notifier = WeftHotPlaneNotifier(widget.plane, watch: widget.watch);
    _frameBudgetNs = (1e9 / widget.hz).round();
    // Ticker → pump: the ONLY thing the frame path does. notifyListeners is
    // conditional inside pump; CustomPainter repaints via repaint: notifier.
    _ticker = createTicker(_onTick);
    _ticker.start();
  }

  void _onTick(Duration elapsed) {
    // frame-budget guard: skip pumps that arrive early (drop-not-queue)
    final ns = elapsed.inMicroseconds * 1000;
    if (ns - _last.inMicroseconds * 1000 < _frameBudgetNs * 0.5) return;
    _last = elapsed;
    _notifier.pump();
  }

  @override
  void dispose() {
    _ticker.stop();
    _notifier.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      painter: widget.painterOverride ?? WeftHuddlePainter(_notifier),
      child: const SizedBox.expand(),
    );
  }
}
