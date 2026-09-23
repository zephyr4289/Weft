// Mdp1WireTests.swift — XCTest for the MDP1 wire (Apple CI lane).
//
// CRC known answers are externally derived with node:zlib (the repo
// oracle since Pillar 2) and re-derived by audit/static_audit.mjs.

import XCTest
@testable import WeftFintech

final class Mdp1WireTests: XCTestCase {
    func makeBuffer(_ fill: (inout [UInt8]) -> Void) -> UnsafeRawBufferPointer {
        var bytes = [UInt8](repeating: 0, count: Mdp1.size)
        fill(&bytes)
        return UnsafeRawBufferPointer(bytes.withUnsafeBufferPointer { $0 })
    }

    func testCrcKnownAnswers() {
        // KAT1: crc32("WEFT") == 3421166146
        let weft = Array("WEFT".utf8)
        let v1 = Mdp1.crc32(UnsafeRawBufferPointer(weft), 0, 4)
        XCTAssertEqual(v1, 3421166146)
        // KAT2: ascending 0..255 == 688229491
        var asc = [UInt8](repeating: 0, count: 256)
        for i in 0..<256 { asc[i] = UInt8(i) }
        let v2 = Mdp1.crc32(UnsafeRawBufferPointer(asc), 0, 256)
        XCTAssertEqual(v2, 688229491)
        // KAT3: canonical 304B pattern (i*7+13), region [0,296) == 3666738418
        var canon = [UInt8](repeating: 0, count: 304)
        for i in 0..<304 { canon[i] = UInt8((i * 7 + 13) & 0xFF) }
        let v3 = Mdp1.crc32(UnsafeRawBufferPointer(canon), 0, 296)
        XCTAssertEqual(v3, 3666738418)
    }

    func testSynthRecordDecodesExactly() {
        var bytes = [UInt8](repeating: 0, count: Mdp1.size)
        bytes.withUnsafeMutableBufferPointer { buf in
            let p = UnsafeMutableRawBufferPointer(buf)
            func putU16(_ off: Int, _ v: UInt16) {
                withUnsafeBytes(of: v.littleEndian) { p.copyBytes(from: $0, to: off) }
            }
            func putU32(_ off: Int, _ v: UInt32) {
                withUnsafeBytes(of: v.littleEndian) { p.copyBytes(from: $0, to: off) }
            }
            func putU64(_ off: Int, _ v: UInt64) {
                withUnsafeBytes(of: v.littleEndian) { p.copyBytes(from: $0, to: off) }
            }
            putU32(0, Mdp1.magic)
            putU16(Mdp1.offVersion, Mdp1.version)
            putU16(Mdp1.offFlags, Mdp1.fBookValid)
            putU64(Mdp1.offSeq, 1_000_000)
            putU64(Mdp1.offLastTs, 1_758_400_000_123_456_789 % (1 << 48))
            putU32(Mdp1.offBestBid, 901_234)
            putU32(Mdp1.offBestAsk, 901_240)
            for i in 0..<Mdp1.topLevels {
                putU32(Mdp1.offBids + i * 12, UInt32(901_234 - i * 2))
                putU32(Mdp1.offBids + i * 12 + 4, UInt32(1000 * (10 - i)))
                putU32(Mdp1.offBids + i * 12 + 8, UInt32(10 - i))
                putU32(Mdp1.offAsks + i * 12, UInt32(901_240 + i * 2))
                putU32(Mdp1.offAsks + i * 12 + 4, UInt32(1000 * (10 - i)))
                putU32(Mdp1.offAsks + i * 12 + 8, UInt32(10 - i))
            }
            putU64(Mdp1.offMsgCount, 1_000_000)
            putU32(Mdp1.offTradeCount, 4242)
            let view = Mdp1View(UnsafeRawBufferPointer(p))
            putU32(Mdp1.offCrc, Mdp1.crc32(UnsafeRawBufferPointer(p)))
        }
        let view = Mdp1View(UnsafeRawBufferPointer(bytes))
        XCTAssertEqual(view.validate(), Mdp1.ok)
        XCTAssertEqual(view.seq, 1_000_000)
        XCTAssertEqual(view.bestBid, 901_234)
        XCTAssertEqual(view.bestAsk, 901_240)
        XCTAssertEqual(view.bidSize(0), 10_000)
        XCTAssertEqual(view.askOrders(3), 7)
        XCTAssertTrue(view.crcOk)
        XCTAssertTrue(view.bookValid && !view.crossed && !view.locked)
    }

    func testCorruptionMatrixIsFailClosed() {
        var bytes = [UInt8](repeating: 0, count: Mdp1.size)
        for i in 0..<Mdp1.size { bytes[i] = UInt8((i * 7 + 13) & 0xFF) }
        bytes.withUnsafeMutableBufferPointer { buf in
            let p = UnsafeMutableRawBufferPointer(buf)
            withUnsafeBytes(of: UInt32(3_666_738_418).littleEndian) {
                p.copyBytes(from: $0, to: Mdp1.offCrc)
            }
        }
        // canonical record validates
        XCTAssertEqual(Mdp1View(UnsafeRawBufferPointer(bytes)).validate(), Mdp1.ok)

        // short
        XCTAssertEqual(Mdp1View(UnsafeRawBufferPointer(bytes.prefix(303)))
            .validate(), Mdp1.eShort)
        // bad magic
        var bad = bytes; bad[3] = 0x58
        XCTAssertEqual(Mdp1View(UnsafeRawBufferPointer(bad)).validate(), Mdp1.eMagic)
        // bad version
        var bv = bytes; bv[Mdp1.offVersion] = 2
        XCTAssertEqual(Mdp1View(UnsafeRawBufferPointer(bv)).validate(), Mdp1.eVersion)
        // flipped CRC
        var bc = bytes; bc[Mdp1.offCrc] ^= 0xFF
        XCTAssertEqual(Mdp1View(UnsafeRawBufferPointer(bc)).validate(), Mdp1.eCrc)
        // level payload inside CRC region
        var bl = bytes; bl[Mdp1.offBids + 4] ^= 0xFF
        XCTAssertEqual(Mdp1View(UnsafeRawBufferPointer(bl)).validate(), Mdp1.eCrc)
    }
}
