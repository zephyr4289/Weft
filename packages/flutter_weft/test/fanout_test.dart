// fanout_test.dart — RFC-0004 fan-out driver-layer conformance suite, Dart.
//
// The F-series counterpart of packages/core/test/fanout.test.ts,
// core/c/fanout_test.c, FanoutTest.kt, and FanoutTests.swift: geometry, the
// stamp-then-fill writer protocol, per-reader fresh/drop accounting, the
// graceful-skip tear discipline, the canonical four-consumer scenario, the
// zero-allocation identity contract (Law 2), and the raw-bytes layout
// parity check (the cross-port wire contract). SINGLE-ISOLATE schedule —
// the Dart port's honesty boundary: no cross-thread claims are made or
// tested here; the concurrent torture gates live in the C/Kotlin/Swift
// batteries and the C ring via FFI.
//
// Payload mixer: weft_mix32-based u32 words (04-LITMUS §0.1 pattern family —
// the same generator as the C/Kotlin/Swift F-series and the xlang
// fixtures), so payload validation is bit-exact against the shared family.
//
// Environment tag for any timing-sensitive numbers: dart-test (sandbox).

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:weft_flutter/weft_flutter.dart';

// --- shared mixer (weft_mix32, 04-LITMUS §0.1 — identical to core/c) ---

int _mix32(int xIn) {
  var x = xIn;
  x = (x ^ (x >>> 16)) & 0xFFFFFFFF;
  x = (x * 0x7FEB352D) & 0xFFFFFFFF;
  x = (x ^ (x >>> 15)) & 0xFFFFFFFF;
  x = (x * 0x846CA68B) & 0xFFFFFFFF;
  x = (x ^ (x >>> 16)) & 0xFFFFFFFF;
  return x;
}

int _tword(int seq, int w) => _mix32((seq * 2654435761 + w) & 0xFFFFFFFF);

Uint32List _mixerFrame(int words, int seq) =>
    Uint32List.fromList(
        List<int>.generate(words, (w) => _tword(seq, w)));

void _fillFrame(WeftFanoutBroadcaster b, int seq, int words) {
  final view = b.begin();
  final frame = _mixerFrame(words, seq);
  for (var w = 0; w < words; w++) {
    view.setUint32(4 * w, frame[w], Endian.little);
  }
}

bool _expectFrame(Uint32List view, int seq, int words) {
  for (var w = 0; w < words; w++) {
    if (view[w] != _tword(seq, w)) return false;
  }
  return true;
}

