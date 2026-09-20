// weft_notifier.dart — zero-GC reactive plumbing for Weft frames (Pillar 1 §2.E).
//
// Flutter's ChangeNotifier allocates (growable listener list, iteration
// guards) on every notify. At 120-240 FPS that is exactly the GC pressure
// this pillar exists to eliminate. WeftNotifier is a Listenable with
// FIXED-CAPACITY listener storage: `frame()` (the hot notify path) performs
// a plain indexed loop over an array — zero allocation, zero closure churn.
//
// Wire it to weftc-generated Dart views (tools/weftc/codegen/dart):
//
//   final notifier = WeftNotifier();
//   final pump = WeftFramePump<TelemetryFrame>(
//     notifier: notifier,
//     view: TelemetryFrame(),
//     bind: (v, bd, offset) => v.bind(bd, offset),
//     validate: (v, bd, offset, avail) => v.validateHeader(avail),
//   );
//
//   // ring producer thread / isolate / FFI callback:
//   pump.handleWindow(ringWindow, byteOffset, avail);
//
//   // UI: repaint WITHOUT rebuilding the widget tree
//   return CustomPaint(
//     repaint: notifier,           // frame() == repaint signal
//     painter: MyScopePainter(view), // reads view fields, zero alloc
//   );
//
// Law 1 (zero steady-state allocation): notify path is allocation-free;
// addListener grows storage by doubling (cold path); WeftFramePump reuses
// one view instance and caches the ByteData window per source buffer.
library;

import 'dart:typed_data';

import 'package:flutter/foundation.dart' show Listenable, VoidCallback, visibleForTesting;

/// A [Listenable] with fixed-capacity listener storage.
///
/// * `frame()` — hot path: notify all listeners, zero allocation.
/// * `addListener` — cold path: O(1) amortized (doubling growth).
/// * `removeListener` — cold path: O(n) scan + swap-remove.
///
/// Mutating listeners during `frame()` follows Flutter's ChangeNotifier
/// contract: removal during notification is honored for listeners not yet
/// visited in this pass.
class WeftNotifier implements Listenable {
  /// Creates a notifier with an initial listener capacity (grown by
  /// doubling on demand — the growth itself is a cold-path allocation).
  WeftNotifier({int capacity = 8})
      : assert(capacity > 0),
        _listeners = List<VoidCallback?>.filled(capacity, null, growable: false);

  List<VoidCallback?> _listeners;
  int _count = 0;
  int _frames = 0;

  /// Number of registered listeners.
  int get listenerCount => _count;

  /// Total frames notified (monotonic; diagnostics only — reading it from
  /// the UI is fine, incrementing it costs one integer add per frame).
  int get frameCount => _frames;

  /// Hot path: notify every listener (e.g. repaint a CustomPaint).
  /// Zero allocation: fixed array, indexed loop, no iterators.
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
      final grown = List<VoidCallback?>.filled(_listeners.length * 2, null, growable: false);
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

/// Caches one [ByteData] window per source buffer object.
///
/// Ring implementations typically hand back the SAME underlying buffer with
/// shifting offsets; [forWindow] re-derives the wrapper only when the source
/// object changes (identity check), keeping the steady state allocation-free.
class WeftWindowCache {
  Uint8List? _source;
  ByteData? _window;

  /// Returns a ByteData view over [window] — no bytes are copied.
  ByteData forWindow(Uint8List window) {
    if (identical(_source, window)) return _window!;
    _source = window;
    _window = ByteData.view(window.buffer, window.offsetInBytes, window.lengthInBytes);
    return _window!;
  }
}

/// Binds a frame window onto a weftc-generated view and notifies.
///
/// [bind] and [validate] are user-supplied adapters over the generated view
/// (mechanism, not policy: the pump never imports codegen output). The pump
/// owns exactly one view instance and reuses it for every frame — Law 1.
typedef WeftViewBinder<V> = V Function(V view, ByteData bd, int byteOffset);
typedef WeftViewValidator<V> = bool Function(V view, ByteData bd, int byteOffset, int avail);

class WeftFramePump<V> {
  WeftFramePump({
    required this.notifier,
    required this.view,
    required this.bind,
    this.validate,
    WeftWindowCache? windowCache,
  }) : _windows = windowCache ?? WeftWindowCache();

  final WeftNotifier notifier;
  final V view;
  final WeftViewBinder<V> bind;
  final WeftViewValidator<V>? validate;
  final WeftWindowCache _windows;

  int accepted = 0;
  int rejected = 0;

  /// Steady-state entry point. [window] is the ring window holding one
  /// frame at [byteOffset]; [avail] is the readable span from [byteOffset].
  void handleWindow(Uint8List window, int byteOffset, int avail) {
    final bd = _windows.forWindow(window);
    final validator = validate;
    if (validator != null && !validator(view, bd, byteOffset, avail)) {
      rejected++;
      return;
    }
    bind(view, bd, byteOffset);
    accepted++;
    notifier.frame();
  }
}

@visibleForTesting
const int weftNotifierApiVersion = 1;
