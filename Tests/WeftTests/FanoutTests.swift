// FanoutTests.swift — RFC-0004 fan-out driver-layer conformance suite, Swift.
//
// The F-series counterpart of packages/core/test/fanout.test.ts,
// core/c/fanout_test.c, and FanoutTest.kt: geometry, the stamp-then-fill
// writer protocol, per-reader fresh/drop accounting, the graceful-skip tear
// discipline, the canonical four-consumer scenario, and a REAL multi-thread
// torture (writer + 3 readers on DispatchQueues — the analog of the C
// torture runner and the Kotlin executor battery).
//
// Payload mixer: weft_mix32-based u32 words (04-LITMUS §0.1 pattern family —
// the same generator as the C/Kotlin F-series and the xlang fixtures), so
// payload validation is bit-exact against the shared family.
//
// Environment tag for any timing-sensitive numbers: XCTest (sandbox).

import XCTest
@testable import WeftCore

final class FanoutTests: XCTestCase {

    // --- shared mixer (weft_mix32, 04-LITMUS §0.1 — identical to core/c).
    //     STATIC: pure functions, no self capture from concurrent closures. ---

    private static func mix32(_ xIn: UInt32) -> UInt32 {
        var x = xIn
        x ^= x >> 16
        x &*= 0x7FEB352D
        x ^= x >> 15
        x &*= 0x846CA68B
        x ^= x >> 16
        return x
    }

    private static func tword(_ seq: UInt32, _ w: UInt32) -> UInt32 {
        mix32(seq &* 2654435761 &+ w)
    }

    private static func mixerFrame(_ words: Int, _ seq: UInt32) -> [UInt32] {
        (0..<UInt32(words)).map { tword(seq, $0) }
    }

    private static func expectFrame(_ view: [UInt32], _ seq: UInt32, _ words: Int) -> Bool {
        for w in 0..<words where view[w] != tword(seq, UInt32(w)) { return false }
        return true
    }

    private func fillFrame(_ b: WeftFanoutBroadcaster, _ seq: UInt32, _ words: Int) {
        b.begin()
        XCTAssertEqual(words, b.fill(FanoutTests.mixerFrame(words, seq), words))
    }

    // ----- F1: geometry validation -----

    func testF1GeometryValidation() {
        XCTAssertEqual(0, weftFanoutRingBytes(payloadBytes: 0, slotCount: 4))
        XCTAssertEqual(0, weftFanoutRingBytes(payloadBytes: 6, slotCount: 4)) // %4 != 0
        XCTAssertEqual(0, weftFanoutRingBytes(payloadBytes: 256, slotCount: 1)) // < 2
        XCTAssertEqual(0, weftFanoutRingBytes(payloadBytes: 256, slotCount: 0))
        XCTAssertEqual(0, weftFanoutRingBytes(payloadBytes: 256, slotCount: WEFT_FANOUT_MAX_SLOTS + 1))
        // ring_bytes formula: 16 + 8M + M*payload_bytes — TS/C/Kotlin parity.
        XCTAssertEqual(16 + 8 * 4 + 4 * 256, weftFanoutRingBytes(payloadBytes: 256, slotCount: 4))
        // Constructors reject bad geometry via precondition (fatal in debug
        // builds — the kernel port's stance); the helper above is the
        // testable contract surface, and attach mismatches are validated by
        // the ringBytes precondition in WeftFanoutReader.init.
        let b = WeftFanoutBroadcaster(payloadBytes: 256, slotCount: 4)
        XCTAssertEqual(16 + 8 * 4 + 4 * 256,
                       weftFanoutRingBytes(payloadBytes: b.payloadBytes, slotCount: b.slotCount))
    }

    // ----- F2: roundtrip (null frame -> first publish -> fresh claim) -----

