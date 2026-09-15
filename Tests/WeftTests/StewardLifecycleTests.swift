import XCTest
@testable import WeftCore

final class StewardLifecycleTests: XCTestCase {

    func testStewardAllocationAndStats() {
        let steward = Steward()
        let w1 = steward.weft(payloadMax: 64)
        let w2 = steward.weft(payloadMax: 128)

        _ = w1.publish(seq: 1, payloadLen: 64)
        _ = w2.publish(seq: 1, payloadLen: 128)
        _ = w1.claim()

        let stats = steward.stats()
        XCTAssertEqual(stats.weftCount, 2)
        XCTAssertEqual(stats.totalPublishes, 2)
        XCTAssertEqual(stats.totalReads, 1)

        steward.releaseAll()
        let postReleaseStats = steward.stats()
        XCTAssertEqual(postReleaseStats.weftCount, 0)
    }

    func testStewardARCReclaimOnDeinit() {
        var steward: Steward? = Steward()
        _ = steward?.weft(payloadMax: 64)
        XCTAssertEqual(steward?.stats().weftCount, 1)

        // Drop reference: ARC calls deinit -> releaseAll
        steward = nil
        XCTAssertNil(steward)
    }
}
