// verified.dart — RFC 0005: VerifiedWeft authenticated frames, Dart driver layer
//
// WHY EXISTS: RFC 0005 shipped VerifiedWeft to the three canonical kernels
// (core/c, core/rust, core/ts — PR #6); Series 6 brings it to every VM port
// (Kotlin via the platform Mac, Swift via CryptoKit, this file). Flutter
// apps and Dart engines consuming authenticated records (a .weftrec bridge,
// a WebSocket feed) had no in-language verifier. This module gives Dart the
// SAME wire format, key schedule, and result codes (docs/PORTS.md §7;
// shared fixture: fixtures/xlang-verifiedweft/hmac-vectors.json).
//
// WIRE FORMAT (identical bytes in all ports):
//   [0..16)                envelope v1 (magic "WEFT", version 1,
//                          header_size 16, seq u32 LE, payload_len u32 LE)
//   [16..16+payload_len)   payload
//   [16+payload_len..+32)  HMAC-SHA256 tag
// KEY SCHEDULE:
//   auth_key = HMAC-SHA256(secret, "Weft-VerifiedWeft-v1:key")
//   tag      = HMAC-SHA256(auth_key, envelope[0..16] || payload)
// RESULT CODES: 0 OK / 1 short / 2 bad-magic / 3 tag — numeric parity with
// weft_vw_result_t (C), VW_* (TS), VwResult (Kotlin), VwResultCode (Swift).
//
// THE HONESTY WALL OF THIS PORT (per-port culture, PORTS.md §7): SHA-256 and
// HMAC are implemented here in PURE DART — dart:io exposes no HMAC, and
// package:crypto would itself be pure Dart, so the zero-dependency rule of
// this port (same as its kernel, core/dart/weft.dart) costs nothing in
// honesty and keeps pubspec frozen. CONSEQUENCE, DECLARED: no hardware SHA
// (no SHA-NI / ARMv8 CE reach from pure Dart) — this port is the SCALAR
// reference of the six, byte-identical on the wire, slower on the CPU.
// Callers that need HW rates attach the C verifier via dart:ffi
// (packages/flutter_weft/src/weft_ffi.dart is the existing road).
//
// GUARANTEE BOUNDARY (Law 4): integrity + authenticity of frame contents.
// A record that fails verification is DROPPED and counted, never consumed.

import 'dart:typed_data';

/// Result codes — numeric parity with weft_vw_result_t (C) / VW_* (TS).
class VwResult {
  static const int ok = 0;
  static const int errShort = 1;
  static const int errBadMagic = 2;
  static const int errTag = 3;
}

/// Derived auth-key length (bytes).
const int vwKeyLen = 32;

/// HMAC-SHA256 tag length (bytes).
const int vwTagLen = 32;

/// Signed envelope prefix length (bytes).
const int vwEnvelopeLen = 16;

const int _blockLen = 64;
const int _digestLen = 32;

