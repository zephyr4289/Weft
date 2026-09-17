// GovernedConsumerTests.swift — RFC-0009 Series 7: the composed display
// consumer conformance suite, Swift (the GovernedFanoutConsumerTest.kt /
// governed_consumer_test.dart twin): the exact-count steady-cadence regime
// end to end, PC2 telescoping through the ring, the ladder's advisory
// classes on real drop accounting, and dispose-to-pool.
//
// Environment tag: XCTest on macOS/ARM (apple-packages CI); NOT runnable
// in the x86_64 Linux sandbox (no Swift toolchain there) — declared, per
// the repo's per-port honesty culture (PORTS.md §9).

import XCTest
@testable import WeftCore

final class GovernedConsumerTests: XCTestCase {

    // (The broadcaster owns its ring allocation — readers attach to b.ring.)

    private func publish(_ b: WeftFanoutBroadcaster, tag: Int, words: Int) {
        let w = b.begin()
        for i in 0..<words {
            let v = UInt32(truncatingIfNeeded: Int64(tag * 31 + i * 2654435761))
            w.storeBytes(of: v.littleEndian, toByteOffset: 4 * i, as: UInt32.self)
        }
        _ = b.publish()
    }

    // --- C1: PACED 30-on-120 end to end ---

    func testC1Paced30On120SteadyRasterEndToEnd() {
        let words = 64
        let b = WeftFanoutBroadcaster(payloadBytes: words * 4, slotCount: 4)
        let reader = WeftFanoutReader(ring: b.ring, ringBytes: weftFanoutRingBytes(payloadBytes: words * 4, slotCount: 4), payloadBytes: words * 4, slotCount: 4)
        let pool = WeftBufferRecycler(slotBytes: words * 4, maxFreeSlots: 1)
        let consumer = GovernedFanoutConsumer(reader: reader,
                                              policyKind: CadencePolicyKind.pacedInterpolate,
                                              rasterPool: pool)
        var clock: Int64 = 0
        consumer.clock = { clock }

        var presents = 0
        for t in 1...400 {
            if t % 4 == 1 { publish(b, tag: t / 4 + 1, words: words) } // 30 Hz feed
            clock = Int64(t)
            if consumer.tick().present { presents += 1 }
        }
        // The PC5 battery's exact count for this regime.
        XCTAssertEqual(398, presents)
        XCTAssertEqual(consumer.cadence.presents, Int64(presents))
        XCTAssertTrue(consumer.cadence.interpFrames > 0) // synthesis exercised
    }

    // --- C2: PC2 telescoping through the ring ---

    func testC2TelescopingHoldsThroughTheRing() {
        func xorshift32(_ x0: UInt32) -> UInt32 {
            var x = x0
            x ^= (x << 13)
            x ^= (x >> 17)
            x ^= (x << 5)
            return x
        }
        for kind in [CadencePolicyKind.latestWins, CadencePolicyKind.burstCoalesce] {
            let words = 32
            let b = WeftFanoutBroadcaster(payloadBytes: words * 4, slotCount: 4)
            let reader = WeftFanoutReader(ring: b.ring, ringBytes: weftFanoutRingBytes(payloadBytes: words * 4, slotCount: 4), payloadBytes: words * 4, slotCount: 4)
            let consumer = GovernedFanoutConsumer(reader: reader, policyKind: kind)
            consumer.clock = { 0 }
            var state: UInt32 = 0x00c0ffee
            for t in 1...10_000 {
                state = xorshift32(state)
                let arrivals = Int(state % 5)
                if arrivals > 0 || t % 3 == 0 { publish(b, tag: t, words: words) }
                _ = consumer.tick()
            }
            let c = consumer.cadence
            XCTAssertEqual(c.lastPresentedSeqForTest() - c.presents,
                           c.coalescedByDecision, "C2 kind=\(kind)")
        }
    }

    // --- C3: the ladder sees the ring's REAL drop accounting ---

    func testC3LadderClassesFromRealDropAccounting() {
        let words = 16
        let b = WeftFanoutBroadcaster(payloadBytes: words * 4, slotCount: 4)
        let reader = WeftFanoutReader(ring: b.ring, ringBytes: weftFanoutRingBytes(payloadBytes: words * 4, slotCount: 4), payloadBytes: words * 4, slotCount: 4)
        let consumer = GovernedFanoutConsumer(reader: reader,
                                              policyKind: CadencePolicyKind.latestWins)
        var clock: Int64 = 0
        consumer.clock = { clock }

        // Steady feed: FastPath (behind == 0).
        publish(b, tag: 1, words: words); _ = consumer.tick()
        publish(b, tag: 2, words: words)
        _ = consumer.tick()
        XCTAssertEqual(GovernorActionKind.fastPath, consumer.action.kind)

        // Burst of 4 between ticks: dropped=3 -> Skip(2) ladder class.
        publish(b, tag: 3, words: words)
        publish(b, tag: 4, words: words)
        publish(b, tag: 5, words: words)
        publish(b, tag: 6, words: words)
        clock += 100
        _ = consumer.tick()
        XCTAssertEqual(GovernorActionKind.skip, consumer.action.kind)
        XCTAssertEqual(2, consumer.action.skipN)
        XCTAssertTrue(consumer.actionChanged)

        // Behind 0 again: back to FastPath (edge).
        clock += 100
        _ = consumer.tick()
        XCTAssertEqual(GovernorActionKind.fastPath, consumer.action.kind)

        // Massive burst: dropped=40 -> Reseed (post-cooldown).
        clock += 1000
        for s in 7...47 { publish(b, tag: s, words: words) }
        _ = consumer.tick()
        XCTAssertEqual(GovernorActionKind.reseed, consumer.action.kind)
        XCTAssertEqual(1, consumer.staleness.reseeds)
    }

    // --- C4: dispose returns the raster slot to the pool ---

    func testC4DisposeReturnsRasterSlotToPool() {
        let words = 8
        let b = WeftFanoutBroadcaster(payloadBytes: words * 4, slotCount: 4)
        let reader = WeftFanoutReader(ring: b.ring, ringBytes: weftFanoutRingBytes(payloadBytes: words * 4, slotCount: 4), payloadBytes: words * 4, slotCount: 4)
        let pool = WeftBufferRecycler(slotBytes: words * 4, maxFreeSlots: 2)
        let consumer = GovernedFanoutConsumer(reader: reader,
                                              policyKind: CadencePolicyKind.latestWins,
                                              rasterPool: pool)
        XCTAssertEqual(1, pool.liveNow)
        XCTAssertEqual(0, pool.pooledNow)
        let r = consumer.raster
        consumer.dispose()
        consumer.dispose() // idempotent
        XCTAssertEqual(0, pool.liveNow)
        XCTAssertEqual(1, pool.pooledNow)
        XCTAssertTrue(pool.acquire() == r, "pooled slot identity reused")
    }
}
