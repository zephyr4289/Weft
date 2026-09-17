// verified_test.dart — RFC-0005 VerifiedWeft conformance suite, Dart.
//
// The V-series counterpart of core/c/verified_test.c, packages/core/test/
// verified.test.ts, core/rust/src/verified.rs, VerifiedTest.kt, and
// VerifiedTests.swift: shared fixture vectors, derivation, roundtrip,
// exhaustive tamper, rejections, pre-keyed verifier semantics, and the
// Series-6 batch stream API. Tags are bit-identical to every other port by
// construction (same key schedule, same wire format).
//
// Environment tag: `dart test` / `flutter test` (flutter-packages CI); NOT
// runnable in the x86_64 Linux sandbox (no Dart toolchain there) —
// declared, per the repo's per-port honesty culture (PORTS.md §7).
//
// Fixture: byte-identical to fixtures/xlang-verifiedweft/hmac-vectors.json
// (RFC 4231 TC1-4,6,7 + Weft boundary cases; node:crypto cross-checked at
// generation time).

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:weft_flutter/src/reference/verified.dart' as vw;

Uint8List _hex(String s) {
  final out = Uint8List(s.length ~/ 2);
  for (var i = 0; i < out.length; i++) {
    out[i] = int.parse(s.substring(i * 2, i * 2 + 2), radix: 16);
  }
  return out;
}

String _hexOf(Uint8List b) =>
    b.map((x) => x.toRadixString(16).padLeft(2, '0')).join();

class _Vec {
  final String name, key, data, tag;
  const _Vec(this.name, this.key, this.data, this.tag);
}

const _vectors = <_Vec>[
  _Vec(
      'rfc4231-tc1',
      '0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b',
      '4869205468657265',
      'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7'),
  _Vec(
      'rfc4231-tc2',
      '4a656665',
      '7768617420646f2079612077616e7420666f72206e6f7468696e673f',
      '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843'),
  _Vec(
      'rfc4231-tc3',
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
      'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',
      '773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe'),
  _Vec(
      'rfc4231-tc4',
      '0102030405060708090a0b0c0d0e0f10111213141516171819',
      'cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd',
      '82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b'),
  _Vec(
      'rfc4231-tc6',
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
      '54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b6579202d2048617368204b6579204669727374',
      '60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54'),
  _Vec(
      'rfc4231-tc7',
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
      '5468697320697320612074657374207573696e672061206c6172676572207468616e20626c6f636b2d73697a65206b657920616e642061206c6172676572207468616e20626c6f636b2d73697a6520646174612e20546865206b6579206e6565647320746f20626520686173686564206265666f7265206265696e6720757365642062792074686520484d414320616c676f726974686d2e',
      '9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2'),
  _Vec('weft-empty-payload', '5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a',
      '',
      '87a26610b4e32f22d6d403b2397f534fb64c83b15aa53deaec60b1afa31dbb74'),
  _Vec('weft-one-byte', '5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a',
      'ff', '869b6896716dbdbce95aa32d75657fae807c82b8d52c25c83b7afa617271b7c8'),
];

