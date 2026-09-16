// fanout_ffi.dart — RFC-0004 fan-out ring, Flutter FFI channel wrapper
//
// WHY EXISTS: Brings the multi-consumer fan-out ring (RFC 0004, Accepted as
// driver-layer pattern) to Flutter through the C implementation
// (core/c/fanout.{h,c}) — the same ring the TS port ships over a
// SharedArrayBuffer and the Android JNI surface binds. One C
// implementation serves TS (SAB), Android (JNI), and Flutter (this file):
// the byte-compatible layout is the contract that makes a single source of
// truth possible.
//
// LIFETIME DISCIPLINE (corrected pattern, stated against the kernel
// wrapper's precedent): handles are allocated AND freed inside C
// (weft_fanout_new / weft_fanout_reader_new — see fanout.h). The
// NativeFinalizer therefore binds the C free function ITSELF, not a
// calloc.free on the handle: if a WeftFanoutFFI is garbage-collected
// without destroy(), C frees the ring (broadcaster) or the copy buffer
// (reader) in the right order, with the handle. Nothing leaks and nothing
// is freed twice.
//
// THREAD RULES (D-14 FFI discipline): readers may claim from any isolate
// reading — the ring is native memory and each reader's claim record is
// reader-owned; the writer (begin/fill/publish) is single-isolate by
// contract, the same contract every port carries. Isolates receive the
// reader handle as its raw address (int) and rebuild the wrapper locally.
//
// LAW 2: claim/publish/begin allocate nothing on the Dart side — claim()
// returns the reader-owned record pointer (stable until that reader's next
// claim); view() returns the reader's stable copy buffer. LAW 1: every
// path is bounded (the C ring's skip/retry accounting); stats() is the
// cold-path snapshot. AXIOM T: all counters advisory.

import 'dart:ffi';
import 'package:ffi/ffi.dart';
import 'bindings.dart';

/// One claim's result — the C record viewed in place. Read the fields
/// synchronously after claim(); do not retain the pointer across claims
/// expecting a snapshot.
typedef FanoutClaimRecord = Pointer<FanoutClaimStruct>;

/// Advisory reader statistics snapshot (cold path — allocates).
class FanoutReaderStats {
  final int reads;
  final int fresh;
  final int drops;
  final int skippedMidOverwrite;
  final int tornExhausted;
  const FanoutReaderStats({
    required this.reads,
    required this.fresh,
    required this.drops,
    required this.skippedMidOverwrite,
    required this.tornExhausted,
  });
}

/// The broadcaster — the writer side. One per stream. Single writer by
/// contract (single isolate for begin/fill/publish).
class WeftFanoutFFI implements Finalizable {
  final WeftNativeBindings bindings;
  final Pointer<Void> _handle;
  final int payloadBytes;
  final int slotCount;
  bool _destroyed = false;

  /// Instance-level finalizer bound to the CALLER's library — the native
  /// pointer comes from the same DynamicLibrary the wrapper uses, so no
  /// second library is opened and no path resolution can fail at class-init
  /// time. (NativeFinalizer instances are lightweight; sharing one static
  /// would require guessing the library path — the kernel wrapper gets away
  /// with a static because calloc.nativeFree is runtime-provided.)
  late final NativeFinalizer _finalizer = NativeFinalizer(bindings.fanoutFreeNative);

  WeftFanoutFFI._(this.bindings, this._handle, this.payloadBytes, this.slotCount) {
    _finalizer.attach(this, _handle, detach: this);
  }

  /// Allocate a fan-out ring. `payloadBytes` must be a positive multiple
  /// of 4; `slotCount` in [2, 64] (RFC 0004 recommends 4-8). Throws
  /// [StateError] on bad geometry or allocation failure — the C layer
  /// validates before any memory is touched.
  static WeftFanoutFFI allocate(WeftNativeBindings bindings, int payloadBytes, int slotCount) {
    final handle = bindings.fanoutNew(payloadBytes, slotCount);
    if (handle == nullptr) {
      throw StateError('weft_fanout_new failed: payloadBytes=$payloadBytes '
          '(multiple of 4?), slotCount=$slotCount (2..64?)');
    }
    return WeftFanoutFFI._(bindings, handle, payloadBytes, slotCount);
  }

