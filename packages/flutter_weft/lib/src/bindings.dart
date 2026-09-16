// bindings.dart — dart:ffi signatures over frozen C ABI (core/c/weft.h)
//
// WHY EXISTS: Directly invokes the frozen C kernel ABI without glue overhead
// per WHITEPAPER §8.4 and DIRECTIVE-14 T14.1.

import 'dart:ffi';
import 'dart:io';

// Opaque handle for the C `weft_t`. The previous revision allocated a
// hardcoded `calloc<Uint8>(512)` byte block and cast it — correct today,
// silent corruption the day the kernel struct grows past 512 bytes. This
// typed declaration reserves the same 512 bytes EXPLICITLY and documents
// the budget: core/c weft_t is ~104 bytes (3 pointers + 2 size_t + index
// atomics + revocation state + 5 u64 telemetry counters). A mechanical
// guard `_Static_assert(sizeof(weft_t) <= 512, ...)` lives in
// android/weft-core/src/main/cpp/weft_jni.c — raise this reservation if a
// kernel change (RFC-gated) grows the struct.
final class WeftStruct extends Struct {
  @Array(512)
  external Array<Uint8> storage;
}

// C prototypes
typedef WeftInitC = Int32 Function(Pointer<WeftStruct> w, IntPtr payloadMax);
typedef WeftInitDart = int Function(Pointer<WeftStruct> w, int payloadMax);

typedef WeftDestroyC = Void Function(Pointer<WeftStruct> w);
typedef WeftDestroyDart = void Function(Pointer<WeftStruct> w);

typedef WeftWBeginC = Pointer<Uint8> Function(Pointer<WeftStruct> w);
typedef WeftWBeginDart = Pointer<Uint8> Function(Pointer<WeftStruct> w);

typedef WeftPublishC = Int32 Function(Pointer<WeftStruct> w, Uint32 seq, Uint32 payloadLen);
typedef WeftPublishDart = int Function(Pointer<WeftStruct> w, int seq, int payloadLen);

typedef WeftRClaimC = Uint32 Function(Pointer<WeftStruct> w);
typedef WeftRClaimDart = int Function(Pointer<WeftStruct> w);

typedef WeftRReadSliceC = IntPtr Function(Pointer<WeftStruct> w, Pointer<Uint8> dst, IntPtr offset, IntPtr len);
typedef WeftRReadSliceDart = int Function(Pointer<WeftStruct> w, Pointer<Uint8> dst, int offset, int len);

typedef WeftRevokeC = Void Function(Pointer<WeftStruct> w);
typedef WeftRevokeDart = void Function(Pointer<WeftStruct> w);

typedef WeftReclaimC = Int32 Function(Pointer<WeftStruct> w, Uint32 preRevokeEpoch, Uint32 timeoutMs);
typedef WeftReclaimDart = int Function(Pointer<WeftStruct> w, int preRevokeEpoch, int timeoutMs);

typedef WeftTPublishC = Uint64 Function(Pointer<WeftStruct> w);
typedef WeftTPublishDart = int Function(Pointer<WeftStruct> w);

typedef WeftTClaimC = Uint64 Function(Pointer<WeftStruct> w);
typedef WeftTClaimDart = int Function(Pointer<WeftStruct> w);

typedef WeftEpochC = Uint32 Function(Pointer<WeftStruct> w);
typedef WeftEpochDart = int Function(Pointer<WeftStruct> w);

// ---------------------------------------------------------------------------
// Fan-out ring (RFC 0004) — core/c/fanout.h ABI.
//
// Handles are C-ALLOCATED (weft_fanout_new / weft_fanout_reader_new): no
// WeftStruct-style byte reservation is needed on the Dart side, and the
// NativeFinalizer calls the C free function DIRECTLY (see fanout_ffi.dart)
// so the ring (and the reader's copy buffer) always die with the handle,
// in the right order — the exact property a split allocate/destroy pair
// cannot give a GC backstop.
//
// The C ring is BYTE-COMPATIBLE with core/ts/fanout.ts: latestSeq at ring+0,
// publishes at ring+8, slotSeq[k] at 16+8k, payload slots at 16+8M. Reader
// handles attach to ANY such memory (weft_fanout_reader_new takes the ring
// pointer), so a reader can consume a ring produced by the TS port living
// in native memory; the broadcaster side here always creates its own ring.
// ---------------------------------------------------------------------------

