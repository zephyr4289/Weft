import XCTest
@testable import WeftCore

final class WeftTests: XCTestCase {

    func testPublishAndClaimBasic() {
        let payloadMax = 256
        let weft = Weft(payloadMax: payloadMax)

        XCTAssertEqual(weft.tPublishCount(), 0)
        XCTAssertEqual(weft.tClaimCount(), 0)

        let wBuf = weft.wBegin
        wBuf.storeBytes(of: UInt8(0x42), as: UInt8.self)
        wBuf.advanced(by: 1).storeBytes(of: UInt8(0x99), as: UInt8.self)

        let pubRes = weft.publish(seq: 1, payloadLen: UInt32(payloadMax))
        XCTAssertEqual(pubRes, .ok)
        XCTAssertEqual(weft.tPublishCount(), 1)

        let heldIdx = weft.claim()
        XCTAssertEqual(weft.tClaimCount(), 1)
        XCTAssertEqual(weft.rSeq(), 1)
        XCTAssertEqual(weft.rMagic(), WEFT_MAGIC)
        XCTAssertEqual(weft.rPayloadLen(), UInt32(payloadMax))
        XCTAssertEqual(heldIdx, 1)

        if let rPtr = weft.rLivePtr(16) {
            let byte0 = rPtr.load(as: UInt8.self)
            let byte1 = rPtr.advanced(by: 1).load(as: UInt8.self)
            XCTAssertEqual(byte0, 0x42)
            XCTAssertEqual(byte1, 0x99)
        } else {
            XCTFail("rLivePtr returned nil")
        }
    }

    func testRevocationAndReclaim() {
        let weft = Weft(payloadMax: 128)
        weft.revoke()

        let res = weft.publish(seq: 1, payloadLen: 128)
        XCTAssertEqual(res, .droppedRevoked)
        XCTAssertEqual(weft.tDropCount(), 1)

        let reclaimed = weft.reclaim(preRevokeEpoch: 0, timeoutMs: 100)
        XCTAssertTrue(reclaimed)
    }

    func testEnvelopeDecodeValidation() {
        let ptr = UnsafeMutableRawPointer.allocate(byteCount: 64, alignment: 64)
        defer { ptr.deallocate() }

        envelopeEncodeV1(ptr, seq: 42, payloadLen: 32)
        let dec = envelopeDecode(UnsafeRawPointer(ptr), avail: 64)
        XCTAssertEqual(dec, .ok)

        let shortDec = envelopeDecode(UnsafeRawPointer(ptr), avail: 10)
        XCTAssertEqual(shortDec, .short)
    }
}
