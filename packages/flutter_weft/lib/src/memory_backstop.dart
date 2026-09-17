// memory_backstop.dart — Flutter memory-pressure wiring (Series 7).
//
// WHY EXISTS: the work order's backstop contract — "memory-pressure
// backstops that safely re-allocate without frame drops" — needs a
// platform signal source on Flutter. The engine delivers exactly one:
// WidgetsBindingObserver.didHaveMemoryPressure (fired for both Android
// onTrimMemory-complete/onLowMemory and iOS didReceiveMemoryWarning).
// This file is the two-line bridge the app installs ONCE:
//
//   final backstop = WeftMemoryPressureBackstop()..install();
//   // recyclers/consumers register themselves with WeftRecyclerCenter
//
// Every registered recycler then drops its FREE slots (LIVE slots are
// never touched — a raster mid-blend cannot lose its buffer; the next
// acquire lazily re-allocates and counts it). Zero frame drops by
// construction; the cost is visible in reallocs (AXIOM T).
//
// WHY NOT ComponentCallbacks2 levels on Android: the Flutter engine does
// not forward trim levels — didHaveMemoryPressure is the whole signal
// (it maps to the COMPLETE class: drop all free slots). The classed
// WeftRecyclerCenter.onLowMemory(level) API remains available for hosts
// that wire their own MethodChannel; declared, per the per-port honesty
// culture.
//
// LAW 2: the observer is cold-path only (pressure events, registration);
// nothing here runs per frame.

import 'package:flutter/widgets.dart';

import 'reference/recycler.dart';

/// A [WidgetsBindingObserver] that forwards engine memory-pressure events
/// to [WeftRecyclerCenter]. Install exactly one per app (or per isolate
/// that owns recyclers); [dispose] removes it.
class WeftMemoryPressureBackstop with WidgetsBindingObserver {
  WeftMemoryPressureBackstop({this.onPressure});

  /// Optional app hook fired AFTER the center fan-out (logging, analytics).
  final void Function()? onPressure;

  bool _installed = false;

  /// Register with the WidgetsBinding. Idempotent.
  void install() {
    if (_installed) return;
    WidgetsBinding.instance.addObserver(this);
    _installed = true;
  }

  /// Remove from the WidgetsBinding. Safe to call when not installed.
  void dispose() {
    if (!_installed) return;
    WidgetsBinding.instance.removeObserver(this);
    _installed = false;
  }

  @override
  void didHaveMemoryPressure() {
    // COMPLETE class: every registered recycler drops all FREE slots.
    WeftRecyclerCenter.shared.handleMemoryWarning();
    onPressure?.call();
  }
}
