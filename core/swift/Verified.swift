// Verified.swift — RFC 0005: VerifiedWeft authenticated frames, Swift driver layer
//
// WHY EXISTS: RFC 0005 shipped VerifiedWeft to the three canonical kernels
// (core/c, core/rust, core/ts — PR #6) and Series 6 is bringing it to every
// VM port (Kotlin landed with the platform-Mac story; this file is the
// Swift leg; Dart follows). iOS/macOS apps consuming authenticated records
// (WebSocket bridges, cross-process IPC, .weftrec replay on device) had to
// detour through the C verifier. This module gives Swift the SAME wire
// format, key schedule, and result codes (docs/PORTS.md §7; shared fixture:
// fixtures/xlang-verifiedweft/hmac-vectors.json).
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
// weft_vw_result_t (C), VW_* (TS), VwResult (Kotlin).
//
// THE PLATFORM IS THE ACCELERATOR (Series 6 HW story, per-port honesty):
// CryptoKit's HMAC<SHA256> delegates to CoreCrypto, which compiles SHA-256
// down to the ARMv8 Crypto Extensions (FEAT_SHA256) on every Apple-silicon
// device and SHA-NI on Intel Macs. Swift has no intrinsics surface for
// these instructions; the platform library is the honest road — the same
// story as Kotlin's javax.crypto.Mac and TS's WebCrypto. DECLARED
// DIVERGENCE (per the repo's honesty culture, PORTS.md §7): CryptoKit
// exposes no streaming HMAC (no update/final), so the "pre-keyed"
// VwSigner/VwVerifier hold the SymmetricKey once and pay CoreCrypto's
// per-call pad derivation — CoreCrypto's hardware SHA makes that cheap.
// The API contract mirrors the other ports; the optimization boundary is
// documented here instead of being papered over.
//
// GUARANTEE BOUNDARY (Law 4): integrity + authenticity of frame contents.
// A record that fails verification is DROPPED and counted, never consumed.

import Foundation
import CryptoKit

/// Result codes — numeric parity with weft_vw_result_t (C) / VW_* (TS).
public enum VwResultCode {
    public static let ok: Int32 = 0
    public static let errShort: Int32 = 1
    public static let errBadMagic: Int32 = 2
    public static let errTag: Int32 = 3
}

/// Derived auth-key length (bytes).
public let VW_KEY_LEN = 32

/// HMAC-SHA256 tag length (bytes).
public let VW_TAG_LEN = 32

/// Signed envelope prefix length (bytes).
public let VW_ENVELOPE_LEN = 16

private let vwDomain = Data("Weft-VerifiedWeft-v1:key".utf8)

/// One-time auth-key derivation with domain separation.
public func vwDeriveKey(secret: Data) -> Data {
    let key = SymmetricKey(data: secret)
    return Data(HMAC<SHA256>.authenticationCode(for: vwDomain, using: key))
}

/// One-shot HMAC-SHA256 (setup + conformance use).
public func vwHmacSha256(key: Data, data: Data) -> Data {
    Data(HMAC<SHA256>.authenticationCode(for: data, using: SymmetricKey(data: key)))
}

/// Constant-time equality — manual double-walk accumulate (CryptoKit has no
/// public constant-time compare for raw bytes). Time depends on length only.
public func vwCtEq(_ a: Data, _ b: Data) -> Bool {
    guard a.count == b.count else { return false }
    var diff: UInt8 = 0
    for i in 0..<a.count {
        diff |= a[a.startIndex + i] ^ b[b.startIndex + i]
    }
    return diff == 0
}

/// Reusable pre-keyed signer — the Swift mirror of weft_vw_signer_t (C) /
/// VwSigner (Kotlin). Holds the SymmetricKey; per-call pad derivation is
/// CoreCrypto's (declared divergence, see header).
public struct VwSigner {
    private let key: SymmetricKey

    public init(authKey: Data) {
        self.key = SymmetricKey(data: authKey)
    }

    /// tag = HMAC(auth_key, envelope[0..16] || payload).
    public func sign(envelope: Data, payload: Data) -> Data {
        var msg = Data(capacity: VW_ENVELOPE_LEN + payload.count)
        msg.append(envelope.prefix(VW_ENVELOPE_LEN))
        msg.append(payload)
        return Data(HMAC<SHA256>.authenticationCode(for: msg, using: key))
    }
}

/// Reusable pre-keyed verifier — accept/reject identical to the one-shot
/// vwVerify; the same declared-divergence note as VwSigner applies.
public struct VwVerifier {
    private let key: SymmetricKey

    public init(authKey: Data) {
        self.key = SymmetricKey(data: authKey)
    }