const List<int> _k = [
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

const List<int> _h0 = [
  0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
];

int _rotr32(int x, int n) => ((x >> n) | (x << (32 - n))) & 0xffffffff;

/// Streaming SHA-256 (FIPS 180-4), pure Dart. Mirrors the structure of the
/// TS port's Sha256 class (core/ts/verified.ts) — same message schedule,
/// same finalize discipline; digests byte-identical by the shared fixture.
class VwSha256 {
  final Uint8List _block = Uint8List(_blockLen);
  final Uint32List _h = Uint32List(8);
  int _fill = 0;
  int _totalLen = 0;

  VwSha256() {
    _h.setAll(0, _h0);
  }

  void reset() {
    _h.setAll(0, _h0);
    _fill = 0;
    _totalLen = 0;
  }

  void update(Uint8List data, [int start = 0, int? end]) {
    end ??= data.length;
    var len = end - start;
    _totalLen += len;
    if (_fill > 0) {
      final take = (_blockLen - _fill) < len ? (_blockLen - _fill) : len;
      _block.setRange(_fill, _fill + take, data, start);
      _fill += take;
      start += take;
      len -= take;
      if (_fill == _blockLen) {
        _compress(_block);
        _fill = 0;
      }
    }
    while (len >= _blockLen) {
      _compress(Uint8List.sublistView(data, start, start + _blockLen));
      start += _blockLen;
      len -= _blockLen;
    }
    if (len > 0) {
      _block.setRange(0, len, data, start);
      _fill = len;
    }
  }

  /// Finalize into out[0..32); state resets for reuse.
  void finalize(Uint8List out) {
    final bitLen = _totalLen * 8;
    final pad = 0x80;
    update(Uint8List.fromList([pad]));
    final zero = Uint8List(1);
    zero[0] = 0;
    while (_fill != 56) {
      update(zero);
    }
    // Length field, big-endian, without counting it into _totalLen.
    _block[56] = (bitLen >> 56) & 0xff;
    _block[57] = (bitLen >> 48) & 0xff;
    _block[58] = (bitLen >> 40) & 0xff;
    _block[59] = (bitLen >> 32) & 0xff;
    _block[60] = (bitLen >> 24) & 0xff;
    _block[61] = (bitLen >> 16) & 0xff;
    _block[62] = (bitLen >> 8) & 0xff;
    _block[63] = bitLen & 0xff;
    _compress(_block);
    _fill = 0;
    for (var i = 0; i < 8; i++) {
      out[i * 4] = (_h[i] >> 24) & 0xff;
      out[i * 4 + 1] = (_h[i] >> 16) & 0xff;
      out[i * 4 + 2] = (_h[i] >> 8) & 0xff;
      out[i * 4 + 3] = _h[i] & 0xff;
    }
    reset();
  }

  /// Snapshot-copy this state into dst (preallocated) — the HMAC template.
  void copyInto(VwSha256 dst) {
    dst._h.setAll(0, _h);
    dst._totalLen = _totalLen;
    dst._block.setAll(0, _block);
    dst._fill = _fill;
  }

  void _compress(Uint8List block) {
    final w = Uint32List(64);
    for (var i = 0; i < 16; i++) {
      w[i] = (block[i * 4] << 24) |
          (block[i * 4 + 1] << 16) |
          (block[i * 4 + 2] << 8) |
          block[i * 4 + 3];
    }
    for (var i = 16; i < 64; i++) {
      final s0 = _rotr32(w[i - 15], 7) ^ _rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
      final s1 = _rotr32(w[i - 2], 17) ^ _rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) & 0xffffffff;
    }
    var a = _h[0], b = _h[1], c = _h[2], d = _h[3];
    var e = _h[4], f = _h[5], g = _h[6], h = _h[7];
    for (var i = 0; i < 64; i++) {
      final s1 = _rotr32(e, 6) ^ _rotr32(e, 11) ^ _rotr32(e, 25);
      final ch = (e & f) ^ ((~e & 0xffffffff) & g);
      final t1 = (h + s1 + ch + _k[i] + w[i]) & 0xffffffff;
      final s0 = _rotr32(a, 2) ^ _rotr32(a, 13) ^ _rotr32(a, 22);
      final maj = (a & b) ^ (a & c) ^ (b & c);
      final t2 = (s0 + maj) & 0xffffffff;
      h = g;
      g = f;
      f = e;
      e = (d + t1) & 0xffffffff;
      d = c;
      c = b;
      b = a;
      a = (t1 + t2) & 0xffffffff;
    }
    _h[0] = (_h[0] + a) & 0xffffffff;
    _h[1] = (_h[1] + b) & 0xffffffff;
    _h[2] = (_h[2] + c) & 0xffffffff;
    _h[3] = (_h[3] + d) & 0xffffffff;
    _h[4] = (_h[4] + e) & 0xffffffff;
    _h[5] = (_h[5] + f) & 0xffffffff;
    _h[6] = (_h[6] + g) & 0xffffffff;
    _h[7] = (_h[7] + h) & 0xffffffff;
  }
}

/// Constant-time equality — manual double-walk accumulate (Dart exposes no
/// platform constant-time compare). Time depends on length only.
bool vwCtEq(Uint8List a, Uint8List b) {
  if (a.length != b.length) return false;
  var diff = 0;
  for (var i = 0; i < a.length; i++) {
    diff |= a[i] ^ b[i];
  }
  return diff == 0;
}

const List<int> _vwDomainBytes = [
  87, 101, 102, 116, 45, 86, 101, 114, 105, 102, 105, 101, 100, 87, 101, 102,
  116, 45, 118, 49, 58, 107, 101, 121,
]; // "Weft-VerifiedWeft-v1:key"

/// One-shot HMAC-SHA256 (pure Dart; setup + conformance use).
Uint8List vwHmacSha256(Uint8List key, Uint8List data) {
  final signer = VwSigner._raw(key);
  signer._key.inner.update(data);
  final out = Uint8List(vwTagLen);
  signer._finish(out);
  return out;
}

/// One-time auth-key derivation with domain separation.
Uint8List vwDeriveKey(Uint8List secret) =>
    vwHmacSha256(secret, Uint8List.fromList(_vwDomainBytes));

/// Reusable pre-keyed HMAC state: ipad pre-fed into the inner hash, opad
/// stored for finalize — the Dart mirror of hmac_sha256_key_t (C) and
/// VerifiedWeftSigner (TS). One key schedule, N messages.
class _VwHmacKey {
  late VwSha256 inner;
  late VwSha256 innerTemplate;
  final Uint8List opad = Uint8List(_blockLen);
  final Uint8List innerDigest = Uint8List(_digestLen);

