// weft_video_overlay.dart — ticker-driven zero-GC tensor overlay widget.
//
// Pillar 2 Deliverable D. ONE Ticker (display refresh rate, auto-muted by
// TickerMode via SingleTickerProviderStateMixin) drives the acquire loop:
//
//   tick -> ring.acquireLatest() -> new seq? -> onFrame(view) (detector
//          fills the reused OverlayScratch) -> notifier.frame() (repaint)
//
// NO setState in the frame path: the widget tree is NEVER rebuilt for
// frames — CustomPaint(repaint: notifier) re-paints only. setState is
// reserved for lifecycle-level configuration (e.g. the 1 Hz stats line in
// the example), which is exactly the Pillar 1 weft_notifier.dart contract.
//
// Law 1 (zero steady-state allocation): the tick path allocates nothing —
// ring acquire rebinds the flyweight, the notifier is a fixed-capacity
// indexed loop, the scratch is caller-owned. Law 4 (boundary validation):
// only ring-validated views ever reach [onFrame] and the painter.
library weft_flutter_tensor.src.weft_video_overlay;

import 'package:flutter/scheduler.dart'
    show SingleTickerProviderStateMixin, Ticker;
import 'package:flutter/widgets.dart';

import 'weft_ring.dart';
import 'weft_tensor_notifier.dart';
import 'weft_tensor_painter.dart';
import 'weft_tensor_view.dart';

/// Real-time AI overlay over a [WeftRing], repaint-driven, zero-GC.
///
/// ```dart
/// WeftVideoOverlay(
///   ring: ring,
///   scratch: scratch,
///   onFrame: (view) => detector.fillScratch(view, scratch),
/// )
/// ```
///
/// The notifier is created internally unless injected via [notifier]; when
/// internal it is disposed with the state, when injected its lifecycle
/// belongs to the caller.
class WeftVideoOverlay extends StatefulWidget {
  const WeftVideoOverlay({
    super.key,
    required this.ring,
    this.notifier,
    this.scratch,
    this.onFrame,
    this.textPainter,
    this.child,
  });

  final WeftRing ring;

  /// Optional externally-owned notifier (share one signal across overlays).
  final WeftTensorNotifier? notifier;

  /// Caller-owned detection scratch (detector writes, painter reads).
  final OverlayScratch? scratch;

  /// Frame hook — invoked ONLY with a fully-validated view, ONLY when the
  /// sequence advanced. Zero-alloc contract: implementations must not
  /// allocate (fill preallocated scratch in place).
  final void Function(WeftTensorView view)? onFrame;

  /// Opt-in label painter (documented allocation path — see painter docs).
  final TextPainter? textPainter;

  /// Optional child rendered under the overlay.
  final Widget? child;

  @override
  State<WeftVideoOverlay> createState() => _WeftVideoOverlayState();
}

class _WeftVideoOverlayState extends State<WeftVideoOverlay>
    with SingleTickerProviderStateMixin {
  Ticker? _ticker;
  WeftTensorNotifier? _ownedNotifier;
  int _lastSeq = 0;
  int _frames = 0;
  int _ticks = 0;

  WeftTensorNotifier get _notifier => widget.notifier ?? _ownedNotifier!;

  @override
  void initState() {
    super.initState();
    if (widget.notifier == null) {
      _ownedNotifier = WeftTensorNotifier();
    }
    // ONE ticker at display refresh (muted automatically under TickerMode).
    _ticker = createTicker(_onTick)..start();
  }

  @override
  void didUpdateWidget(covariant WeftVideoOverlay oldWidget) {
    super.didUpdateWidget(oldWidget);
    // Lifecycle-level config change only (NOT the frame path): rebase the
    // watermark when the ring identity swaps. No setState needed — the
    // ticker repaints on the next acquired frame anyway.
    if (!identical(oldWidget.ring, widget.ring)) {
      _lastSeq = 0;
    }
  }

  /// Hot path — runs at display refresh. ZERO allocation (Law 1):
  /// acquireLatest rebinds the ring's flyweight; `frame()` is an indexed
  /// loop; the scratch is mutated in place. NO setState here (Law 1 /
  /// weft_notifier pattern): repaint happens via the notifier, not rebuilds.
  void _onTick(Duration elapsed) {
    _ticks++;
    final view = widget.ring.acquireLatest();
    if (view == null) return; // nothing committed / torn window: keep pixels
    if (view.seq == _lastSeq) return; // nothing new — no notify, no paint
    _lastSeq = view.seq;
    _frames++;
    final cb = widget.onFrame;
    if (cb != null) cb(view); // detector fills reused scratch (zero-alloc)
    _notifier.frame(); // hot notify path -> CustomPaint(repaint: notifier)
  }

  @override
  Widget build(BuildContext context) {
    // Build runs on lifecycle/config changes only — never per frame.
    return CustomPaint(
      size: Size.infinite,
      repaint: _notifier,
      painter: WeftTensorPainter(
        widget.ring,
        _notifier,
        scratch: widget.scratch,
        textPainter: widget.textPainter,
        repaint: _notifier, // same signal — addListener is idempotent
      ),
      child: widget.child,
    );
  }

  @override
  void dispose() {
    _ticker?.stop();
    _ticker?.dispose();
    _ticker = null;
    _ownedNotifier?.dispose();
    _ownedNotifier = null;
    super.dispose();
  }

  /// Diagnostics: ticks seen / frames notified (monotonic counters).
  (int, int) get tickStats => (_ticks, _frames);
}
