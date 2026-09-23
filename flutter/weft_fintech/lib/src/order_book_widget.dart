// order_book_widget.dart — ticker-driven zero-rebuild order book (Dart side).
//
// ONE Ticker at display refresh. Each tick:
//   producer.offer(bytes) -> controller edge-checks seq -> notifier fires
//   -> CustomPaint repaints via `repaint:` (NO setState, NO rebuild).
//
// The controller delivers MDP1 records by REFERENCE (Uint8List produced
// upstream — e.g. attached from the native ring, or the demo synthesizer);
// the widget never copies and never parses into widget state.
//
// didUpdateWidget rebases the watermark on controller swap; dispose stops
// and disposes the ticker. The 1 Hz stats path is edge-triggered through
// the same notifier (documented cold path if a Text listenable is used).

library;

import 'dart:typed_data';

import 'package:flutter/scheduler.dart';
import 'package:flutter/widgets.dart';

import 'mdp1_wire.dart';
import 'order_book_notifier.dart';
import 'order_book_painter.dart';

/// Producer seam: hand the widget the latest 304-byte MDP1 record.
/// The bytes may be REUSED by the producer (ring slot / pooled buffer) —
/// the controller forwards the reference within the same tick only.
class WeftOrderBookController {
  final WeftOrderBookNotifier notifier = WeftOrderBookNotifier();
  final Mdp1Snapshot snapshot = Mdp1Snapshot(Uint8List(MDP1_SIZE));

  int _lastSeenSeq = -1;
  Uint8List? _pending;
  int _pendingSeq = -1;
  bool _pendingValid = false;

  /// Called by the producer (hot path — one reference store).
  void offer(Uint8List mdp1Record) {
    _pending = mdp1Record;
  }

  /// Called by the ticker: consumes the pending record, validates it,
  /// edge-triggers the notifier. Returns true when a new frame landed.
  bool consume() {
    final p = _pending;
    if (p == null) return false;
    _pending = null;
    snapshot.retarget(p);
    final code = snapshot.validate();
    final wasValid = _pendingValid;
    _pendingValid = code == MDP1_OK;
    if (!_pendingValid) {
      // Edge into FALLBACK: repaint ONCE (warning band), then latch so a
      // poison record cannot hot-loop the compositor.
      if (wasValid) {
        _pendingSeq = _SEQ_INVALID;
        notifier.rebase(_SEQ_INVALID);
        notifier.notifyListeners();
      }
      return wasValid;
    }
    _pendingSeq = snapshot.seq;
    return notifier.notifyIfChanged(_pendingSeq);
  }

  bool get snapshotValid => _pendingValid;

  /// Latest consumed seq (for tests / HUD).
  int get seq => _pendingSeq;

  static const int _SEQ_INVALID = -1 << 30;
}

class WeftOrderBookWidget extends StatefulWidget {
  final WeftOrderBookController controller;
  final bool paintLabels;
  const WeftOrderBookWidget(
      {super.key, required this.controller, this.paintLabels = false});

  @override
  State<WeftOrderBookWidget> createState() => _WeftOrderBookWidgetState();
}

class _WeftOrderBookWidgetState extends State<WeftOrderBookWidget>
    with SingleTickerProviderStateMixin {
  late final Ticker _ticker;
  WeftOrderBookController? _bound;

  @override
  void initState() {
    super.initState();
    _bind(widget.controller);
    _ticker = createTicker((_) {
      widget.controller.consume();
    });
    _ticker.muted = false;
    _ticker.start();
  }

  void _bind(WeftOrderBookController c) {
    _bound = c;
    c.notifier.rebase(-1);
  }

  @override
  void didUpdateWidget(covariant WeftOrderBookWidget old) {
    super.didUpdateWidget(old);
    if (!identical(old.controller, widget.controller)) {
      _bind(widget.controller);
    }
  }

  @override
  void dispose() {
    _ticker.stop();
    _ticker.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // ONE build for the widget's lifetime: repaint is notifier-driven.
    return CustomPaint(
      repaint: widget.controller.notifier,
      painter: WeftOrderBookPainter(
        widget.controller,
        paintLabels: widget.paintLabels,
      ),
      child: const SizedBox.expand(),
    );
  }
}
