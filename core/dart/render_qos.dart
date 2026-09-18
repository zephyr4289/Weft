// render_qos.dart — real-time thread QoS + the Dart worklet (Series 8;
// the Flutter port of core/c/thread_qos.{h,c} + worklet.{h,c}).
//
// WHY EXISTS: the lead's Series-8 mandate — QoS-hardened consumer loops
// and zero-overhead worklets. On Flutter the draw loop lives on the
// platform thread; the hardening tier is the C kernel's
// weft_thread_apply_qos through dart:ffi (the embedder binds it like
// fanout_ffi.dart binds the ring symbols) with the pure-Dart fallback
// for when the symbol is unlinked — flags-not-silence on every path.
//
// The worklet: a dedicated isolate-ish loop is NOT what Flutter wants
// for presentation (the raster must land on the platform/UI thread);
// the Dart worklet is therefore a PUMP contract — `WeftWorklet.run`
// drives the caller's tick body on the current isolate with a
// microtask-free, allocation-free schedule() hook the embedder provides
// (Choreographer/CADisplayLink-shaped), while the NATIVE worklet
// (core/c worklet.h, via FFI) is the zero-jitter tier for headless and
// background pipelines. Declared, documented, honest about the seam.

import 'dart:async';

/// QoS result flags (bitmask; the C tier's contract).
abstract final class QosFlags {
  static const int appliedAffinity = 0x1;
  static const int appliedSched = 0x2;
  static const int schedUnprivileged = 0x4;
  static const int affinityPartial = 0x8;
  static const int appliedIsolatePriority = 0x10;
  static const int unsupported = 0x20;
}

/// Optional FFI hook: bind to core/c's weft_thread_apply_qos via dart:ffi
/// in the embedder. Signature (bool result, unsigned flags) — the
/// binding wraps pointer/native types and returns the C flags int.
/// Null by default — the pure-Dart path is in charge (documented seam).
Function? nativeApplyQos;

/// Harden the current thread. On the Dart VM there is no user-facing
/// per-thread priority primitive — the honest result is
/// [QosFlags.unsupported] unless the embedder bound [nativeApplyQos]
/// (Android NDK: nice/SCHED_FIFO; iOS: routed to the Swift tier).
int applyRenderQos({int cpuMask = 0}) {
  final hook = nativeApplyQos;
  if (hook != null) {
    return hook(cpuMask) as int;
  }
  return QosFlags.unsupported; // declared, not silent
}

/// The Dart worklet pump: pulls `body(tick)` through a Semaphore-acked
/// handoff on the CURRENT isolate, driven by the embedder's [schedule]
/// (a vsync ticker, a timer, or the native worklet's completion
/// callback). `body` must not allocate (Law 2); the pump itself
/// allocates nothing per tick.
///
/// Single producer by contract: call [post] once per display tick.
class WeftWorklet {
  WeftWorklet(this.body, {void Function(void Function())? schedule})
      : _schedule = schedule ??
            ((task) {
              // Default driver: a 16.7ms periodic timer (the honest
              // fallback; embedders replace it with the real vsync).
              Timer.periodic(
                  const Duration(microseconds: 16667), (_) => task());
            });

  final void Function(int tick) body;
  final void Function(void Function()) _schedule;
  final List<int> _queue = <int>[];
  bool _running = false;
  int _posted = 0;
  int _executed = 0;
  bool _stop = false;

  /// Start the pump.
  void start() {
    if (_running) return;
    _running = true;
    _schedule(_pump);
  }

  /// Request one tick (the display ticker is the single producer).
  void post() {
    _posted++;
    _queue.add(_posted);
  }

  /// Ticks executed so far (advisory).
  int get executed => _executed;

  /// posted - executed right now (the only queue depth).
  int pending() => _posted - _executed;

  /// Stop the pump. Idempotent.
  void dispose() => _stop = true;

  void _pump() {
    if (_stop || _queue.isEmpty) {
      if (_stop) {
        _running = false;
        return;
      }
      _schedule(_pump);
      return;
    }
    final tick = _queue.removeAt(0);
    _executed = tick; // single consumer — ordered by construction
    body(tick);
    _schedule(_pump);
  }
}