/// C weft_fanout_claim_t { bool fresh; uint64_t seq; uint64_t dropped; }.
/// Dart FFI applies C ABI layout rules (Uint8 at 0, Uint64 aligned to 8) —
/// the same packing the C compiler produces, verified by the fanout test
/// suite reading fresh/seq/dropped through this view.
final class FanoutClaimStruct extends Struct {
  @Uint8()
  external int fresh; // C bool: 0 / 1
  @Uint64()
  external int seq;
  @Uint64()
  external int dropped;
}

/// C weft_fanout_stats_t — advisory reader statistics (AXIOM T).
final class FanoutReaderStatsStruct extends Struct {
  @Uint64()
  external int reads;
  @Uint64()
  external int fresh;
  @Uint64()
  external int drops;
  @Uint64()
  external int skippedMidOverwrite;
  @Uint64()
  external int tornExhausted;
}

typedef FanoutNewC = Pointer<Void> Function(IntPtr payloadBytes, Uint32 slotCount);
typedef FanoutNewDart = Pointer<Void> Function(int payloadBytes, int slotCount);

typedef FanoutFreeC = Void Function(Pointer<Void> f);
typedef FanoutFreeDart = void Function(Pointer<Void> f);

typedef FanoutRingBytesC = IntPtr Function(IntPtr payloadBytes, Uint32 slotCount);
typedef FanoutRingBytesDart = int Function(int payloadBytes, int slotCount);

typedef FanoutBeginC = Pointer<Uint8> Function(Pointer<Void> f);
typedef FanoutBeginDart = Pointer<Uint8> Function(Pointer<Void> f);

typedef FanoutFillC = Int32 Function(Pointer<Void> f, Pointer<Uint8> src, IntPtr len);
typedef FanoutFillDart = int Function(Pointer<Void> f, Pointer<Uint8> src, int len);

typedef FanoutPublishC = Uint64 Function(Pointer<Void> f);
typedef FanoutPublishDart = int Function(Pointer<Void> f);

typedef FanoutRingC = Pointer<Void> Function(Pointer<Void> f);
typedef FanoutRingDart = Pointer<Void> Function(Pointer<Void> f);

typedef FanoutReaderNewC = Pointer<Void> Function(
    Pointer<Void> ring, IntPtr ringBytes, IntPtr payloadBytes, Uint32 slotCount);
typedef FanoutReaderNewDart = Pointer<Void> Function(
    Pointer<Void> ring, int ringBytes, int payloadBytes, int slotCount);

typedef FanoutReaderFreeC = Void Function(Pointer<Void> r);
typedef FanoutReaderFreeDart = void Function(Pointer<Void> r);

typedef FanoutClaimC = Pointer<FanoutClaimStruct> Function(Pointer<Void> r);
typedef FanoutClaimDart = Pointer<FanoutClaimStruct> Function(Pointer<Void> r);

typedef FanoutViewC = Pointer<Uint8> Function(Pointer<Void> r);
typedef FanoutViewDart = Pointer<Uint8> Function(Pointer<Void> r);

typedef FanoutReaderStatsC = Void Function(Pointer<Void> r, Pointer<FanoutReaderStatsStruct> out);
typedef FanoutReaderStatsDart = void Function(Pointer<Void> r, Pointer<FanoutReaderStatsStruct> out);

class WeftNativeBindings {
  final DynamicLibrary dylib;

  late final WeftInitDart weftInit;
  late final WeftDestroyDart weftDestroy;
  late final WeftWBeginDart weftWBegin;
  late final WeftPublishDart weftPublish;
  late final WeftRClaimDart weftRClaim;
  late final WeftRReadSliceDart weftRReadSlice;
  late final WeftRevokeDart weftRevoke;
  late final WeftReclaimDart weftReclaim;
  late final WeftTPublishDart weftTPublish;
  late final WeftTClaimDart weftTClaim;
  late final WeftEpochDart weftEpoch;

  // Fan-out (RFC 0004).
  late final FanoutNewDart fanoutNew;
  late final FanoutFreeDart fanoutFree;
  late final FanoutRingBytesDart fanoutRingBytes;
  late final FanoutBeginDart fanoutBegin;
  late final FanoutFillDart fanoutFill;
  late final FanoutPublishDart fanoutPublish;
  late final FanoutRingDart fanoutRing;
  late final FanoutReaderNewDart fanoutReaderNew;
  late final FanoutReaderFreeDart fanoutReaderFree;
  late final FanoutClaimDart fanoutClaim;
  late final FanoutViewDart fanoutView;
  late final FanoutReaderStatsDart fanoutReaderStats;