  _VwHmacKey(Uint8List key) {
    final keyBlock = Uint8List(_blockLen);
    if (key.length > _blockLen) {
      final hashed = Uint8List(_digestLen);
      final kh = VwSha256();
      kh.update(key);
      kh.finalize(hashed);
      keyBlock.setRange(0, _digestLen, hashed);
    } else {
      keyBlock.setRange(0, key.length, key);
    }
    final ipad = Uint8List(_blockLen);
    for (var i = 0; i < _blockLen; i++) {
      ipad[i] = keyBlock[i] ^ 0x36;
      opad[i] = keyBlock[i] ^ 0x5c;
    }
    inner = VwSha256();
    inner.update(ipad);
    innerTemplate = VwSha256();
    inner.copyInto(innerTemplate);
  }
}

/// Reusable pre-keyed signer — the Dart mirror of weft_vw_signer_t (C) /
/// VwSigner (Kotlin). sign() restores the ipad template after every tag.
class VwSigner {
  final _VwHmacKey _key;

  VwSigner._raw(Uint8List key) : _key = _VwHmacKey(key);

  VwSigner(Uint8List authKey) : _key = _VwHmacKey(authKey);

  void _feedEnvelope(Uint8List envelope) =>
      _key.inner.update(envelope, 0, vwEnvelopeLen);

  void _finish(Uint8List outTag) {
    _key.inner.finalize(_key.innerDigest);
    final outer = VwSha256();
    outer.update(_key.opad);
    outer.update(_key.innerDigest);
    outer.finalize(outTag);
    // Restore the pre-keyed ipad state for the next message.
    _key.innerTemplate.copyInto(_key.inner);
  }

  /// tag = HMAC(auth_key, envelope[0..16] || payload).
  Uint8List sign(Uint8List envelope, Uint8List payload) {
    _feedEnvelope(envelope);
    _key.inner.update(payload);
    final out = Uint8List(vwTagLen);
    _finish(out);
    return out;
  }
}

/// Reusable pre-keyed verifier — accept/reject identical to the one-shot
/// vwVerify; the key schedule is amortized (Series 6).
class VwVerifier {
  final VwSigner _signer;

  VwVerifier(Uint8List authKey) : _signer = VwSigner(authKey);

  /// Returns VwResult.ok only on a byte-exact constant-time tag match.
  int verify(Uint8List envelope, Uint8List payload, Uint8List tag) {
    final expect = _signer.sign(envelope, payload);
    return vwCtEq(expect, tag) ? VwResult.ok : VwResult.errTag;
  }
}

/// Verify one frame with a one-shot key schedule (API compat mirror).
int vwVerify(Uint8List authKey, Uint8List envelope, Uint8List payload, Uint8List tag) =>
    VwVerifier(authKey).verify(envelope, payload, tag);

/// Encode envelope v1 (magic "WEFT", version 1, header_size 16, seq,
/// payload_len — all little-endian), mirroring weftEnvelopeEncodeV1 (TS).
void vwEnvelopeEncodeV1(Uint8List dst, int seq, int payloadLen) {
  dst[0] = 0x57;
  dst[1] = 0x45;
  dst[2] = 0x46;
  dst[3] = 0x54; // "WEFT"
  dst[4] = 1;
  dst[5] = 0; // version u16 LE
  dst[6] = 16;
  dst[7] = 0; // header_size u16 LE
  dst[8] = seq & 0xff;
  dst[9] = (seq >> 8) & 0xff;
  dst[10] = (seq >> 16) & 0xff;
  dst[11] = (seq >> 24) & 0xff;
  dst[12] = payloadLen & 0xff;
  dst[13] = (payloadLen >> 8) & 0xff;
  dst[14] = (payloadLen >> 16) & 0xff;
  dst[15] = (payloadLen >> 24) & 0xff;
}

/// Encode a full auth record into dst; returns bytes written (0 if too small).
int vwRecordEncode(Uint8List envelope, Uint8List payload, Uint8List tag, Uint8List dst) {
  final total = vwEnvelopeLen + payload.length + vwTagLen;
  if (dst.length < total) return 0;
  dst.setRange(0, vwEnvelopeLen, envelope);
  dst.setRange(vwEnvelopeLen, vwEnvelopeLen + payload.length, payload);
  dst.setRange(vwEnvelopeLen + payload.length, total, tag);
  return total;
}

/// Zero-copy view of one verified record: offsets into the source buffer.
class VwRecordView {
  /// Record start offset in the source buffer.
  final int offset;

  /// Payload start offset in the source buffer.
  final int payloadOffset;