    /// Returns VwResultCode.ok only on a byte-exact constant-time match.
    public func verify(envelope: Data, payload: Data, tag: Data) -> Int32 {
        var msg = Data(capacity: VW_ENVELOPE_LEN + payload.count)
        msg.append(envelope.prefix(VW_ENVELOPE_LEN))
        msg.append(payload)
        let expect = Data(HMAC<SHA256>.authenticationCode(for: msg, using: key))
        return vwCtEq(expect, tag) ? VwResultCode.ok : VwResultCode.errTag
    }
}

/// Verify one frame with a one-shot key schedule (API compat mirror).
public func vwVerify(authKey: Data, envelope: Data, payload: Data, tag: Data) -> Int32 {
    let expect = vwHmacSha256(key: authKey, data: {
        var msg = Data(capacity: VW_ENVELOPE_LEN + payload.count)
        msg.append(envelope.prefix(VW_ENVELOPE_LEN))
        msg.append(payload)
        return msg
    }())
    return vwCtEq(expect, tag) ? VwResultCode.ok : VwResultCode.errTag
}

/// Encode envelope v1 (magic "WEFT", version 1, header_size 16, seq,
/// payload_len — all little-endian), mirroring weftEnvelopeEncodeV1 (TS).
public func vwEnvelopeEncodeV1(dst: inout Data, seq: UInt32, payloadLen: UInt32) {
    dst = Data(count: VW_ENVELOPE_LEN)
    dst[0] = 0x57; dst[1] = 0x45; dst[2] = 0x46; dst[3] = 0x54 // "WEFT"
    dst[4] = 1; dst[5] = 0            // version u16 LE
    dst[6] = 16; dst[7] = 0           // header_size u16 LE
    dst[8] = UInt8(seq & 0xff)
    dst[9] = UInt8((seq >> 8) & 0xff)
    dst[10] = UInt8((seq >> 16) & 0xff)
    dst[11] = UInt8((seq >> 24) & 0xff)
    dst[12] = UInt8(payloadLen & 0xff)
    dst[13] = UInt8((payloadLen >> 8) & 0xff)
    dst[14] = UInt8((payloadLen >> 16) & 0xff)
    dst[15] = UInt8((payloadLen >> 24) & 0xff)
}

/// Encode a full auth record into dst; returns bytes written (0 if too small).
public func vwRecordEncode(envelope: Data, payload: Data, tag: Data, dst: inout Data) -> Int {
    let total = VW_ENVELOPE_LEN + payload.count + VW_TAG_LEN
    guard dst.count >= total else { return 0 }
    var off = dst.startIndex
    dst.replaceSubrange(off..<off + VW_ENVELOPE_LEN, with: envelope.prefix(VW_ENVELOPE_LEN))
    off += VW_ENVELOPE_LEN
    dst.replaceSubrange(off..<off + payload.count, with: payload)
    off += payload.count
    dst.replaceSubrange(off..<off + VW_TAG_LEN, with: tag)
    return total
}

/// Zero-copy-ish view of one verified record: offsets into the source Data.
public struct VwRecordView {
    /// Record start offset in the source buffer.
    public let offset: Int
    /// Payload start offset in the source buffer.
    public let payloadOffset: Int
    /// Payload length in bytes.
    public let payloadLen: Int
    /// Envelope seq field, decoded little-endian.
    public let seq: UInt32
}

/// Batch decode-verify result — mirrors the C/TS/Kotlin batch APIs.
public struct VwBatchResult {
    /// VwResultCode.ok, or the code of the FIRST bad record.
    public let code: Int32
    /// Count of verified records (the good prefix).
    public let verified: Int
    /// End offset of the last VERIFIED record (resync point).
    public let bytesConsumed: Int
    /// Zero-copy views of verified records (bounded by maxViews).
    public let records: [VwRecordView]
}