    func testF2Roundtrip() {
        let b = WeftFanoutBroadcaster(payloadBytes: 256, slotCount: 4)
        let r = b.createReader()
        let c0 = r.claim()
        XCTAssertFalse(c0.fresh)
        XCTAssertEqual(0, c0.seq)
        XCTAssertEqual(0, c0.dropped)
        fillFrame(b, 1, 64)
        XCTAssertEqual(1, b.publish())
        let c1 = r.claim()
        XCTAssertTrue(c1.fresh)
        XCTAssertEqual(1, c1.seq)
        XCTAssertEqual(0, c1.dropped)
        XCTAssertTrue(FanoutTests.expectFrame(r.view(), 1, 64))
        // publish() before any begin() is a detectable no-op (TS/C parity).
        let b2 = WeftFanoutBroadcaster(payloadBytes: 256, slotCount: 4)
        XCTAssertEqual(0, b2.publish())
    }

    // ----- F3: drop accounting -----

    func testF3DropAccounting() {
        let b = WeftFanoutBroadcaster(payloadBytes: 256, slotCount: 4)
        fillFrame(b, 1, 64); b.publish()
        let r = b.createReader()
        _ = r.claim() // lastSeq = 1 (the TS/C F3 structure: claim the baseline first)
        for f in 2...6 {
            fillFrame(b, UInt32(f), 64); b.publish()
        }
        let c = r.claim() // jumps to frame 6; frames 2..5 dropped
        XCTAssertTrue(c.fresh)
        XCTAssertEqual(6, c.seq)
        XCTAssertEqual(4, c.dropped) // frames 2..5 unseen
        XCTAssertTrue(FanoutTests.expectFrame(r.view(), 6, 64))
    }

    // ----- F4: telescoping identity across an interleaved sequence -----

    func testF4TelescopingIdentity() {
        let b = WeftFanoutBroadcaster(payloadBytes: 256, slotCount: 4)
        let r = b.createReader()
        var sumDropped: UInt64 = 0
        var freshClaims: UInt64 = 0
        for seq in 1...60 {
            fillFrame(b, UInt32(seq), 64)
            b.publish()
            if seq % 3 == 0 {
                let c = r.claim()
                if c.fresh { sumDropped += c.dropped; freshClaims += 1 }
            }
        }
        let c = r.claim()
        if c.fresh { sumDropped += c.dropped; freshClaims += 1 }
        // The exact telescoping identity (RFC 0004):
        // sum(dropped) == lastSeq - freshClaims
        XCTAssertEqual(UInt64(60) - freshClaims, sumDropped)
        XCTAssertEqual(60, c.seq)
    }

    // ----- F5: ring overwrite -> claim yields the LATEST frame -----

    func testF5RingOverwriteYieldsLatest() {
        let b = WeftFanoutBroadcaster(payloadBytes: 256, slotCount: 4)
        let r = b.createReader()
        for f in 1...7 { // M+3 publishes: slots overwritten twice over
            fillFrame(b, UInt32(f), 64)
            b.publish()
        }
        let c = r.claim()
        XCTAssertTrue(c.fresh)
        XCTAssertEqual(7, c.seq)
        XCTAssertTrue(FanoutTests.expectFrame(r.view(), 7, 64))
    }

    // ----- F6: graceful-skip tear discipline (deterministic exercise) -----

    func testF6GracefulSkip() {
        let b = WeftFanoutBroadcaster(payloadBytes: 64, slotCount: 4)
        fillFrame(b, 1, 16); b.publish()
        let r = b.createReader()
        XCTAssertEqual(1, r.claim().seq) // lastSeq = 1
        for f in 2...4 { fillFrame(b, UInt32(f), 16); b.publish() }
        // latest = 4 lives in slot 3. M abandoned begins advance wSeq to 8;
        // the 4th invalidates slot 3 — the very slot latest points at.
        for _ in 0..<4 { _ = b.begin() }
        let c = r.claim()
        XCTAssertFalse(c.fresh) // graceful skip, not a torn frame
        XCTAssertEqual(1, c.seq) // keeps the last consistent frame
        XCTAssertEqual(1, r.stats().skippedMidOverwrite)
        // The in-flight frame completes -> the next claim resumes cleanly.
        fillFrame(b, 9, 16)
        b.publish()
        let c2 = r.claim()
        XCTAssertTrue(c2.fresh)
        XCTAssertEqual(9, c2.seq)
        XCTAssertEqual(7, c2.dropped) // frames 2..8 gap over lastSeq=1
        XCTAssertTrue(FanoutTests.expectFrame(r.view(), 9, 16))
    }

