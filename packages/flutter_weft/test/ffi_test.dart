import 'dart:ffi';
import 'dart:io';
import 'package:ffi/ffi.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:weft_flutter/weft_flutter.dart';

void main() {
  group('Weft FFI & Invariant Tests', () {
    late DynamicLibrary dylib;
    late WeftNativeBindings bindings;

    setUpAll(() {
      // Look for compiled libweft.so in build or local directory
      final soPaths = [
        'libweft.so',
        'build/libweft.so',
        '/tmp/libweft.so',
        '../../build/libweft.so'
      ];
      String? foundPath;
      for (final p in soPaths) {
        if (File(p).existsSync()) {
          foundPath = p;
          break;
        }
      }
      dylib = WeftNativeBindings.openLibrary(foundPath);
      bindings = WeftNativeBindings(dylib);
    });

    test('10^6 Exchanges and Invariants Verification', () {
      final payloadMax = 256;
      final weft = WeftFFI.allocate(bindings, payloadMax);
      final readBuf = calloc<Uint8>(payloadMax);

      try {
        expect(weft.tPublishCount, 0);
        expect(weft.tClaimCount, 0);

        // Run 1,000,000 roundtrip publish & claims
        const iterations = 1000000;
        for (var i = 1; i <= iterations; i++) {
          final wBuf = weft.wBegin;
          wBuf[0] = i & 0xFF;
          wBuf[1] = (i >> 8) & 0xFF;

          final pubRes = weft.publish(i, payloadMax);
          expect(pubRes, 0); // PubResult.OK

          final slot = weft.claim();
          expect(slot >= 0 && slot <= 2, true);

          final readBytes = weft.readSlice(readBuf, 16, 2);
          expect(readBytes, 2);
          expect(readBuf[0], i & 0xFF);
          expect(readBuf[1], (i >> 8) & 0xFF);
        }

        expect(weft.tPublishCount, iterations);
        expect(weft.tClaimCount, iterations);
      } finally {
        calloc.free(readBuf);
        weft.destroy();
      }
    });

    test('Revocation and Reclaim Invariants (I6)', () {
      final weft = WeftFFI.allocate(bindings, 128);
      try {
        weft.revoke();
        final dropRes = weft.publish(1, 128);
        expect(dropRes, 1); // DROPPED_REVOKED

        final reclaimed = weft.reclaim(0, 100);
        expect(reclaimed, true);
      } finally {
        weft.destroy();
      }
    });
  });
}