void main() {
  test('V1: HMAC fixture vectors (shared ground truth)', () {
    for (final v in _vectors) {
      final tag = vw.vwHmacSha256(_hex(v.key), _hex(v.data));
      expect(_hexOf(tag), v.tag, reason: '${v.name}: tag matches shared fixture');
    }
  });

  test('V2: domain-separated derivation', () {
    final secret = Uint8List.fromList('v2-secret'.codeUnits);
    final k1 = vw.vwDeriveKey(secret);
    final k2 = vw.vwDeriveKey(secret);
    expect(k1.length, 32, reason: '32-byte key');
    expect(k1, k2, reason: 'deterministic');
    expect(_hexOf(k1) == _hexOf(vw.vwDeriveKey(Uint8List.fromList('other'.codeUnits))),
        false,
        reason: 'different secret -> different key');
    final domain =
        Uint8List.fromList('Weft-VerifiedWeft-v1:key'.codeUnits);
    expect(_hexOf(k1), _hexOf(vw.vwHmacSha256(secret, domain)),
        reason: 'key = HMAC(secret, domain) exactly');
  });

  test('V3: record roundtrip (zero-copy views)', () {
    final key = vw.vwDeriveKey(Uint8List.fromList('v3-roundtrip'.codeUnits));
    final signer = vw.VwSigner(key);
    final envelope = Uint8List(16);
    for (final plen in [0, 1, 16, 63, 64, 65, 128, 300]) {
      final payload = Uint8List.fromList(
          List.generate(plen, (i) => (i * 7 + 3) & 0xff));
      vw.vwEnvelopeEncodeV1(envelope, 42, plen);
      final tag = signer.sign(envelope, payload);
      final rec = Uint8List(16 + plen + 32);
      final n = vw.vwRecordEncode(envelope, payload, tag, rec);
      expect(n, 16 + plen + 32, reason: 'record length ($plen)');
      final r = vw.vwRecordDecodeVerify(key, rec);
      expect(r.code, vw.VwResult.ok, reason: 'decode+verify OK ($plen)');
      expect(r.records.first.payloadLen, plen, reason: 'payload view length');
      expect(r.records.first.seq, 42, reason: 'seq decoded');
    }
  });

  test('V4: exhaustive byte-flip tamper rejection', () {
    final key = vw.vwDeriveKey(Uint8List.fromList('v4-tamper'.codeUnits));
    final signer = vw.VwSigner(key);
    final envelope = Uint8List(16);
    vw.vwEnvelopeEncodeV1(envelope, 7, 64);
    final payload = Uint8List.fromList(List.generate(64, (i) => (i * 13) & 0xff));
    final tag = signer.sign(envelope, payload);
    final rec = Uint8List(16 + 64 + 32);
    vw.vwRecordEncode(envelope, payload, tag, rec);

    var rejected = 0;
    for (var i = 0; i < rec.length; i++) {
      final tampered = Uint8List.fromList(rec);
      tampered[i] ^= 0x80;
      if (vw.vwRecordDecodeVerify(key, tampered).code != vw.VwResult.ok) {
        rejected++;
      }
    }
    expect(rejected, rec.length, reason: 'all ${rec.length} byte flips rejected');
  });

  test('V5: wrong key / short / bad magic / hostile geometry', () {
    final key = vw.vwDeriveKey(Uint8List.fromList('v5-key'.codeUnits));
    final wrong = vw.vwDeriveKey(Uint8List.fromList('wrong'.codeUnits));
    final signer = vw.VwSigner(key);
    final envelope = Uint8List(16);
    vw.vwEnvelopeEncodeV1(envelope, 1, 24);
    final payload = Uint8List.fromList(List.filled(24, 0xAB));
    final tag = signer.sign(envelope, payload);
    final rec = Uint8List(16 + 24 + 32);
    vw.vwRecordEncode(envelope, payload, tag, rec);

    expect(vw.vwRecordDecodeVerify(wrong, rec).code, vw.VwResult.errTag,
        reason: 'wrong key');
    expect(vw.vwRecordDecodeVerify(key, Uint8List(40)).code, vw.VwResult.errShort,
        reason: 'short record');
    final badMagic = Uint8List.fromList(rec);
    badMagic[0] = 0x58; // 'X'
    expect(vw.vwRecordDecodeVerify(key, badMagic).code, vw.VwResult.errBadMagic,
        reason: 'bad magic');
    final hostile = Uint8List.fromList(rec);
    hostile[15] = 0x80; // payload_len high bit
    expect(vw.vwRecordDecodeVerify(key, hostile).code, vw.VwResult.errShort,
        reason: 'hostile plen lands in errShort (unsigned decode)');
  });

  test('V8: pre-keyed verifier == one-shot across a stream', () {
    final key = vw.vwDeriveKey(Uint8List.fromList('v8-stream'.codeUnits));
    final verifier = vw.VwVerifier(key);
    final signer = vw.VwSigner(key);
    final envelope = Uint8List(16);
    final payload = Uint8List(64);
    for (var i = 0; i < 1000; i++) {
      vw.vwEnvelopeEncodeV1(envelope, i, 64);
      for (var j = 0; j < 64; j++) {
        payload[j] = (i + j) & 0xff;
      }
      final tag = signer.sign(envelope, payload);
      expect(verifier.verify(envelope, payload, tag), vw.VwResult.ok,
          reason: 'pre-keyed OK ($i)');
      expect(vw.vwVerify(key, envelope, payload, tag), vw.VwResult.ok,
          reason: 'one-shot OK ($i)');
      tag[0] ^= 1;
      expect(verifier.verify(envelope, payload, tag), vw.VwResult.errTag,
          reason: 'tamper red ($i)');
    }
    vw.vwEnvelopeEncodeV1(envelope, 0, 64);
    for (var j = 0; j < 64; j++) {
      payload[j] = j;
    }
    final tag = signer.sign(envelope, payload);
    expect(verifier.verify(envelope, payload, tag), vw.VwResult.ok,
        reason: 'state stable after stream');
  });

  group('V9: batch decode-verify', () {
    final key = vw.vwDeriveKey(Uint8List.fromList('v9-batch'.codeUnits));
    const n = 500, plen = 48;
    const recLen = vw.vwEnvelopeLen + plen + vw.vwTagLen;

    Uint8List buildStream() {
      final signer = vw.VwSigner(key);
      final stream = Uint8List(n * recLen);
      final envelope = Uint8List(16);
      final payload = Uint8List.fromList(List.generate(plen, (i) => i));
      final slice = Uint8List(recLen);
      for (var i = 0; i < n; i++) {
        vw.vwEnvelopeEncodeV1(envelope, i * 3 + 1, plen);
        final tag = signer.sign(envelope, payload);
        final written = vw.vwRecordEncode(envelope, payload, tag, slice);
        stream.setRange(i * recLen, i * recLen + written, slice);
      }
      return stream;
    }

    test('all OK, views, exact bytesConsumed', () {
      final stream = buildStream();
      final r = vw.vwBatchDecodeVerify(key, stream);
      expect(r.code, vw.VwResult.ok, reason: 'all OK');
      expect(r.verified, n);
      expect(r.bytesConsumed, n * recLen);
      expect(r.records.length, n);
      for (var i = 0; i < n; i++) {
        expect(r.records[i].offset, i * recLen, reason: 'view offset $i');
        expect(r.records[i].seq, i * 3 + 1, reason: 'view seq $i');
      }
    });

    test('tamper stop + resync offset', () {
      final stream = buildStream();
      stream[137 * recLen + 20] ^= 0x40;
      final r = vw.vwBatchDecodeVerify(key, stream);
      expect(r.code, vw.VwResult.errTag);
      expect(r.verified, 137);
      expect(r.bytesConsumed, 137 * recLen);
    });

    test('truncation policy + capped views', () {
      final fresh = buildStream();
      final tail = vw.vwBatchDecodeVerify(
          key, Uint8List.sublistView(fresh, 0, (n - 1) * recLen + 17));
      expect(tail.code, vw.VwResult.ok, reason: 'short tail ignored');
      expect(tail.verified, n - 1);
      final midRec = vw.vwBatchDecodeVerify(
          key, Uint8List.sublistView(fresh, 0, n * recLen - 30));
      expect(midRec.code, vw.VwResult.errShort, reason: 'mid-record is errShort');
      expect(midRec.verified, n - 1);

      final capped = vw.vwBatchDecodeVerify(key, fresh, 10);
      expect(capped.verified, n, reason: 'capped views still verify all');
      expect(capped.records.length, 10);
    });
  });
}
