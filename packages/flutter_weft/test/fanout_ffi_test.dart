// fanout_ffi_test.dart — RFC-0004 fan-out ring over the real C library
//
// Mirrors the F-series contract the TS port (packages/core/test/
// fanout.test.ts), the C port (core/c/fanout_test.c), the JVM harness
// (fixtures/jni-fanout) and the Kotlin suite (FanoutTest.kt) are pinned
// by — plus the one test only Dart can run: cross-ISOLATE consumers on
// the same native ring (real OS threads, handle passed as its raw
// address per the D-14 FFI lifetime discipline).
//
// CI-GATED (declared, D-12/D-14 precedent): needs libweft.so built from
// core/c/weft.c + core/c/fanout.c (the flutter workflow compiles it).
// CI-GATED, not contributor-sandbox-verified: no Flutter SDK in the
// contribution sandbox — the same declaration every flutter_weft change
// carries.

import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'package:ffi/ffi.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:weft_flutter/weft_flutter.dart';

// The kernel's shared payload pattern (04-LITMUS §0.1), Dart port — an
// INDEPENDENT reimplementation, the fixtures/xlang-fanout discipline.
int _mix32(int x) {
  int v = x & 0xFFFFFFFF;
  v = (v ^ (v >>> 16)) & 0xFFFFFFFF;
  v = (v * 0x7FEB352D) & 0xFFFFFFFF;
  v = (v ^ (v >>> 15)) & 0xFFFFFFFF;
  v = (v * 0x846CA68B) & 0xFFFFFFFF;
  v = (v ^ (v >>> 16)) & 0xFFFFFFFF;
  return v;
}

int _pat(int seq, int i) {
  final x = (seq * 2654435761 + i * 2246822519) & 0xFFFFFFFF;
  return _mix32(x) & 0xFF;
}