/// Decode + verify ONE record from wire bytes.
public func vwRecordDecodeVerify(authKey: Data, src: Data) -> VwBatchResult {
    if src.count < VW_ENVELOPE_LEN + VW_TAG_LEN {
        return VwBatchResult(code: VwResultCode.errShort, verified: 0, bytesConsumed: 0, records: [])
    }
    guard src[src.startIndex] == 0x57, src[src.startIndex + 1] == 0x45,
          src[src.startIndex + 2] == 0x46, src[src.startIndex + 3] == 0x54 else {
        return VwBatchResult(code: VwResultCode.errBadMagic, verified: 0, bytesConsumed: 0, records: [])
    }
    let headerSize = Int(src[src.startIndex + 6]) | (Int(src[src.startIndex + 7]) << 8)
    // Unsigned decode (C: size_t / TS: >>> 0) — hostile high-bit payload_len
    // lands in errShort, never wraps negative.
    let plen = UInt32(src[src.startIndex + 12]) | (UInt32(src[src.startIndex + 13]) << 8) |
        (UInt32(src[src.startIndex + 14]) << 16) | (UInt32(src[src.startIndex + 15]) << 24)
    if headerSize < VW_ENVELOPE_LEN {
        return VwBatchResult(code: VwResultCode.errBadMagic, verified: 0, bytesConsumed: 0, records: [])
    }
    let body = UInt64(headerSize) + UInt64(plen)
    if body > UInt64(src.count) - UInt64(VW_TAG_LEN) {
        return VwBatchResult(code: VwResultCode.errShort, verified: 0, bytesConsumed: 0, records: [])
    }
    let bodyInt = Int(body)
    let envelope = src.prefix(VW_ENVELOPE_LEN)
    let payload = src.subdata(in: (src.startIndex + headerSize)..<(src.startIndex + bodyInt))
    let tag = src.subdata(in: (src.startIndex + bodyInt)..<(src.startIndex + bodyInt + VW_TAG_LEN))
    let code = vwVerify(authKey: authKey, envelope: envelope, payload: payload, tag: tag)
    if code != VwResultCode.ok {
        return VwBatchResult(code: code, verified: 0, bytesConsumed: 0, records: [])
    }
    let seq = UInt32(src[src.startIndex + 8]) | (UInt32(src[src.startIndex + 9]) << 8) |
        (UInt32(src[src.startIndex + 10]) << 16) | (UInt32(src[src.startIndex + 11]) << 24)
    return VwBatchResult(
        code: VwResultCode.ok, verified: 1, bytesConsumed: bodyInt + VW_TAG_LEN,
        records: [VwRecordView(offset: 0, payloadOffset: headerSize, payloadLen: Int(plen), seq: seq)]
    )
}

/// Walk a buffer of concatenated auth records, verifying each in order
/// (Series 6 stream-consumer path; Law 4 drop-and-count preserved).
/// Same stop/truncation contract as the C/TS/Kotlin ports: first bad record
/// stops the walk with its code + good-prefix count + resync offset; a tail
/// shorter than a minimal record is ignored; a started-but-unfitting record
/// is errShort. maxViews bounds the returned views only.
public func vwBatchDecodeVerify(authKey: Data, src: Data, maxViews: Int = Int.max) -> VwBatchResult {
    let verifier = VwVerifier(authKey: authKey)
    var records: [VwRecordView] = []
    var verified = 0
    var off = 0
    let n = src.count
    let base = src.startIndex

    while off + VW_ENVELOPE_LEN + VW_TAG_LEN <= n {
        guard src[base + off] == 0x57, src[base + off + 1] == 0x45,
              src[base + off + 2] == 0x46, src[base + off + 3] == 0x54 else {
            return VwBatchResult(code: VwResultCode.errBadMagic, verified: verified,
                                 bytesConsumed: off, records: records)
        }
        let headerSize = Int(src[base + off + 6]) | (Int(src[base + off + 7]) << 8)
        let plen = UInt32(src[base + off + 12]) | (UInt32(src[base + off + 13]) << 8) |
            (UInt32(src[base + off + 14]) << 16) | (UInt32(src[base + off + 15]) << 24)
        if headerSize < VW_ENVELOPE_LEN {
            return VwBatchResult(code: VwResultCode.errBadMagic, verified: verified,
                                 bytesConsumed: off, records: records)
        }
        let body = UInt64(headerSize) + UInt64(plen)
        if body > UInt64(n) - UInt64(off) - UInt64(VW_TAG_LEN) {
            return VwBatchResult(code: VwResultCode.errShort, verified: verified,
                                 bytesConsumed: off, records: records)
        }
        let bodyInt = Int(body)

        let envelope = src.subdata(in: (base + off)..<(base + off + VW_ENVELOPE_LEN))
        let payload = src.subdata(in: (base + off + headerSize)..<(base + off + bodyInt))
        let tag = src.subdata(in: (base + off + bodyInt)..<(base + off + bodyInt + VW_TAG_LEN))
        if verifier.verify(envelope: envelope, payload: payload, tag: tag) != VwResultCode.ok {
            return VwBatchResult(code: VwResultCode.errTag, verified: verified,
                                 bytesConsumed: off, records: records)
        }

        if verified < maxViews {
            let seq = UInt32(src[base + off + 8]) | (UInt32(src[base + off + 9]) << 8) |
                (UInt32(src[base + off + 10]) << 16) | (UInt32(src[base + off + 11]) << 24)
            records.append(VwRecordView(offset: off, payloadOffset: off + headerSize,
                                        payloadLen: Int(plen), seq: seq))
        }
        verified += 1
        off += bodyInt + VW_TAG_LEN
    }
    return VwBatchResult(code: VwResultCode.ok, verified: verified,
                         bytesConsumed: off, records: records)
}
