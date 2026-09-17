// VerifiedTests.swift — RFC-0005 VerifiedWeft conformance suite, Swift.
//
// The V-series counterpart of core/c/verified_test.c, packages/core/test/
// verified.test.ts, core/rust/src/verified.rs, and FanoutTest.kt's verified
// battery: shared fixture vectors, derivation, roundtrip, exhaustive tamper,
// rejections, pre-keyed verifier semantics, and the Series-6 batch stream
// API. Tags are bit-identical to every other port by construction (same key
// schedule, same wire format; the fixture below is the shared ground truth).
//
// Environment tag: XCTest on macOS/ARM (apple-packages CI); NOT runnable in
// the x86_64 Linux sandbox (no Swift toolchain there) — declared, per the
// repo's per-port honesty culture (see PORTS.md §7).
//
// Fixture: byte-identical to fixtures/xlang-verifiedweft/hmac-vectors.json
// (RFC 4231 TC1-4,6,7 + Weft boundary cases; node:crypto cross-checked at
// generation time).

import XCTest
@testable import WeftCore

final class VerifiedTests: XCTestCase {

    struct Vector {
        let name: String
        let key: String
        let data: String
        let tag: String
    }

    let vectors: [Vector] = [
        Vector(name: "rfc4231-tc1",
               key: "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
               data: "4869205468657265",
               tag: "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
        Vector(name: "rfc4231-tc2",
               key: "4a656665",
               data: "7768617420646f2079612077616e7420666f72206e6f7468696e673f",
               tag: "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
        Vector(name: "rfc4231-tc3",
               key: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
               data: String(repeating: "dd", count: 50),
               tag: "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"),
        Vector(name: "rfc4231-tc4",
               key: "0102030405060708090a0b0c0d0e0f10111213141516171819",
               data: String(repeating: "cd", count: 50),
               tag: "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"),
        Vector(name: "rfc4231-tc6",
               key: String(repeating: "aa", count: 131),
               data: "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b6579202d2048617368204b6579204669727374",
               tag: "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"),
        Vector(name: "rfc4231-tc7",
               key: String(repeating: "aa", count: 131),
               data: "5468697320697320612074657374207573696e672061206c6172676572207468616e20626c6f636b2d73697a65206b657920616e642061206c6172676572207468616e20626c6f636b2d73697a6520646174612e20546865206b6579206e6565647320746f20626520686173686564206265666f7265206265696e6720757365642062792074686520484d414320616c676f726974686d2e",
               tag: "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"),
        Vector(name: "weft-empty-payload",
               key: String(repeating: "5a", count: 32),
               data: "",
               tag: "87a26610b4e32f22d6d403b2397f534fb64c83b15aa53deaec60b1afa31dbb74"),
        Vector(name: "weft-one-byte",
               key: String(repeating: "5a", count: 32),
               data: "ff",
               tag: "869b6896716dbdbce95aa32d75657fae807c82b8d52c25c83b7afa617271b7c8"),
    ]

    func hex(_ s: String) -> Data {
        var d = Data(capacity: s.count / 2)
        var idx = s.startIndex
        while idx < s.endIndex {
            let next = s.index(idx, offsetBy: 2)
            d.append(UInt8(s[idx..<next], radix: 16)!)
            idx = next
        }
        return d
    }

    func hexString(_ d: Data) -> String {
        d.map { String(format: "%02x", $0) }.joined()
    }

    // --- V1: fixture vectors ------------------------------------------------

    func testV1FixtureVectors() {
        for v in vectors {
            let tag = vwHmacSha256(key: hex(v.key), data: hex(v.data))
            XCTAssertEqual(v.tag, hexString(tag), "\(v.name): tag matches shared fixture")
        }
    }

    // --- V2: key derivation ---------------------------------------------------

    func testV2Derivation() {
        let secret = Data("v2-secret".utf8)
        let k1 = vwDeriveKey(secret: secret)
        let k2 = vwDeriveKey(secret: secret)
        XCTAssertEqual(32, k1.count, "32-byte key")
        XCTAssertEqual(k1, k2, "deterministic")
        XCTAssertNotEqual(k1, vwDeriveKey(secret: Data("other".utf8)),
                          "different secret -> different key")
        XCTAssertEqual(hexString(vwHmacSha256(key: secret, data: vwDomain)), hexString(k1),
                       "key = HMAC(secret, domain) exactly")
    }

    // --- V3: record roundtrip ---------------------------------------------------

    func testV3Roundtrip() {
        let key = vwDeriveKey(secret: Data("v3-roundtrip".utf8))
        let signer = VwSigner(authKey: key)
        var envelope = Data()
        for plen in [0, 1, 16, 63, 64, 65, 128, 300] {
            let payload = Data((0..<plen).map { UInt8(($0 * 7 + 3) & 0xff) })
            vwEnvelopeEncodeV1(dst: &envelope, seq: 42, payloadLen: UInt32(plen))
            let tag = signer.sign(envelope: envelope, payload: payload)
            var rec = Data(count: 16 + plen + 32)
            let n = vwRecordEncode(envelope: envelope, payload: payload, tag: tag, dst: &rec)
            XCTAssertEqual(16 + plen + 32, n, "record length (\(plen))")
            let r = vwRecordDecodeVerify(authKey: key, src: rec)
            XCTAssertEqual(Int32(0), r.code, "decode+verify OK (\(plen))")
            XCTAssertEqual(plen, r.records.first?.payloadLen, "payload view length")
            XCTAssertEqual(UInt32(42), r.records.first?.seq, "seq decoded")
        }
    }

    // --- V4: exhaustive tamper ---------------------------------------------------

    func testV4Tamper() {
        let key = vwDeriveKey(secret: Data("v4-tamper".utf8))
        let signer = VwSigner(authKey: key)
        var envelope = Data()
        vwEnvelopeEncodeV1(dst: &envelope, seq: 7, payloadLen: 64)
        let payload = Data((0..<64).map { UInt8($0 * 13) })
        let tag = signer.sign(envelope: envelope, payload: payload)
        var rec = Data(count: 16 + 64 + 32)
        _ = vwRecordEncode(envelope: envelope, payload: payload, tag: tag, dst: &rec)

        var rejected = 0
        for i in 0..<rec.count {
            var tampered = rec
            tampered[tampered.startIndex + i] ^= 0x80
            if vwRecordDecodeVerify(authKey: key, src: tampered).code != 0 { rejected += 1 }
        }
        XCTAssertEqual(rec.count, rejected, "all \(rec.count) byte flips rejected")
    }

    // --- V5: rejections ---------------------------------------------------

    func testV5Rejections() {
        let key = vwDeriveKey(secret: Data("v5-key".utf8))
        let wrong = vwDeriveKey(secret: Data("wrong".utf8))
        let signer = VwSigner(authKey: key)
        var envelope = Data()
        vwEnvelopeEncodeV1(dst: &envelope, seq: 1, payloadLen: 24)
        let payload = Data(repeating: 0xAB, count: 24)
        let tag = signer.sign(envelope: envelope, payload: payload)
        var rec = Data(count: 16 + 24 + 32)
        _ = vwRecordEncode(envelope: envelope, payload: payload, tag: tag, dst: &rec)

        XCTAssertEqual(Int32(3), vwRecordDecodeVerify(authKey: wrong, src: rec).code, "wrong key")
        XCTAssertEqual(Int32(1), vwRecordDecodeVerify(authKey: key, src: Data(count: 40)).code, "short record")
        var badMagic = rec
        badMagic[badMagic.startIndex] = UInt8(ascii: "X")
        XCTAssertEqual(Int32(2), vwRecordDecodeVerify(authKey: key, src: badMagic).code, "bad magic")
        // Hostile geometry: high-bit payload_len must land in errShort, never
        // wrap negative (the Kotlin port's lesson, pinned in every port).
        var hostile = rec
        hostile[hostile.startIndex + 15] = 0x80
        XCTAssertEqual(Int32(1), vwRecordDecodeVerify(authKey: key, src: hostile).code, "hostile plen")
    }

    // --- V8: pre-keyed verifier == one-shot --------------------------------------

    func testV8PrekeyedVerifier() {
        let key = vwDeriveKey(secret: Data("v8-stream".utf8))
        let verifier = VwVerifier(authKey: key)
        let signer = VwSigner(authKey: key)
        var envelope = Data()
        var payload = Data(count: 64)
        for i in 0..<1000 {
            vwEnvelopeEncodeV1(dst: &envelope, seq: UInt32(i), payloadLen: 64)
            for j in 0..<64 { payload[payload.startIndex + j] = UInt8((i + j) & 0xff) }
            var tag = signer.sign(envelope: envelope, payload: payload)
            XCTAssertEqual(Int32(0), verifier.verify(envelope: envelope, payload: payload, tag: tag),
                           "pre-keyed OK (\(i))")
            XCTAssertEqual(Int32(0), vwVerify(authKey: key, envelope: envelope, payload: payload, tag: tag),
                           "one-shot OK (\(i))")
            tag[tag.startIndex] ^= 1
            XCTAssertEqual(Int32(3), verifier.verify(envelope: envelope, payload: payload, tag: tag),
                           "tamper red (\(i))")
        }
        vwEnvelopeEncodeV1(dst: &envelope, seq: 0, payloadLen: 64)
        for j in 0..<64 { payload[payload.startIndex + j] = UInt8(j) }
        let tag = signer.sign(envelope: envelope, payload: payload)
        XCTAssertEqual(Int32(0), verifier.verify(envelope: envelope, payload: payload, tag: tag),
                       "state stable after stream")
    }

    // --- V9: batch decode-verify ---------------------------------------------------

    func buildStream(n: Int, plen: Int, key: Data) -> Data {
        let signer = VwSigner(authKey: key)
        let recLen = VW_ENVELOPE_LEN + plen + VW_TAG_LEN
        var stream = Data(count: n * recLen)
        var envelope = Data()
        let payload = Data((0..<plen).map { UInt8($0) })
        for i in 0..<n {
            vwEnvelopeEncodeV1(dst: &envelope, seq: UInt32(i * 3 + 1), payloadLen: UInt32(plen))
            let tag = signer.sign(envelope: envelope, payload: payload)
            var slice = Data(count: recLen)
            let written = vwRecordEncode(envelope: envelope, payload: payload, tag: tag, dst: &slice)
            let start = stream.startIndex + i * recLen
            stream.replaceSubrange(start..<start + written, with: slice)
        }
        return stream
    }

    func testV9Batch() {
        let key = vwDeriveKey(secret: Data("v9-batch".utf8))
        let n = 500
        let plen = 48
        let recLen = VW_ENVELOPE_LEN + plen + VW_TAG_LEN
        let stream = buildStream(n: n, plen: plen, key: key)

        var r = vwBatchDecodeVerify(authKey: key, src: stream)
        XCTAssertEqual(Int32(0), r.code, "all OK")
        XCTAssertEqual(n, r.verified, "count")
        XCTAssertEqual(n * recLen, r.bytesConsumed, "consumed exact")
        XCTAssertEqual(n, r.records.count, "views size")
        for i in 0..<n {
            XCTAssertEqual(i * recLen, r.records[i].offset, "view offset \(i)")
            XCTAssertEqual(UInt32(i * 3 + 1), r.records[i].seq, "view seq \(i)")
        }

        // Tamper record 137.
        var tampered = stream
        tampered[tampered.startIndex + 137 * recLen + 20] ^= 0x40
        r = vwBatchDecodeVerify(authKey: key, src: tampered)
        XCTAssertEqual(Int32(3), r.code, "tamper code")
        XCTAssertEqual(137, r.verified, "good prefix")
        XCTAssertEqual(137 * recLen, r.bytesConsumed, "resync offset")

        // Fresh stream for truncation policy.
        let fresh = buildStream(n: n, plen: plen, key: key)
        r = vwBatchDecodeVerify(authKey: key, src: fresh.prefix((n - 1) * recLen + 17))
        XCTAssertEqual(Int32(0), r.code, "short tail OK")
        XCTAssertEqual(n - 1, r.verified, "short tail count")
        r = vwBatchDecodeVerify(authKey: key, src: fresh.prefix(n * recLen - 30))
        XCTAssertEqual(Int32(1), r.code, "mid-record short")
        XCTAssertEqual(n - 1, r.verified, "mid-record prefix")

        // Capped views still verify all.
        r = vwBatchDecodeVerify(authKey: key, src: fresh, maxViews: 10)
        XCTAssertEqual(n, r.verified, "capped still verifies all")
        XCTAssertEqual(10, r.records.count, "cap respected")
    }
}
