// recycler.dart — RFC-0009 Series 7: 0-GC buffer recycler + memory-pressure
// backstop, Dart driver layer.
//
// WHY EXISTS: the Series-7 drawing loops (the governed consumers, the
// PACED_INTERPOLATE two-frame history, the raster blend) must not allocate
// per frame — Law 2. A PACED consumer retains a PREV-frame snapshot and a
// raster scratch buffer; allocating those per present would hand the GC a
// steady 60-120 Hz garbage stream (exactly the jank source mobile hardening
// exists to kill). This module pools those slots once and reuses them
// forever, with the memory-pressure backstop the work order demanded.
//
// HOT PATH (Law 2): acquire()/release() allocate NOTHING on the pooled
// path — a growable List of Uint8List references bounded by maxFreeSlots
// (removeLast/add on a list with retained capacity does not reallocate
// storage). Dart has no portable allocation counter (the per-port honesty
// wall — the JVM's allocated-bytes audit is the Kotlin leg's proof): the
// battery pins the identity discipline (identical() on a
// released-then-reacquired slot) and the deterministic counter equations.
//
// FLUTTER WIRING (the memory-pressure backstop, flutter side): register
// recyclers with WeftRecyclerCenter and install the observer from
// packages/flutter_weft (WeftMemoryPressureBackstop, a WidgetsBindingObserver
// that forwards didHaveMemoryPressure). The center is defined here, pure
// Dart, so ANY isolate can wire its own pressure signal.
//
// TRIM SEMANTICS (deterministic, documented, tested — identical to the
// Kotlin/Swift twins):
//   onLowMemory() / hard pressure -> drop ALL free slots (keepFree = 0)
//   onLowMemory(level)            -> keepFree by level class:
//     complete(80) / runningCritical(15)                  -> 0
//     moderate(60) / background(40) / runningLow(10)      -> half
//     uiHidden(20) / runningModerate(5) / unknown         -> all
//   LIVE slots are never touched — the backstop cannot drop a frame
//   because it cannot free the buffer a raster is being blended into; the
//   next acquire lazily reallocates (counted in reallocs; steady-state
//   first-fill allocations are not reallocs — pressure's cost is).
//
// SINGLE ISOLATE per recycler is the contract (the paint isolate); the
// center's listener list is plain (cold path — registration and pressure
// events, never per-frame).
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION (Dart VM battery
// green via the plain-dart expect shim; flutter-packages CI is the cover).

import 'dart:typed_data';

/// The app-facing memory-pressure hook (the work order's name).
/// Implement and register with [WeftRecyclerCenter]; forward the platform
/// signal (Flutter: WidgetsBindingObserver.didHaveMemoryPressure via
/// WeftMemoryPressureBackstop; Android host: ComponentCallbacks2;
/// iOS host: didReceiveMemoryWarning).
abstract interface class OnLowMemoryListener {
  /// Memory pressure event. [level] is the caller's severity class
  /// (Android ComponentCallbacks2-compatible values; 0 when unknown —
  /// treated as uiHidden-class, the conservative keep).
  void onLowMemory(int level);
}

/// Level classes (Android ComponentCallbacks2 values — mirrored so this
/// pure-Dart module needs no flutter import; values are API-stable).
abstract final class TrimLevel {
  static const int runningModerate = 5;
  static const int runningLow = 10;
  static const int runningCritical = 15;
  static const int uiHidden = 20;
  static const int background = 40;
  static const int moderate = 60;
  static const int complete = 80;
}

/// A 0-GC pool of same-sized byte slots (the Kotlin WeftBufferRecycler /
/// Swift WeftBufferRecycler twin; see those headers for the full contract).
class WeftBufferRecycler implements OnLowMemoryListener {
  /// Slot capacity in bytes (payload-sized for frame snapshots).
  final int slotBytes;
  /// Pool ceiling: at most this many released slots are kept free.
  final int maxFreeSlots;
  final List<Uint8List> _free = <Uint8List>[];

  // --- counters (advisory, AXIOM T; the battery pins the exact ones) ---
  /// Total acquires (pooled + allocated).
  int acquires = 0;
  /// Total releases (pooled + dropped-to-GC).
  int releases = 0;
  /// Allocations that happened because a trim removed slots.
  int reallocs = 0;
  /// trim()/onLowMemory() invocations.
  int trims = 0;
  /// Slots dropped by trims (free slots only — live slots never).
  int trimmedSlots = 0;

  /// Slots acquired and not yet released right now.
  int get liveNow => _live;
  /// Slots currently pooled free.
  int get pooledNow => _free.length;

  int _live = 0;
  bool _poolWasTrimmed = false;

  WeftBufferRecycler({required this.slotBytes, this.maxFreeSlots = 2}) {
    if (slotBytes <= 0) {
      throw ArgumentError('slotBytes must be > 0: $slotBytes');
    }
    if (maxFreeSlots < 0) {
      throw ArgumentError('maxFreeSlots must be >= 0: $maxFreeSlots');
    }
  }

  /// Take a slot: pooled (zero allocation) or freshly allocated (counted
  /// as a realloc iff a trim previously emptied the pool — steady-state
  /// first-fill allocations are not reallocs; pressure's cost is).
  Uint8List acquire() {
    acquires++;
    _live++;
    if (_free.isNotEmpty) {
      return _free.removeLast();
    }
    if (_poolWasTrimmed) reallocs++;
    return Uint8List(slotBytes);
  }

  /// Return a slot. True if pooled; false if the pool was full (the slot
  /// is dropped to the GC — bounded steady-state memory, Law 2's cold
  /// side).
  bool release(Uint8List slot) {
    releases++;
    _live--;
    if (_free.length < maxFreeSlots) {
      _free.add(slot);
      return true;
    }
    return false;
  }

  /// The memory-pressure backstop: drop FREE slots down to [keepFree]
  /// (default 0 — drop all). LIVE slots are never touched: the backstop
  /// cannot drop a frame because it cannot free the buffer a raster is
  /// being blended into; the next acquire lazily reallocates (counted).
  void trim([int keepFree = 0]) {
    trims++;
    var dropped = 0;
    while (_free.length > keepFree) {
      _free.removeLast();
      dropped++;
    }
    trimmedSlots += dropped;
    if (dropped > 0) _poolWasTrimmed = true;
  }

  /// [OnLowMemoryListener] forwarding with the documented level mapping.
  @override
  void onLowMemory(int level) {
    switch (level) {
      case TrimLevel.complete:
      case TrimLevel.runningCritical:
        trim(0);
      case TrimLevel.moderate:
      case TrimLevel.background:
      case TrimLevel.runningLow:
        trim(_free.length ~/ 2);
      default:
        trim(_free.length); // uiHidden / runningModerate / unknown: keep all
    }
  }
}

/// The process-wide fan-out point: one registration per app (Flutter:
/// WeftMemoryPressureBackstop -> didHaveMemoryPressure; any host: forward
/// the platform signal), N listeners (recyclers, governed consumers).
/// Cold path only — never called per frame.
class WeftRecyclerCenter {
  WeftRecyclerCenter._();

  static final WeftRecyclerCenter instance = WeftRecyclerCenter._();

  final List<OnLowMemoryListener> _listeners = <OnLowMemoryListener>[];

  /// The shared process center (per isolate — Dart isolates share nothing).
  static WeftRecyclerCenter get shared => instance;

  void register(OnLowMemoryListener listener) => _listeners.add(listener);
  void unregister(OnLowMemoryListener listener) => _listeners.remove(listener);
  int get registered => _listeners.length;

  /// Forward a classed pressure event (ComponentCallbacks2-compatible).
  void onLowMemory(int level) {
    for (final l in _listeners.toList()) {
      l.onLowMemory(level);
    }
  }

  /// Forward a hard pressure event (didHaveMemoryPressure /
  /// didReceiveMemoryWarning / onLowMemory()).
  void handleMemoryWarning() => onLowMemory(TrimLevel.complete);
}
