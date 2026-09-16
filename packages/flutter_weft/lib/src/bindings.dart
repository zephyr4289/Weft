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

  WeftNativeBindings(this.dylib) {
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