    // ----- F6b: abandoned begins are skip-observable, not silent -----

    func testF6bAbandonedBeginsObservable() {
        let b = WeftFanoutBroadcaster(payloadBytes: 64, slotCount: 4)
        for f in 1...4 { fillFrame(b, UInt32(f), 16); b.publish() }
        for _ in 0..<4 { _ = b.begin() } // none ever published
        let r = b.createReader()
        let c = r.claim()
        XCTAssertFalse(c.fresh)
        XCTAssertEqual(0, c.seq)
        XCTAssertEqual(1, r.stats().skippedMidOverwrite)
        fillFrame(b, 9, 16)
        b.publish()
        let c2 = r.claim()
        XCTAssertTrue(c2.fresh)
        XCTAssertEqual(9, c2.seq)
        // Seqs 5..8 never completed, yet read as 8 drops — abandoned seqs
        // are indistinguishable from missed publishes (declared boundary).
        XCTAssertEqual(8, c2.dropped)
    }

    // ----- F7: canonical four-consumer scenario, deterministic closed form -----

    func testF7FourConsumerScenario() {
        let n = 10_000
        let b = WeftFanoutBroadcaster(payloadBytes: 256, slotCount: 4)
        let consumers: [(name: String, divisor: Int, reader: WeftFanoutReader)] = [
            ("flight-recorder", 1, b.createReader()),
            ("primary-canvas", 2, b.createReader()),
            ("minimap", 4, b.createReader()),
            ("network-viz", 8, b.createReader())
        ]
        var integrityFailures = 0
        for t in 1...n {
            fillFrame(b, UInt32(t), 64)
            b.publish()
            for consumer in consumers where t % consumer.divisor == 0 {
                let claim = consumer.reader.claim()
                if !claim.fresh {
                    integrityFailures += 1
                } else {
                    if claim.dropped != consumer.divisor - 1 { integrityFailures += 1 }
                    if !FanoutTests.expectFrame(consumer.reader.view(), UInt32(claim.seq), 64) {
                        integrityFailures += 1
                    }
                }
            }
        }
        XCTAssertEqual(0, integrityFailures)
        for consumer in consumers {
            let st = consumer.reader.stats()
            let expectedFresh = UInt64(n / consumer.divisor)
            XCTAssertEqual(expectedFresh, st.reads)
            XCTAssertEqual(expectedFresh, st.fresh)
            XCTAssertEqual(UInt64(n) - expectedFresh, st.drops)
            XCTAssertEqual(0, st.skippedMidOverwrite) // single-threaded schedule
            XCTAssertEqual(0, st.tornExhausted)
            XCTAssertEqual(n, consumer.reader.claim().seq)
        }
    }

    // ----- F8: zero-allocation contract (Law 2) — identity stability -----
    // claim() must hand back the SAME record object; view() must expose the
    // SAME copy buffer (stable base address) across 1,000 claims.

    func testF8ZeroAllocationIdentity() {
        let b = WeftFanoutBroadcaster(payloadBytes: 256, slotCount: 4)
        let r = b.createReader()
        let firstClaim: FanoutClaim = r.claim()
        let firstBase = r.view().withUnsafeBufferPointer { $0.baseAddress }
        for f in 1...1000 {
            fillFrame(b, UInt32(f), 64)
            b.publish()
            let c = r.claim()
            if c !== firstClaim {
                XCTFail("claim record identity changed at frame \(f)")
                return
            }
            let base = r.view().withUnsafeBufferPointer { $0.baseAddress }
            if base != firstBase {
                XCTFail("view buffer identity changed at frame \(f)")
                return
            }
        }
    }