void main() {
  final bindings = WeftNativeBindings(_openLib());

  // The .so path, resolved once, so isolate closures can reopen it.
  final soPath = _soPath;

  test('DF1 geometry validation refuses bad pairs', () {
    expect(() => WeftFanoutFFI.allocate(bindings, 0, 4), throwsStateError);
    expect(() => WeftFanoutFFI.allocate(bindings, 6, 4), throwsStateError); // not %4
    expect(() => WeftFanoutFFI.allocate(bindings, 64, 1), throwsStateError); // <2
    expect(() => WeftFanoutFFI.allocate(bindings, 64, 65), throwsStateError); // >64
  });

  test('DF2 ring_bytes is the byte-layout interop contract', () {
    expect(WeftFanoutFFI.ringBytes(bindings, 256, 4), 16 + 8 * 4 + 4 * 256);
    expect(WeftFanoutFFI.ringBytes(bindings, 6, 4), 0); // bad geometry -> 0
  });

  test('DF3 roundtrip via the zero-copy begin cursor', () {
    final b = WeftFanoutFFI.allocate(bindings, 256, 4);
    final r = b.createReader();
    try {
      final cur = b.begin();
      expect(cur.address, isNot(0));
      for (var i = 0; i < 256; i++) {
        cur[i] = _pat(1, i);
      }
      expect(b.publish(), 1);

      final rec = r.claim();
      expect(rec.ref.fresh, 1);
      expect(rec.ref.seq, 1);
      expect(rec.ref.dropped, 0);

      final view = r.view();
      var ok = true;
      for (var i = 0; i < 256; i++) {
        if (view[i] != _pat(1, i)) ok = false;
      }
      expect(ok, isTrue, reason: 'claimed bytes match pat(1,i) — no tear');

      // Advisory state via the documented ring offsets (AXIOM T).
      expect(b.latestSeq, 1);
      expect(b.publishes, 1);
    } finally {
      r.destroy();
      b.destroy();
    }
  });

  test('DF4 telescoping identity is exact', () {
    final b = WeftFanoutFFI.allocate(bindings, 256, 4);
    final r = b.createReader();
    try {
      var fresh = 0, sumDropped = 0, lastSeq = 0;
      for (var seq = 1; seq <= 40; seq++) {
        final cur = b.begin();
        for (var i = 0; i < 256; i++) {
          cur[i] = _pat(seq, i);
        }
        b.publish();
        if (seq % 4 == 0) {
          final rec = r.claim();
          if (rec.ref.fresh != 0) {
            fresh++;
            sumDropped += rec.ref.dropped;
            lastSeq = rec.ref.seq;
          }
        }
      }
      expect(fresh, 10);
      expect(lastSeq, 40);
      expect(sumDropped, 30);
      expect(lastSeq - fresh, sumDropped, reason: 'lastSeq - freshClaims == sum(dropped)');
    } finally {
      r.destroy();
      b.destroy();
    }
  });

  test('DF5 graceful skip: mid-overwrite of the LATEST slot, counted', () {
    final b = WeftFanoutFFI.allocate(bindings, 256, 4);
    final r = b.createReader();
    try {
      var cur = b.begin();
      for (var i = 0; i < 256; i++) {
        cur[i] = _pat(1, i);
      }
      b.publish();
      r.claim(); // reader holds frame 1

      // Publish 2..5 (latest = 5), no claims.
      for (var seq = 2; seq <= 5; seq++) {
        cur = b.begin();
        for (var i = 0; i < 256; i++) {
          cur[i] = _pat(seq, i);
        }
        b.publish();
      }
      // M begins WITHOUT publishing: the 4th re-opens slot 0 (frame 5's
      // own slot) — the mid-overwrite window.
      for (var k = 0; k < 4; k++) {
        b.begin();
      }
      final rec = r.claim();
      expect(rec.ref.fresh, 0, reason: 'mid-overwrite claim skips the tick');
      expect(rec.ref.seq, 1, reason: 'keeps frame 1');
      expect(r.stats().skippedMidOverwrite, 1, reason: 'skip counted, never silent');

      expect(b.publish(), 9, reason: 'publish after the skip (frame 9)');
      final rec2 = r.claim();
      expect(rec2.ref.fresh, 1);
      expect(rec2.ref.seq, 9);
      expect(rec2.ref.dropped, 7, reason: 'frames 2..8 completed unseen');
    } finally {
      r.destroy();
      b.destroy();
    }
  });

  test('DF6 publish without begin is a detectable no-op', () {
    final b = WeftFanoutFFI.allocate(bindings, 256, 4);
    try {
      expect(b.publish(), 0); // no begin ever ran
      final cur = b.begin();
      for (var i = 0; i < 256; i++) {
        cur[i] = _pat(1, i);
      }
      expect(b.publish(), 1);
      expect(b.publish(), 1); // re-stamps the SAME frame (TS parity)
    } finally {
      b.destroy();
    }
  });

  test('DF7 N readers on one ring are fully independent', () {
    final b = WeftFanoutFFI.allocate(bindings, 256, 4);
    final readers = [b.createReader(), b.createReader(), b.createReader()];
    final cadence = [1, 3, 7];
    final fresh = [0, 0, 0];
    final dropped = [0, 0, 0];
    try {
      for (var seq = 1; seq <= 70; seq++) {
        final cur = b.begin();
        for (var i = 0; i < 256; i++) {
          cur[i] = _pat(seq, i);
        }
        b.publish();
        for (var k = 0; k < 3; k++) {
          if (seq % cadence[k] == 0) {
            final rec = readers[k].claim();
            if (rec.ref.fresh != 0) {
              fresh[k]++;
              dropped[k] += rec.ref.dropped;
            }
          }
        }
      }
      for (var k = 0; k < 3; k++) {
        final lastClaimed = (70 ~/ cadence[k]) * cadence[k];
        expect(fresh[k], 70 ~/ cadence[k], reason: 'reader $k fresh claims');
        expect(dropped[k], lastClaimed - fresh[k], reason: 'reader $k telescoping exact');
        expect(readers[k].stats().reads, 70 ~/ cadence[k], reason: 'reader $k reads');
      }
    } finally {
      for (final r in readers) {
        r.destroy();
      }
      b.destroy();
    }
  });

  test('DF8 fill path delivers intact bytes and rejects bad lengths', () {
    final b = WeftFanoutFFI.allocate(bindings, 256, 4);
    final r = b.createReader();
    try {
      final src = calloc<Uint8>(256);
      for (var i = 0; i < 256; i++) {
        src[i] = _pat(9, i);
      }
      b.begin();
      expect(b.fill(src, 256), 64, reason: 'payload/4 words copied');
      expect(b.publish(), 1);
      calloc.free(src);

      final rec = r.claim();
      final view = r.view();
      var ok = true;
      for (var i = 0; i < 256; i++) {
        if (view[i] != _pat(9, i)) ok = false;
      }
      expect(rec.ref.fresh, 1);
      expect(ok, isTrue);
      expect(b.fill(r.view(), 3), -1, reason: 'len%4 != 0 rejected');
    } finally {
      r.destroy();
      b.destroy();
    }
  });

  test('DF9 stats snapshot matches the protocol accounting', () {
    final b = WeftFanoutFFI.allocate(bindings, 256, 4);
    final r = b.createReader();
    try {
      for (var seq = 1; seq <= 3; seq++) {
        final cur = b.begin();
        for (var i = 0; i < 256; i++) {
          cur[i] = _pat(seq, i);
        }
        b.publish();
      }
      final rec = r.claim();
      final s = r.stats();
      expect(s.reads, 1);
      expect(s.fresh, 1);
      expect(s.drops, rec.ref.dropped);
      expect(s.skippedMidOverwrite, 0);
      expect(s.tornExhausted, 0);
    } finally {
      r.destroy();
      b.destroy();
    }
  });

  test('DFT cross-isolate torture: 1 writer, 2 reader isolates, pat validation', () async {
    const frames = 50000;
    const payloadBytes = 256;
    final b = WeftFanoutFFI.allocate(bindings, payloadBytes, 4);
    final r1 = b.createReader();
    final r2 = b.createReader();
    try {
      // Reader isolates: rebuild the reader from the raw handle address
      // (FFI pointers cannot cross isolates — their ADDRESS can; the ring
      // and the reader state are native memory, shared by construction).
      // _ReaderArgs is all String/int fields, so the closures capture it
      // directly (sendable).
      final args1 = _ReaderArgs(
          soPath: soPath, handleAddress: r1.handleAddress, frames: frames, payloadBytes: payloadBytes);
      final args2 = _ReaderArgs(
          soPath: soPath, handleAddress: r2.handleAddress, frames: frames, payloadBytes: payloadBytes);
      final done = Future.wait([
        Isolate.run(() => _readerIsolateLoop(args1)),
        Isolate.run(() => _readerIsolateLoop(args2)),
      ]);

      // Writer: publish all frames from THIS isolate while the reader
      // isolates claim concurrently (single writer by contract).
      for (var seq = 1; seq <= frames; seq++) {
        final cur = b.begin();
        for (var i = 0; i < payloadBytes; i++) {
          cur[i] = _pat(seq, i);
        }
        b.publish();
      }

      final results = await done;
      for (var k = 0; k < results.length; k++) {
        final res = results[k];
        expect(res['violations'], isEmpty,
            reason: 'reader isolate $k: ${res['violations']}');
        expect(res['lastSeq'], frames, reason: 'reader isolate $k converged to the final frame');
        expect((res['fresh'] as int) > 0, isTrue, reason: 'reader isolate $k observed frames');
        expect(
          (res['lastSeq'] as int) - (res['fresh'] as int),
          res['dropped'] as int,
          reason: 'reader isolate $k telescoping exact',
        );
      }
    } finally {
      r1.destroy();
      r2.destroy();
      b.destroy();
    }
  });
}

