// weft_ffi.dart — Production Weft FFI channel wrapper with zero steady-state allocation
//
// WHY EXISTS: Wraps the native C Triad kernel for Flutter with zero steady-state
// Dart allocations during paint/exchange loops per WHITEPAPER §8.4 and DIRECTIVE-14 T14.1.

import 'dart:ffi';
import 'package:ffi/ffi.dart';
import 'bindings.dart';

class WeftFFI {
  final WeftNativeBindings bindings;
  final Pointer<WeftStruct> _handle;
  final int payloadMax;
  bool _isDestroyed = false;

  WeftFFI._(this.bindings, this._handle, this.payloadMax);

  static WeftFFI allocate(WeftNativeBindings bindings, int payloadMax) {
    // Allocation occurs strictly at initialization (confined to setup)
    final handle = calloc<Uint8>(512).cast<WeftStruct>();
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

  void checkNotDestroyed() {
    if (_isDestroyed) throw StateError('WeftFFI has been destroyed');
  }

  /// Destroy and free native memory.
  void destroy() {
    if (_isDestroyed) return;
    _isDestroyed = true;
    bindings.weftDestroy(_handle);
    calloc.free(_handle);
  }
}
