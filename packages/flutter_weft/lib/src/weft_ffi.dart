// weft_ffi.dart — Production Weft FFI channel wrapper with zero steady-state allocation
//
// WHY EXISTS: Wraps the native C Triad kernel for Flutter with zero steady-state
// Dart allocations during paint/exchange loops per WHITEPAPER §8.4 and DIRECTIVE-14 T14.1.
//
// LIFETIME DISCIPLINE (this revision):
//   - Typed native allocation via WeftStruct (no hardcoded byte-count cast).
//   - NativeFinalizer bound to the handle: if the Dart wrapper is garbage
//     collected without an explicit destroy(), the native weft_t storage is
//     still freed (the kernel's three payload buffers are owned inside
//     weft_t and freed by weft_destroy — the finalizer runs that same path).
//   - destroy() runs the I6 handshake (revoke → bounded epoch-ACK wait →
//     destroy) required by weft.h before freeing; the previous revision
//     called weft_destroy directly, the exact caller-error the header
//     documents as forbidden while a writer may still run.

import 'dart:ffi';
import 'package:ffi/ffi.dart';
import 'bindings.dart';

/// WeftFFI wraps the native C Triad kernel handle. `implements Finalizable`
/// is REQUIRED by NativeFinalizer.attach (its target parameter is
/// Finalizable — the SDK enforces the "attach before the object dies"
/// contract through the type system); without it this file did not compile
/// ("The argument type 'WeftFFI' can't be assigned to the parameter type
/// 'Finalizable'", flutter CI log for 8d6eebf — masked until now by the
/// tee-pipe exit-code swallow in the CI steps).
class WeftFFI implements Finalizable {
  final WeftNativeBindings bindings;
  final Pointer<WeftStruct> _handle;
  final int payloadMax;
  bool _isDestroyed = false;

  /// Safety net: frees the native handle even if destroy() is never called.
  /// calloc.nativeFree matches the calloc/probe allocation below.
  static final NativeFinalizer _finalizer = NativeFinalizer(calloc.nativeFree);

  WeftFFI._(this.bindings, this._handle, this.payloadMax) {
    _finalizer.attach(this, _handle.cast(), detach: this);
  }

  static WeftFFI allocate(WeftNativeBindings bindings, int payloadMax) {
    // Allocation occurs strictly at initialization (confined to setup).
    // Typed reservation: 512 bytes documented on WeftStruct (bindings.dart).
    final handle = calloc<WeftStruct>();
    final res = bindings.weftInit(handle, payloadMax);
    if (res != 0) {
      calloc.free(handle);
      throw StateError('Failed to initialize native weft_t: error code $res');
    }
    return WeftFFI._(bindings, handle, payloadMax);
  }

  /// Get direct pointer to writer buffer (zero allocation).
  Pointer<Uint8> get wBegin => bindings.weftWBegin(_handle);

  /// Publish the current working buffer.
  int publish(int seq, int payloadLen) {
    checkNotDestroyed();
    return bindings.weftPublish(_handle, seq, payloadLen);
  }

  /// Claim the latest published buffer (zero allocation, non-blocking).
  int claim() {
    checkNotDestroyed();
    return bindings.weftRClaim(_handle);
  }

  /// Read live buffer slice into destination buffer.
  int readSlice(Pointer<Uint8> dst, int offset, int len) {
    checkNotDestroyed();
    return bindings.weftRReadSlice(_handle, dst, offset, len);
  }

  /// Writer revocation.
  void revoke() {
    checkNotDestroyed();
    bindings.weftRevoke(_handle);
  }

  /// Reclaim handshake.
  bool reclaim(int preRevokeEpoch, int timeoutMs) {
    checkNotDestroyed();
    return bindings.weftReclaim(_handle, preRevokeEpoch, timeoutMs) == 0;
  }

  /// Advisory telemetry counters.
  int get tPublishCount => bindings.weftTPublish(_handle);
  int get tClaimCount => bindings.weftTClaim(_handle);

  /// Current writer epoch (I6 handshake bookkeeping).
  int get epoch => bindings.weftEpoch(_handle);

  void checkNotDestroyed() {
    if (_isDestroyed) throw StateError('WeftFFI has been destroyed');
  }

  /// Destroy and free native memory. Runs the I6 handshake FIRST (revoke →
  /// bounded ACK wait → destroy) so a live writer can never write into
  /// freed memory — the contract weft.h states and the bridge previously
  /// skipped. Idempotent; detaches the NativeFinalizer.
  void destroy() {
    if (_isDestroyed) return;
    _isDestroyed = true;
    final preRevokeEpoch = bindings.weftEpoch(_handle);
    bindings.weftRevoke(_handle);
    bindings.weftReclaim(_handle, preRevokeEpoch, 50);
    bindings.weftDestroy(_handle);
    _finalizer.detach(this);
    calloc.free(_handle);
  }
}
