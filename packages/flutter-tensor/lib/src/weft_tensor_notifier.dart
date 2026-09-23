// weft_tensor_notifier.dart — zero-GC repaint signalling for tensor frames.
//
// Adapted from Pillar 1's packages/flutter_weft/lib/src/weft_notifier.dart
// (WeftNotifier) with the SAME mechanical zero-alloc discipline:
//
// Flutter's ChangeNotifier allocates on every notify (growable listener list,
// iteration guards, set-iteration checks). At 120-240 FPS that is exactly the
// GC pressure Pillar 2 exists to eliminate. WeftTensorNotifier is a Listenable
// with FIXED-CAPACITY listener storage: `frame()` (the hot notify path) is a
// plain indexed loop over one array — zero allocation, zero closure churn,
// no iterator objects.
//
// Law 1 (zero steady-state allocation): notify path is allocation-free;
// addListener grows storage by doubling (cold path only); removeListener is
// an O(n) scan + swap-remove (no list compaction).
library weft_flutter_tensor.src.weft_tensor_notifier;

import 'package:flutter/foundation.dart' show Listenable, VoidCallback;

/// A [Listenable] with fixed-capacity listener storage (Pillar 1 pattern).
///
/// * `frame()` — hot path: notify all listeners, zero allocation.
/// * `addListener` — cold path: O(1) amortized (doubling growth), idempotent
///   by identity (so wiring the same notifier to both `CustomPaint.repaint`
///   and the painter costs nothing extra).
/// * `removeListener` — cold path: O(n) scan + swap-remove.
///
/// Mutating listeners during `frame()` follows Flutter's ChangeNotifier
/// contract: removal during notification is honored for listeners not yet
/// visited in this pass.
class WeftTensorNotifier implements Listenable {
  /// Creates a notifier with an initial listener capacity (grown by
  /// doubling on demand — the growth itself is a cold-path allocation).
  WeftTensorNotifier({int capacity = 8})
      : assert(capacity > 0),
        _listeners = List<VoidCallback?>.filled(capacity, null, growable: false);

  List<VoidCallback?> _listeners;
  int _count = 0;
  int _frames = 0;

  /// Number of registered listeners.
  int get listenerCount => _count;

  /// Total frames notified (monotonic; diagnostics only).
  int get frameCount => _frames;

  /// Hot path: notify every listener (repaint the tensor overlay).
  /// Zero allocation: fixed array, indexed loop, no iterators, no closures.
  void frame() {
    _frames++;
    final l = _listeners;
    final n = _count;
    for (var i = 0; i < n; i++) {
      final cb = l[i];
      if (cb != null) cb();
    }
  }

  @override
  void addListener(VoidCallback listener) {
    for (var i = 0; i < _count; i++) {
      if (identical(_listeners[i], listener)) return; // idempotent
    }
    if (_count == _listeners.length) {
      // COLD PATH: grow by doubling.
      final grown =
          List<VoidCallback?>.filled(_listeners.length * 2, null, growable: false);
      for (var i = 0; i < _count; i++) {
        grown[i] = _listeners[i];
      }
      _listeners = grown;
    }
    _listeners[_count++] = listener;
  }

  @override
  void removeListener(VoidCallback listener) {
    for (var i = 0; i < _count; i++) {
      if (identical(_listeners[i], listener)) {
        _count--;
        _listeners[i] = _listeners[_count]; // swap-remove
        _listeners[_count] = null;
        return;
      }
    }
  }

  @override
  void dispose() {
    for (var i = 0; i < _listeners.length; i++) {
      _listeners[i] = null;
    }
    _count = 0;
  }
}