    // ----- F9: layout parity — raw ctrl bytes are the wire contract -----
    // (Plain loads on a quiesced ring; native-endian — every CI runner is
    // LE, and the layout contract IS little-endian. The existing Weft.swift
    // envelope decoders take the same stance on LE hosts.)

    func testF9LayoutParityRawCtrl() {
        let b = WeftFanoutBroadcaster(payloadBytes: 64, slotCount: 4)
        fillFrame(b, 1, 16); b.publish()
        XCTAssertEqual(1, b.ring.load(as: UInt64.self))                      // byte 0: latestSeq
        XCTAssertEqual(1, b.ring.load(fromByteOffset: 8, as: UInt64.self))   // publishes
        XCTAssertEqual(1, b.ring.load(fromByteOffset: 16, as: UInt64.self))  // slotSeq[0]
        XCTAssertEqual(0, b.ring.load(fromByteOffset: 24, as: UInt64.self))  // slotSeq[1]: untouched -> 0
        // payload word 0 of slot 0 at 16 + 8*4 = byte 48 (LE u32)
        XCTAssertEqual(FanoutTests.tword(1, 0), b.ring.load(fromByteOffset: 48, as: UInt32.self))
    }

    // ----- F10: multi-thread torture — writer + 3 readers -----

    func testF10MultiThreadTorture() {
        let frames = 100_000
        let words = 64
        let payloadBytes = words * 4
        let b = WeftFanoutBroadcaster(payloadBytes: payloadBytes, slotCount: 4)
        let readers = [b.createReader(), b.createReader(), b.createReader()]

        // Shared failure counter (pointer — no captured-var races).
        let integrityFailures = UnsafeMutablePointer<Int>.allocate(capacity: 1)
        integrityFailures.initialize(to: 0)
        defer { integrityFailures.deallocate() }

        let group = DispatchGroup()

        // Writer: mixer frames 1..frames, tight loop.
        DispatchQueue(label: "weft.fanout.torture.writer", qos: .userInitiated)
            .async(group: group) {
                var src = [UInt32](repeating: 0, count: words)
                for f in 1...frames {
                    for w in 0..<words { src[w] = FanoutTests.tword(UInt32(f), UInt32(w)) }
                    b.begin()
                    b.fill(src, words)
                    b.publish()
                }
            }

        // Readers: tight claim loops; every fresh claim word-validated; a
        // torn frame ACCEPTED would flip integrityFailures (the protocol's
        // job is to make that impossible); graceful skips are legal.
        let readerQueue = DispatchQueue(label: "weft.fanout.torture.readers",
                                        attributes: .concurrent)
        for r in readers {
            readerQueue.async(group: group) {
                while true {
                    let c = r.claim()
                    if c.fresh {
                        if !FanoutTests.expectFrame(r.view(), UInt32(c.seq), words) {
                            integrityFailures.pointee += 1
                        }
                        if c.seq >= frames { break }
                    } else if c.seq >= frames {
                        break
                    }
                }
            }
        }

        // Converge (bounded wait — a hang is a failure, not a pass).
        let result = group.wait(timeout: .now() + 120)
        XCTAssertEqual(.success, result, "torture did not converge in 120s")

        XCTAssertEqual(0, integrityFailures.pointee, "torn or corrupt frame accepted")
        for r in readers {
            let st = r.stats()
            // Telescoping identity per reader: drops == frames - fresh.
            XCTAssertEqual(UInt64(frames) - st.fresh, st.drops)
            XCTAssertEqual(UInt64(frames), r.claim().seq) // convergence
        }
    }
}