void main() {
  group('fanout construction and geometry', () {
    test('rejects bad geometry; ring_bytes formula has TS/C parity', () {
      expect(() => WeftFanoutBroadcaster(0, 4), throwsArgumentError);
      expect(() => WeftFanoutBroadcaster(6, 4), throwsArgumentError); // %4 != 0
      expect(() => WeftFanoutBroadcaster(256, 1), throwsArgumentError); // < 2
      expect(() => WeftFanoutBroadcaster(256, 0), throwsArgumentError);
      expect(() => WeftFanoutBroadcaster(256, weftFanoutMaxSlots + 1),
          throwsArgumentError);
      // 16 + 8M + M*payload_bytes — the interop contract.
      expect(weftFanoutRingBytes(256, 4), 16 + 8 * 4 + 4 * 256);
      expect(weftFanoutRingBytes(6, 4), 0);
    });

    test('initial state: no frame, nothing resident, zero telemetry', () {
      final b = WeftFanoutBroadcaster(256, 4);
      final st = b.debugStats();
      expect(st.latestSeq, 0);
      expect(st.publishes, 0);
      expect(st.slotStamps, everyElement(0));
      final r = b.createReader();
      final c = r.claim();
      expect(c.fresh, isFalse);
      expect(c.seq, 0);
      expect(c.dropped, 0);
    });

    test('publish() before any begin() is a detectable no-op', () {
      final b = WeftFanoutBroadcaster(256, 4);
      expect(b.publish(), 0);
      expect(b.debugStats().publishes, 0);
    });
  });

  group('fanout writer + reader protocol', () {
    test('one publish -> fresh claim, seq 1, dropped 0, payload intact', () {
      final b = WeftFanoutBroadcaster(256, 4);
      final r = b.createReader();
      _fillFrame(b, 1, 64);
      expect(b.publish(), 1);
      final c = r.claim();
      expect(c.fresh, isTrue);
      expect(c.seq, 1);
      expect(c.dropped, 0);
      expect(_expectFrame(r.view(), 1, 64), isTrue);
    });

    test('dropped counts frames completed without this reader seeing them',
        () {
      final b = WeftFanoutBroadcaster(256, 4);
      _fillFrame(b, 1, 64);
      b.publish();
      final r = b.createReader();
      r.claim(); // lastSeq = 1 (the TS/C F3 structure: claim the baseline first)
      for (var f = 2; f <= 6; f++) {
        _fillFrame(b, f, 64);
        b.publish();
      }
      final c = r.claim(); // jumps to frame 6; frames 2..5 dropped
      expect(c.fresh, isTrue);
      expect(c.seq, 6);
      expect(c.dropped, 4); // frames 2..5 unseen
      expect(_expectFrame(r.view(), 6, 64), isTrue);
    });

    test('ring overwrite: after M+3 publishes the claim yields the LATEST',
        () {
      final b = WeftFanoutBroadcaster(256, 4);
      final r = b.createReader();
      for (var f = 1; f <= 7; f++) {
        _fillFrame(b, f, 64);
        b.publish();
      }
      final c = r.claim();
      expect(c.fresh, isTrue);
      expect(c.seq, 7);
      expect(_expectFrame(r.view(), 7, 64), isTrue);
    });

    test('telescoping identity holds exactly across an interleaved run', () {
      final b = WeftFanoutBroadcaster(256, 4);
      final r = b.createReader();
      var sumDropped = 0;
      var freshClaims = 0;
      for (var seq = 1; seq <= 60; seq++) {
        _fillFrame(b, seq, 64);
        b.publish();
        if (seq % 3 == 0) {
          final c = r.claim();
          if (c.fresh) {
            sumDropped += c.dropped;
            freshClaims++;
          }
        }
      }
      final c = r.claim();
      if (c.fresh) {
        sumDropped += c.dropped;
        freshClaims++;
      }
      // RFC 0004: sum(dropped) == lastSeq - freshClaims, exact.
      expect(sumDropped, 60 - freshClaims);
      expect(c.seq, 60);
    });
  });

  group('fanout tear discipline (graceful skip)', () {
    test('a reader skips the tick when its target slot is mid-overwrite', () {
      final b = WeftFanoutBroadcaster(64, 4);
      _fillFrame(b, 1, 16);
      b.publish();
      final r = b.createReader();
      expect(r.claim().seq, 1); // lastSeq = 1
      for (var f = 2; f <= 4; f++) {
        _fillFrame(b, f, 16);
        b.publish();
      }
      // latest = 4 lives in slot 3. M abandoned begins advance wSeq to 8;
      // the 4th invalidates slot 3 — the very slot latest points at.
      for (var i = 0; i < 4; i++) {
        b.begin();
      }
      final c = r.claim();
      expect(c.fresh, isFalse); // graceful skip, not a torn frame
      expect(c.seq, 1); // keeps the last consistent frame
      expect(r.stats().skippedMidOverwrite, 1);
      // The in-flight frame completes -> the next claim resumes cleanly.
      _fillFrame(b, 9, 16);
      b.publish();
      final c2 = r.claim();
      expect(c2.fresh, isTrue);
      expect(c2.seq, 9);
      expect(c2.dropped, 7); // frames 2..8 gap over lastSeq=1
      expect(_expectFrame(r.view(), 9, 16), isTrue);
    });

    test('abandoned begins are skip-observable, not silent', () {
      final b = WeftFanoutBroadcaster(64, 4);
      for (var f = 1; f <= 4; f++) {
        _fillFrame(b, f, 16);
        b.publish();
      }
      for (var i = 0; i < 4; i++) {
        b.begin(); // none ever published
      }
      final r = b.createReader();
      final c = r.claim();
      expect(c.fresh, isFalse);
      expect(c.seq, 0);
      expect(r.stats().skippedMidOverwrite, 1);
      _fillFrame(b, 9, 16);
      b.publish();
      final c2 = r.claim();
      expect(c2.fresh, isTrue);
      expect(c2.seq, 9);
      // Seqs 5..8 never completed, yet read as 8 drops — abandoned seqs are
      // indistinguishable from missed publishes (declared boundary).
      expect(c2.dropped, 8);
    });
  });

  group('fanout canonical four-consumer scenario', () {
    test('exact per-reader accounting over 10,000 frames at 120/60/30/15 Hz',
        () {
      const n = 10000;
      final b = WeftFanoutBroadcaster(256, 4);
      final consumers = <String, (int, WeftFanoutReader)>{
        'flight-recorder': (1, b.createReader()),
        'primary-canvas': (2, b.createReader()),
        'minimap': (4, b.createReader()),
        'network-viz': (8, b.createReader()),
      };
      var integrityFailures = 0;
      for (var t = 1; t <= n; t++) {
        _fillFrame(b, t, 64);
        b.publish();
        for (final e in consumers.entries) {
          final divisor = e.value.$1;
          final reader = e.value.$2;
          if (t % divisor == 0) {
            final claim = reader.claim();
            if (!claim.fresh) {
              integrityFailures++;
            } else {
              if (claim.dropped != divisor - 1) integrityFailures++;
              if (!_expectFrame(reader.view(), claim.seq, 64)) {
                integrityFailures++;
              }
            }
          }
        }
      }
      expect(integrityFailures, 0);
      for (final e in consumers.entries) {
        final divisor = e.value.$1;
        final reader = e.value.$2;
        final st = reader.stats();
        final expectedFresh = n ~/ divisor;
        expect(st.reads, expectedFresh);
        expect(st.fresh, expectedFresh);
        expect(st.drops, n - expectedFresh);
        expect(st.skippedMidOverwrite, 0); // single-isolate schedule
        expect(st.tornExhausted, 0);
        expect(reader.claim().seq, n);
      }
    });
  });

  group('fanout zero-allocation contract (Law 2)', () {
    test('claim() mutates one record; view() is one buffer — identity-stable',
        () {
      final b = WeftFanoutBroadcaster(256, 4);
      final r = b.createReader();
      final firstClaim = r.claim();
      final firstView = r.view();
      for (var f = 1; f <= 1000; f++) {
        _fillFrame(b, f, 64);
        b.publish();
        final c = r.claim();
        if (!identical(c, firstClaim)) fail('claim record identity changed');
        if (!identical(r.view(), firstView)) fail('view buffer identity changed');
      }
    });
  });

  group('fanout layout parity (the cross-port wire contract)', () {
    test('raw ctrl bytes match the shared layout; foreign bytes attach', () {
      final b = WeftFanoutBroadcaster(64, 4);
      _fillFrame(b, 1, 16);
      b.publish();
      final raw = b.ringBytes();
      final ctrl = ByteData.view(raw.buffer, 0, raw.length);
      expect(ctrl.getUint64(0, Endian.little), 1); // latestSeq
      expect(ctrl.getUint64(8, Endian.little), 1); // publishes
      expect(ctrl.getUint64(16, Endian.little), 1); // slotSeq[0]
      expect(ctrl.getUint64(24, Endian.little), 0); // slotSeq[1] untouched -> 0
      // payload word 0 of slot 0 at 16 + 8*4 = byte 48 (LE u32)
      expect(ctrl.getUint32(48, Endian.little), _tword(1, 0));

      // A reader over a COPY of the bytes (the FFI/interop story: any port's
      // ring bytes, same geometry) attaches and claims cleanly.
      final foreign = Uint8List.fromList(raw);
      final r2 = WeftFanoutReader(foreign, 64, 4);
      final c = r2.claim();
      expect(c.fresh, isTrue);
      expect(c.seq, 1);
      expect(_expectFrame(r2.view(), 1, 16), isTrue);

      // Geometry mismatch fails fast instead of tearing.
      expect(() => WeftFanoutReader(foreign, 64, 8), throwsArgumentError);
      expect(() => WeftFanoutReader(foreign, 512, 4), throwsArgumentError);
    });

    test('wordToFloat round-trips a Float32 bit pattern', () {
      final r = WeftFanoutBroadcaster(8, 2).createReader();
      // 1.0f = 0x3F800000 LE; -2.0f = 0xC0000000.
      expect(r.wordToFloat(0x3F800000), 1.0);
      expect(r.wordToFloat(0xC0000000), -2.0);
    });
  });
}