// ---------------------------------------------------------------------------
// Cross-isolate plumbing
// ---------------------------------------------------------------------------

class _ReaderArgs {
  final String soPath;
  final int handleAddress;
  final int frames;
  final int payloadBytes;
  const _ReaderArgs({
    required this.soPath,
    required this.handleAddress,
    required this.frames,
    required this.payloadBytes,
  });
}

// Module-level: the resolved .so path (set once by _openLib before any
// test body runs; read by isolate closures via the captured args).
String _soPath = 'libweft.so';

Map<String, Object> _readerIsolateLoop(_ReaderArgs a) {
  final dylib = DynamicLibrary.open(a.soPath);
  final bindings = WeftNativeBindings(dylib);
  // A bare-bones reader over the existing handle: claim + view + stats
  // through the same bindings the wrapper uses.
  final handle = Pointer<Void>.fromAddress(a.handleAddress);
  final claim = bindings.fanoutClaim;
  final view = bindings.fanoutView;

  var fresh = 0, sumDropped = 0, lastSeq = 0, torn = 0;
  final violations = <String>[];
  var claims = 0;
  while (lastSeq < a.frames && claims < 50000000) {
    final rec = claim(handle);
    claims++;
    if (rec.ref.fresh != 0) {
      fresh++;
      sumDropped += rec.ref.dropped;
      final v = view(handle);
      final seq = rec.ref.seq;
      for (var i = 0; i < a.payloadBytes; i += 8) {
        if (v[i] != _pat(seq, i)) {
          torn++;
          break;
        }
      }
      if (seq <= lastSeq) violations.add('non-monotonic seq $seq');
      lastSeq = seq;
    }
  }
  if (lastSeq < a.frames) violations.add('starved before ${a.frames} (last=$lastSeq)');
  if (lastSeq - fresh != sumDropped) {
    violations.add('telescoping violated (last=$lastSeq fresh=$fresh dropped=$sumDropped)');
  }
  if (torn > 0) violations.add('$torn torn accepted frames');
  return <String, Object>{
    'fresh': fresh,
    'dropped': sumDropped,
    'lastSeq': lastSeq,
    'claims': claims,
    'violations': violations,
  };
}

DynamicLibrary _openLib() {
  final soPaths = [
    'libweft.so',
    'build/libweft.so',
    'packages/flutter_weft/libweft.so',
    '/tmp/libweft.so',
    '../../build/libweft.so'
  ];
  for (final p in soPaths) {
    if (File(p).existsSync()) {
      _soPath = p;
      return DynamicLibrary.open(p);
    }
  }
  // CI builds libweft.so next to the package; the workflow copies it to
  // /tmp as well. If none exists this test FAILS loudly in CI (missing
  // libweft.so = the gcc step broke) instead of silently skipping.
  return DynamicLibrary.open('libweft.so');
}
