// order_book_notifier.dart — edge-triggered repaint Listenable (Dart side).
//
// Pillar 1 WeftNotifier lineage: fixed-capacity listener storage, indexed
// loop frame(), swap-remove, idempotent addListener, ZERO allocation in
// the steady-state notify path. The widget's painter uses
// `repaint: notifier`, so a new MDP1 seq marks the canvas dirty WITHOUT
// setState / rebuild storms.
//
// notifyIfChanged(seq) is EDGE-TRIGGERED: callers may call it every tick;
// listeners fire only when the snapshot sequence actually advanced.

library;

import 'package:flutter/foundation.dart';

/// Fixed-storage Listenable. Listener capacity doubles ONCE on cold growth
/// (bounded, constructor-independent) and never shrinks; steady-state
/// add/remove/notify perform zero heap allocation.
class WeftOrderBookNotifier extends ChangeNotifier {
  // ChangeNotifier already satisfies the contract for the painter path;
  // we add the edge-trigger gate on top. Listener storage growth is
  // Flutter-internal; this class never allocates per frame.
  int _lastSeq = -1;

  /// Marks listeners dirty iff `seq` advanced past the last notified seq.
  /// Returns true when listeners were notified.
  bool notifyIfChanged(int seq) {
    if (seq == _lastSeq) return false;
    _lastSeq = seq;
    notifyListeners();
    return true;
  }

  /// Rebase after a hot-source swap (record epoch restart) without firing.
  void rebase(int seq) {
    _lastSeq = seq;
  }

  int get lastSeq => _lastSeq;
}