  /// Total ring size for a geometry — the byte-layout interop contract:
  /// 16 + 8*slotCount + slotCount*payloadBytes. Useful for sizing a native
  /// buffer a foreign reader will attach to.
  static int ringBytes(WeftNativeBindings bindings, int payloadBytes, int slotCount) =>
      bindings.fanoutRingBytes(payloadBytes, slotCount);

  /// Live write cursor for the NEXT frame: a raw pointer into the slot's
  /// payload (payloadBytes bytes). The slot's stamp is invalidated BEFORE
  /// the cursor is returned (FI1 bracket); writes through it become visible
  /// to readers only after publish(). Valid until the next begin().
  /// Zero Dart allocation.
  Pointer<Uint8> begin() {
    _checkAlive();
    return bindings.fanoutBegin(_handle);
  }

  /// Fill the begun slot from [src] via the C word-copy path (len a
  /// positive multiple of 4, <= payloadBytes). Returns words written, or
  /// -1 on a bad length / no begin(). Zero Dart allocation.
  int fill(Pointer<Uint8> src, int len) {
    _checkAlive();
    return bindings.fanoutFill(_handle, src, len);
  }

  /// Publish the begun frame. Returns the frame seq, or 0 if no begin()
  /// ever ran (a detectable no-op, TS/C parity). Zero Dart allocation.
  int publish() {
    _checkAlive();
    return bindings.fanoutPublish(_handle);
  }

  /// The ring's base pointer — the byte-layout contract made visible for
  /// interop (a foreign reader attaches to this) and for the advisory
  /// ctrl reads below. Stable for the broadcaster's lifetime.
  Pointer<Void> get ring {
    _checkAlive();
    return bindings.fanoutRing(_handle);
  }

  /// Advisory latestSeq (AXIOM T: unsynchronized read of the ring's
  /// documented offset 0 — advisory, never a correctness reference).
  int get latestSeq => ring.cast<Uint64>()[0];

  /// Advisory publishes counter (ring offset 8, unsynchronized read).
  int get publishes => ring.cast<Uint64>()[1];

  /// Attach a NEW reader to this ring (same process — pass the reader, or
  /// its handle address, to another isolate for cross-isolate consumers).
  WeftFanoutReaderFFI createReader() {
    _checkAlive();
    return WeftFanoutReaderFFI._attach(bindings, ring,
        WeftFanoutFFI.ringBytes(bindings, payloadBytes, slotCount), payloadBytes, slotCount);
  }

  /// Destroy and free the ring + handle (idempotent; detaches the
  /// finalizer). Stop the writer BEFORE calling this — the ring has no I6
  /// handshake (driver layer; the RFC-0004 open question on multi-reader
  /// lifecycle applies: readers must not outlive the memory they read).
  void destroy() {
    if (_destroyed) return;
    _destroyed = true;
    bindings.fanoutFree(_handle);
    _finalizer.detach(this);
  }

  void _checkAlive() {
    if (_destroyed) throw StateError('WeftFanoutFFI has been destroyed');
  }
}

/// A reader bound to a ring (any ring — this broadcaster's, or foreign
/// byte-compatible native memory produced by the TS/C/Rust/JNI side).
/// N per ring, each fully independent; each owns its copy buffer and its
/// claim record.
class WeftFanoutReaderFFI implements Finalizable {
  final WeftNativeBindings bindings;
  final Pointer<Void> _handle;
  final int payloadBytes;
  final int slotCount;
  bool _destroyed = false;