  /// Raw native pointers for NativeFinalizer (which requires the native
  /// function pointer, not a Dart closure). weft_fanout_free has exactly
  /// the void(void*) signature a finalizer callback needs, and it frees the
  /// ring AND the struct in the right order — the property that makes the
  /// GC backstop safe.
  final Pointer<NativeFunction<Void Function(Pointer<Void>)>> fanoutFreeNative;
  final Pointer<NativeFunction<Void Function(Pointer<Void>)>> fanoutReaderFreeNative;

  WeftNativeBindings(this.dylib)
      : fanoutFreeNative = dylib.lookup('weft_fanout_free'),
        fanoutReaderFreeNative = dylib.lookup('weft_fanout_reader_free') {
    weftInit = dylib.lookupFunction<WeftInitC, WeftInitDart>('weft_init');
    weftDestroy = dylib.lookupFunction<WeftDestroyC, WeftDestroyDart>('weft_destroy');
    weftWBegin = dylib.lookupFunction<WeftWBeginC, WeftWBeginDart>('weft_w_begin');
    weftPublish = dylib.lookupFunction<WeftPublishC, WeftPublishDart>('weft_publish');
    weftRClaim = dylib.lookupFunction<WeftRClaimC, WeftRClaimDart>('weft_r_claim');
    weftRReadSlice = dylib.lookupFunction<WeftRReadSliceC, WeftRReadSliceDart>('weft_r_read_slice');
    weftRevoke = dylib.lookupFunction<WeftRevokeC, WeftRevokeDart>('weft_revoke');
    weftReclaim = dylib.lookupFunction<WeftReclaimC, WeftReclaimDart>('weft_reclaim');
    weftTPublish = dylib.lookupFunction<WeftTPublishC, WeftTPublishDart>('weft_t_publish');
    weftTClaim = dylib.lookupFunction<WeftTClaimC, WeftTClaimDart>('weft_t_claim');
    weftEpoch = dylib.lookupFunction<WeftEpochC, WeftEpochDart>('weft_epoch');

    fanoutNew = dylib.lookupFunction<FanoutNewC, FanoutNewDart>('weft_fanout_new');
    fanoutFree = dylib.lookupFunction<FanoutFreeC, FanoutFreeDart>('weft_fanout_free');
    fanoutRingBytes =
        dylib.lookupFunction<FanoutRingBytesC, FanoutRingBytesDart>('weft_fanout_ring_bytes');
    fanoutBegin = dylib.lookupFunction<FanoutBeginC, FanoutBeginDart>('weft_fanout_begin');
    fanoutFill = dylib.lookupFunction<FanoutFillC, FanoutFillDart>('weft_fanout_fill');
    fanoutPublish = dylib.lookupFunction<FanoutPublishC, FanoutPublishDart>('weft_fanout_publish');
    fanoutRing = dylib.lookupFunction<FanoutRingC, FanoutRingDart>('weft_fanout_ring');
    fanoutReaderNew =
        dylib.lookupFunction<FanoutReaderNewC, FanoutReaderNewDart>('weft_fanout_reader_new');
    fanoutReaderFree =
        dylib.lookupFunction<FanoutReaderFreeC, FanoutReaderFreeDart>('weft_fanout_reader_free');
    fanoutClaim = dylib.lookupFunction<FanoutClaimC, FanoutClaimDart>('weft_fanout_claim');
    fanoutView = dylib.lookupFunction<FanoutViewC, FanoutViewDart>('weft_fanout_view');
    fanoutReaderStats =
        dylib.lookupFunction<FanoutReaderStatsC, FanoutReaderStatsDart>('weft_fanout_reader_stats');
  }

  static DynamicLibrary openLibrary([String? path]) {
    if (path != null) return DynamicLibrary.open(path);
    if (Platform.isLinux || Platform.isAndroid) {
      try {
        return DynamicLibrary.open('libweft.so');
      } catch (_) {
        return DynamicLibrary.open('libweft_core.so');
      }
    } else if (Platform.isMacOS || Platform.isIOS) {
      try {
        return DynamicLibrary.open('libweft.dylib');
      } catch (_) {
        return DynamicLibrary.process();
      }
    } else if (Platform.isWindows) {
      return DynamicLibrary.open('weft.dll');
    }
    return DynamicLibrary.process();
  }
}