  /// Payload length in bytes.
  final int payloadLen;

  /// Envelope seq field, decoded little-endian.
  final int seq;

  VwRecordView(this.offset, this.payloadOffset, this.payloadLen, this.seq);
}

/// Batch decode-verify result — mirrors the C/TS/Kotlin/Swift batch APIs.
class VwBatchResult {
  /// VwResult.ok, or the code of the FIRST bad record.
  final int code;

  /// Count of verified records (the good prefix).
  final int verified;

  /// End offset of the last VERIFIED record (resync point).
  final int bytesConsumed;

  /// Zero-copy views of verified records (bounded by maxViews).
  final List<VwRecordView> records;

  VwBatchResult(this.code, this.verified, this.bytesConsumed, this.records);
}

bool _magicOk(Uint8List src, int off) =>
    src[off] == 0x57 &&
    src[off + 1] == 0x45 &&
    src[off + 2] == 0x46 &&
    src[off + 3] == 0x54;

int _u16le(Uint8List src, int off) => src[off] | (src[off + 1] << 8);

int _u32le(Uint8List src, int off) =>
    (src[off] | (src[off + 1] << 8) | (src[off + 2] << 16) | (src[off + 3] << 24)) &
        0xffffffff;

/// Decode + verify ONE record from wire bytes.
VwBatchResult vwRecordDecodeVerify(Uint8List authKey, Uint8List src) {
  if (src.length < vwEnvelopeLen + vwTagLen) {
    return VwBatchResult(VwResult.errShort, 0, 0, const []);
  }
  if (!_magicOk(src, 0)) {
    return VwBatchResult(VwResult.errBadMagic, 0, 0, const []);
  }
  final headerSize = _u16le(src, 6);
  // Unsigned decode (C: size_t / TS: >>> 0) — hostile high-bit payload_len
  // lands in errShort, never wraps negative (pinned in every port).
  final plen = _u32le(src, 12);
  if (headerSize < vwEnvelopeLen) {
    return VwBatchResult(VwResult.errBadMagic, 0, 0, const []);
  }
  final body = headerSize + plen; // <= 0xffffffff + 64 — Dart int is 64-bit
  if (body > src.length - vwTagLen) {
    return VwBatchResult(VwResult.errShort, 0, 0, const []);
  }
  final tag = Uint8List.sublistView(src, body, body + vwTagLen);
  final verifier = VwVerifier(authKey);
  final code = verifier.verify(
      Uint8List.sublistView(src, 0, vwEnvelopeLen),
      Uint8List.sublistView(src, headerSize, body),
      tag);
  if (code != VwResult.ok) {
    return VwBatchResult(code, 0, 0, const []);
  }
  return VwBatchResult(VwResult.ok, 1, body + vwTagLen,
      [VwRecordView(0, headerSize, plen, _u32le(src, 8))]);
}

/// Walk a buffer of concatenated auth records, verifying each in order
/// (Series 6 stream-consumer path; Law 4 drop-and-count preserved).
/// Same stop/truncation contract as every other port: first bad record
/// stops the walk; a tail shorter than a minimal record is ignored; a
/// started-but-unfitting record is errShort. maxViews bounds views only.
VwBatchResult vwBatchDecodeVerify(Uint8List authKey, Uint8List src,
    [int maxViews = 0x7fffffffffffffff]) {
  final verifier = VwVerifier(authKey);
  final records = <VwRecordView>[];
  var verified = 0;
  var off = 0;

  while (off + vwEnvelopeLen + vwTagLen <= src.length) {
    if (!_magicOk(src, off)) {
      return VwBatchResult(VwResult.errBadMagic, verified, off, records);
    }
    final headerSize = _u16le(src, off + 6);
    final plen = _u32le(src, off + 12);
    if (headerSize < vwEnvelopeLen) {
      return VwBatchResult(VwResult.errBadMagic, verified, off, records);
    }
    final body = headerSize + plen;
    if (body > src.length - off - vwTagLen) {
      return VwBatchResult(VwResult.errShort, verified, off, records);
    }

    // Feed ranges of src directly — zero-copy through sublistView.
    final envelope = Uint8List.sublistView(src, off, off + vwEnvelopeLen);
    final payload = Uint8List.sublistView(src, off + headerSize, off + body);
    final tag = Uint8List.sublistView(src, off + body, off + body + vwTagLen);
    if (verifier.verify(envelope, payload, tag) != VwResult.ok) {
      return VwBatchResult(VwResult.errTag, verified, off, records);
    }

    if (verified < maxViews) {
      records.add(VwRecordView(off, off + headerSize, plen, _u32le(src, off + 8)));
    }
    verified++;
    off += body + vwTagLen;
  }
  return VwBatchResult(VwResult.ok, verified, off, records);
}