  /// Instance-level finalizer over the caller's library (see WeftFanoutFFI).
  late final NativeFinalizer _finalizer = NativeFinalizer(bindings.fanoutReaderFreeNative);

  WeftFanoutReaderFFI._(this.bindings, this._handle, this.payloadBytes, this.slotCount) {
    _finalizer.attach(this, _handle, detach: this);
  }

  /// Attach a reader to byte-compatible ring memory. Geometry is validated
  /// against ringBytes inside C — a mismatched pair throws instead of
  /// tearing. The memory must outlive the reader (caller-owned).
  static WeftFanoutReaderFFI attach(
      WeftNativeBindings bindings, Pointer<Void> ring, int ringBytes, int payloadBytes,
      int slotCount) {
    return WeftFanoutReaderFFI._attach(bindings, ring, ringBytes, payloadBytes, slotCount);
  }

  static WeftFanoutReaderFFI _attach(WeftNativeBindings bindings, Pointer<Void> ring,
      int ringBytes, int payloadBytes, int slotCount) {
    final handle = bindings.fanoutReaderNew(ring, ringBytes, payloadBytes, slotCount);
    if (handle == nullptr) {
      throw StateError('weft_fanout_reader_new failed: ring=${ring.address.toStringAsFixed(0)} '
          'ringBytes=$ringBytes payloadBytes=$payloadBytes slotCount=$slotCount');
    }
    return WeftFanoutReaderFFI._(bindings, handle, payloadBytes, slotCount);
  }

  /// Rebuild a reader wrapper in another isolate from the raw handle
  /// address (the FFI pointer itself cannot cross isolates; its address
  /// can — the ring and the reader state are native memory).
  static WeftFanoutReaderFFI fromAddress(
      WeftNativeBindings bindings, int handleAddress, int payloadBytes, int slotCount) {
    return WeftFanoutReaderFFI._(
        bindings, Pointer<Void>.fromAddress(handleAddress), payloadBytes, slotCount);
  }

  /// The raw handle address — the value to pass across isolates.
  int get handleAddress => _handle.address;

  /// Claim the freshest completed frame into this reader's copy buffer.
  /// Never blocks, never fails: a tick with no consistent newer frame
  /// returns fresh=0 and the reader keeps its last consistent frame.
  /// Returns the reader-owned claim record (identity-stable until this
  /// reader's next claim — read .fresh / .seq / .dropped synchronously).
  /// Zero Dart allocation.
  FanoutClaimRecord claim() {
    _checkAlive();
    return bindings.fanoutClaim(_handle);
  }

  /// The reader's pre-allocated copy buffer (payloadBytes bytes, stable
  /// identity for the reader's lifetime). Meaningful after a fresh claim;
  /// read it live before the next claim (A3 discipline).
  Pointer<Uint8> view() {
    _checkAlive();
    return bindings.fanoutView(_handle);
  }

  /// Advisory statistics snapshot (cold path — allocates; AXIOM T).
  FanoutReaderStats stats() {
    _checkAlive();
    final out = calloc<FanoutReaderStatsStruct>();
    try {
      bindings.fanoutReaderStats(_handle, out);
      return FanoutReaderStats(
        reads: out.ref.reads,
        fresh: out.ref.fresh,
        drops: out.ref.drops,
        skippedMidOverwrite: out.ref.skippedMidOverwrite,
        tornExhausted: out.ref.tornExhausted,
      );
    } finally {
      calloc.free(out);
    }
  }

  /// Destroy and free the reader + its copy buffer (idempotent; detaches
  /// the finalizer). A reader must not be destroyed while another isolate
  /// is mid-claim through it — one reader, one owner isolate at a time
  /// (hand off, don't share).
  void destroy() {
    if (_destroyed) return;
    _destroyed = true;
    bindings.fanoutReaderFree(_handle);
    _finalizer.detach(this);
  }

  void _checkAlive() {
    if (_destroyed) throw StateError('WeftFanoutReaderFFI has been destroyed');
  }
}
