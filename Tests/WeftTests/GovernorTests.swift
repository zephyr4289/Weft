// GovernorTests.swift — RFC-0009 FreshnessGovernor + §cadence policies
// conformance suite, Swift (Series 7).
//
// The G-series/PC-series counterpart of packages/core/test/governor.test.ts
// + cadence.test.ts, core/c/governor_test.c, core/rust/tests/governor_test.rs,
// GovernorTest.kt (Kotlin/JVM), and governor_test.dart: the ladder, the
// cooldown, the Law-4 counters, the three presentation policies, the
// exact-count steady-cadence regimes, and the identity-stable records.
//
// CROSS-LANGUAGE PARITY, LOCALLY (the Series-7 upgrade): the canonical
// xorshift32 traces (04-LITMUS §0.2) are pinned by FNV-1a-64 hashes
// computed from the TS reference (scripts/gen_trace_refs.mjs — ladder
// 0x3c33156204c7cfdf over 10,000 bytes, cadence 0x6f654c298cbcc9f4 over
// 60,000 bytes). Any arithmetic drift in THIS port fails HERE, without
// needing another toolchain; the fixtures (xlang-governor/vm,
// xlang-cadence) byte-compare all ports in CI as the standing proof.
//
// Environment tag: XCTest on macOS/ARM (apple-packages CI); NOT runnable
// in the x86_64 Linux sandbox (no Swift toolchain there) — declared, per
// the repo's per-port honesty culture (PORTS.md §9). The zero-allocation
// proof for this port is the identity audit + pinned traces (Swift has no
// portable allocation counter; the JVM's allocated-bytes audit is the
// Kotlin leg's proof — the same Law-2 discipline, per-port provable
// roads).

import XCTest
@testable import WeftCore

final class GovernorTests: XCTestCase {

    /// xorshift32 — 04-LITMUS §0.2, the repo's canonical deterministic RNG
    /// (identical step in TS/C/Rust/Kotlin/Swift/Dart).
    private func xorshift32(_ x0: Int64) -> Int64 {
        var x = x0 & 0xffffffff
        x ^= (x << 13) & 0xffffffff
        x ^= x >> 17 // >>> on a masked-32-bit value (logical shift)
        x ^= (x << 5) & 0xffffffff
        return x & 0xffffffff
    }

    /// FNV-1a 64 over the packed trace bytes (wrapping arithmetic — the
    /// same hash every port computes; &* is Swift's wrapping multiply).
    private func fnv1a64(_ bytes: [UInt8]) -> Int64 {
        var h = Int64(bitPattern: 0xcbf29ce484222325)
        for b in bytes {
            h ^= Int64(b)
            h = h &* 0x100000001b3
        }
        return h
    }

    // ------------------------------------------------------------------
    // G-series — the ladder
    // ------------------------------------------------------------------

    func testG1LadderEveryBehindMapsToTheDocumentedAction() {
        let gov = FreshnessGovernor()
        // now advances past the cooldown every step so Reseed is never
        // suppressed (G1 tests the LADDER, not the rate limit).
        for behind in Int64(0)...64 {
            let a = gov.step(framesBehind: behind, nowMs: behind * 1000)
            if behind <= governorDefaults.fastPathBehind {
                XCTAssertEqual(a.kind, GovernorActionKind.fastPath)
                XCTAssertEqual(a.skipN, 0)
            } else if behind <= governorDefaults.skipBehind {
                XCTAssertEqual(a.kind, GovernorActionKind.skip)
                XCTAssertEqual(a.skipN, Int32(behind - governorDefaults.fastPathBehind))
            } else if behind <= governorDefaults.snapshotBehind {
                XCTAssertEqual(a.kind, GovernorActionKind.snapshot)
                XCTAssertEqual(a.skipN, 0)
            } else {
                XCTAssertEqual(a.kind, GovernorActionKind.reseed)
                XCTAssertEqual(a.skipN, 0)
            }
        }
    }

    func testG2MonotoneLargerBehindNeverYieldsAFresherClassAction() {
        let gov = FreshnessGovernor()
        var prevClass: Int32 = -1
        for behind in Int64(0)...64 {
            let a = gov.step(framesBehind: behind, nowMs: behind * 1000)
            XCTAssertTrue(a.kind >= prevClass, "G2 monotone at behind=\(behind)")
            prevClass = a.kind
        }
    }

    func testG3ReseedFlap10kSpikeTraceBoundsAndSpacing() {
        let gov = FreshnessGovernor()
        var state = Int64(0x00c0ffee)
        var reseeds: Int64 = 0
        var lastReseedAt: Int64 = -1
        for i in 0..<10_000 {
            state = xorshift32(state)
            let behind = state % 128
            let a = gov.step(framesBehind: behind, nowMs: Int64(i))
            if a.kind == GovernorActionKind.reseed {
                reseeds += 1
                if lastReseedAt >= 0 {
                    XCTAssertTrue(
                        i - lastReseedAt >= governorDefaults.reseedCooldownMs,
                        "G3 cooldown spacing at i=\(i)"
                    )
                }
                lastReseedAt = Int64(i)
            }
        }
        let bound = (10_000 + governorDefaults.reseedCooldownMs - 1) / governorDefaults.reseedCooldownMs
        XCTAssertTrue(reseeds <= bound, "G3 flap bound (\(reseeds) <= \(bound))")
        XCTAssertTrue(reseeds > 0, "G3 exercised")
        XCTAssertEqual(reseeds, gov.reseeds)
    }

    func testG3bSuppressedReseedDegradesToSnapshot() {
        let gov = FreshnessGovernor()
        XCTAssertEqual(gov.step(framesBehind: 64, nowMs: 1000).kind, GovernorActionKind.reseed)
        XCTAssertEqual(gov.step(framesBehind: 64, nowMs: 1050).kind, GovernorActionKind.snapshot)
        XCTAssertEqual(gov.step(framesBehind: 64, nowMs: 1300).kind, GovernorActionKind.reseed)
        XCTAssertEqual(gov.reseeds, 2)
    }

    func testG4IdentityStableRecordAndCounters() {
        let gov = FreshnessGovernor()
        let a1 = gov.step(framesBehind: 0, nowMs: 0)
        for i in 0..<1_000_000 {
            gov.step(framesBehind: Int64(i % 128), nowMs: Int64(i))
        }
        let a2 = gov.step(framesBehind: 0, nowMs: 2_000_000)
        XCTAssertTrue(a1 === a2, "G4 identity-stable record")
        XCTAssertEqual(gov.steps, 1_000_002)
    }

    func testLaw4DecidedDropsDistinctFromRingCounters() {
        let gov = FreshnessGovernor()
        // behind 2 -> Skip(1); behind 4 -> Skip(3): 1+3 = 4 decided drops.
        gov.step(framesBehind: 2, nowMs: 0)
        gov.step(framesBehind: 4, nowMs: 1)
        XCTAssertEqual(gov.decidedDrops, 4)
        // A Snapshot and a FastPath add nothing.
        gov.step(framesBehind: 8, nowMs: 2)
        gov.step(framesBehind: 0, nowMs: 3)
        XCTAssertEqual(gov.decidedDrops, 4)
    }

    func testCustomLadderAndReset() {
        let cfg = GovernorConfig(fastPathBehind: 0, skipBehind: 2,
                                 snapshotBehind: 8, reseedCooldownMs: 100)
        let gov = FreshnessGovernor(config: cfg)
        XCTAssertEqual(gov.step(framesBehind: 0, nowMs: 0).kind, GovernorActionKind.fastPath)
        XCTAssertEqual(gov.step(framesBehind: 1, nowMs: 1).kind, GovernorActionKind.skip)
        XCTAssertEqual(gov.step(framesBehind: 1, nowMs: 1).skipN, 1)
        XCTAssertEqual(gov.step(framesBehind: 3, nowMs: 2).kind, GovernorActionKind.snapshot)
        XCTAssertEqual(gov.step(framesBehind: 9, nowMs: 3).kind, GovernorActionKind.reseed)
        XCTAssertEqual(gov.step(framesBehind: 9, nowMs: 50).kind, GovernorActionKind.snapshot) // suppressed
        gov.reset()
        XCTAssertEqual(gov.steps, 0)
        XCTAssertEqual(gov.reseeds, 0)
        XCTAssertEqual(gov.step(framesBehind: 0, nowMs: 0).kind, GovernorActionKind.fastPath)
    }

    func testG5LocalLadderTraceHashPin() {
        // The canonical (behind, nowMs) trace, packed identically to
        // fixtures/xlang-governor: byte = (kind << 6) | min(skipN, 63).
        // Hash pinned from the TS reference.
        let gov = FreshnessGovernor()
        var bytes = [UInt8](repeating: 0, count: 10_000)
        var state = Int64(0x00c0ffee)
        for i in 0..<10_000 {
            state = xorshift32(state)
            let behind = state % 128
            let a = gov.step(framesBehind: behind, nowMs: Int64(i))
            bytes[i] = UInt8((Int(a.kind) << 6) | min(Int(a.skipN), 63))
        }
        XCTAssertEqual(fnv1a64(bytes), 0x3c33156204c7cfdf,
                       "G5 local trace hash (TS reference pin)")
    }

    // ------------------------------------------------------------------
    // PC-series — the cadence policies
    // ------------------------------------------------------------------

    func testPC1LatestWinsPresentsIffSeqAdvanced() {
        let p = CadencePolicy(policy: CadencePolicyKind.latestWins)
        let seqs: [Int64] = [0, 1, 1, 4, 4, 4, 5]
        let expectPresent = [false, true, false, true, false, false, true]
        let expectCoalesced: [Int64] = [0, 0, 0, 2, 0, 0, 0]
        var presented: [Int64] = []
        for (i, s) in seqs.enumerated() {
            let a = p.step(latestSeq: s)
            XCTAssertEqual(a.present, expectPresent[i], "present@\(i)")
            XCTAssertEqual(a.coalesced, expectCoalesced[i], "coalesced@\(i)")
            XCTAssertFalse(a.interp)
            if a.present { presented.append(a.presentSeq) }
        }
        XCTAssertEqual(presented, [1, 4, 5])
        let held = p.step(latestSeq: 5)
        XCTAssertFalse(held.present)
        XCTAssertEqual(held.presentSeq, 5)
    }

    func testPC5Paced30On120PresentsEveryTickAfterWarmup() {
        let p = CadencePolicy(policy: CadencePolicyKind.pacedInterpolate)
        var latest: Int64 = 0
        var presents: Int64 = 0
        for t in 1...400 {
            if t % 4 == 1 { latest += 1 } // 30 Hz on a 120 Hz ticker
            let a = p.step(latestSeq: latest)
            XCTAssertTrue(a.interp)
            if a.present { presents += 1 }
        }
        // First window period 1 saturates at tick 2 (ticks 3-4 elide);
        // from tick 5 the ladder presents EVERY tick: 2 + 396 = 398 of 400.
        XCTAssertEqual(presents, 398)
        XCTAssertEqual(p.presents, presents)
    }

    func testPC5Paced240On120PresentsEveryTickOnePeriodBehind() {
        let p = CadencePolicy(policy: CadencePolicyKind.pacedInterpolate)
        var latest: Int64 = 0
        for _ in 1...400 {
            latest += 2
            let a = p.step(latestSeq: latest)
            XCTAssertTrue(a.present)
            XCTAssertEqual(a.alphaQ12, 0)
            XCTAssertEqual(a.presentSeq, latest)
        }
        XCTAssertEqual(p.presents, 400)
    }

    func testPC5PacedStallSaturatesAndElidesNeverExtrapolates() {
        let p = CadencePolicy(policy: CadencePolicyKind.pacedInterpolate)
        p.step(latestSeq: 1)
        p.step(latestSeq: 2)
        let a3 = p.step(latestSeq: 2)
        XCTAssertTrue(a3.present)
        XCTAssertEqual(a3.alphaQ12, cadenceAlphaOneQ12)
        let a4 = p.step(latestSeq: 2)
        XCTAssertFalse(a4.present)
        XCTAssertEqual(a4.alphaQ12, cadenceAlphaOneQ12)
        for _ in 0..<200 { p.step(latestSeq: 2) }
        XCTAssertEqual(p.interpFrames, 0) // endpoints only — zero true blends
    }

    func testPC6PacedAlphaLadderIsThePeriodLadder() {
        let p = CadencePolicy(policy: CadencePolicyKind.pacedInterpolate)
        var alphas: [Int32] = []
        for t in 1...8 {
            let latest: Int64
            if t == 1 { latest = 1 } else if t == 5 { latest = 2 } else { latest = t <= 4 ? 1 : 2 }
            let a = p.step(latestSeq: latest)
            if t >= 5 { alphas.append(a.alphaQ12) }
        }
        XCTAssertEqual(alphas, [0, 1024, 2048, 3072])
    }

    func testPC5Burst30On120LocksKOnTheContentBeat() {
        let p = CadencePolicy(policy: CadencePolicyKind.burstCoalesce)
        var latest: Int64 = 0
        var settled: [Int32] = []
        for t in 1...400 {
            if t % 4 == 1 { latest += 1 }
            let a = p.step(latestSeq: latest)
            if t > 200 { settled.append(a.k) }
        }
        XCTAssertTrue(settled.allSatisfy { $0 == 4 }, "K locked at 4 (last 200 ticks)")
    }

    func testPC5Burst240On120DegradesToNewestWins() {
        let p = CadencePolicy(policy: CadencePolicyKind.burstCoalesce)
        var latest: Int64 = 0
        var settled: [Int32] = []
        var presents: Int64 = 0
        for t in 1...400 {
            latest += 2
            let a = p.step(latestSeq: latest)
            if a.present { presents += 1 }
            if t > 200 { settled.append(a.k) }
        }
        XCTAssertTrue(settled.allSatisfy { $0 == 1 })
        XCTAssertEqual(presents, 400)
        XCTAssertEqual(p.coalescedByDecision, 400)
    }

    func testPC5BurstFeedPacesOnePresentPerBurst() {
        let p = CadencePolicy(policy: CadencePolicyKind.burstCoalesce)
        var latest: Int64 = 0
        var presents: Int64 = 0
        var coalescedSeen: Int64 = 0
        for t in 1...400 {
            if t % 10 == 1 { latest += 24 } // 24-frame burst every 10 ticks
            let a = p.step(latestSeq: latest)
            if a.present {
                presents += 1
                coalescedSeen += a.coalesced
            }
        }
        XCTAssertTrue((38...41).contains(presents), "presents per burst in 38..41 (got \(presents))")
        XCTAssertEqual(coalescedSeen, 960 - presents)
        XCTAssertEqual(p.missedPresentTicks + p.elided + presents, 400)
    }

    func testPC5BurstStallFreezesCadenceRecoveryImmediate() {
        let p = CadencePolicy(policy: CadencePolicyKind.burstCoalesce)
        var latest: Int64 = 0
        for _ in 1...200 {
            latest += 2
            p.step(latestSeq: latest)
        }
        let before = p.presents
        for _ in 1...100 { p.step(latestSeq: latest) }
        XCTAssertEqual(p.presents, before) // nothing newer -> no presents
        XCTAssertEqual(p.missedPresentTicks, 100)
        latest += 5
        let a = p.step(latestSeq: latest)
        XCTAssertTrue(a.present)
        XCTAssertEqual(a.coalesced, 4)
    }

    func testPC2TelescopingIdentitiesOverVolatileTraces() {
        // LATEST_WINS / BURST: sum(coalesced) == lastPresentedSeq - presents
        for kind in [CadencePolicyKind.latestWins, CadencePolicyKind.burstCoalesce] {
            let p = CadencePolicy(policy: kind)
            var state = Int64(0x1234abcd)
            var latest: Int64 = 0
            for _ in 0..<10_000 {
                state = xorshift32(state)
                latest += state % 5
                p.step(latestSeq: latest)
            }
            XCTAssertEqual(p.coalescedByDecision,
                           p.lastPresentedSeqForTest() - p.presents,
                           "PC2 kind=\(kind)")
        }
        // PACED: sum(coalesced) == newestSeq - arrivalTicks
        do {
            let p = CadencePolicy(policy: CadencePolicyKind.pacedInterpolate)
            var state = Int64(0xfeedface)
            var latest: Int64 = 0
            for _ in 0..<10_000 {
                state = xorshift32(state)
                latest += state % 5
                p.step(latestSeq: latest)
            }
            XCTAssertEqual(p.coalescedByDecision,
                           p.newestSeqForTest() - p.arrivalTicks,
                           "PC2 PACED")
        }
    }

    func testPC4CadenceIdentityStableRecord() {
        let p = CadencePolicy(policy: CadencePolicyKind.pacedInterpolate)
        let first = p.step(latestSeq: 1)
        var state = Int64(0xabcdef01)
        var latest: Int64 = 1
        for _ in 0..<10_000 {
            state = xorshift32(state)
            latest += state % 5
            let a = p.step(latestSeq: latest)
            XCTAssertTrue(a === first, "SAME object, mutated in place")
        }
    }

    func testPC6PolicySwitchMidTraceMonotonePresentSeq() {
        let p = CadencePolicy(policy: CadencePolicyKind.latestWins)
        var latest: Int64 = 0
        var lastPresented: Int64 = 0
        let kinds: [Int32] = [
            CadencePolicyKind.latestWins,
            CadencePolicyKind.pacedInterpolate,
            CadencePolicyKind.burstCoalesce,
            CadencePolicyKind.latestWins,
        ]
        for i in 0..<4000 {
            if i % 1000 == 0 { p.reset(policy: kinds[i / 1000]) }
            latest += (i % 3 == 0 ? 1 : 0) + (i % 7 == 0 ? 2 : 0)
            let a = p.step(latestSeq: latest)
            if a.present && !a.interp {
                XCTAssertTrue(a.presentSeq >= lastPresented, "PC6 monotone at i=\(i)")
                lastPresented = a.presentSeq
            }
        }
        XCTAssertTrue(p.presents > 0)
    }

    func testPC3LocalCadenceTraceHashPin() {
        // The canonical arrival trace, packed identically to
        // fixtures/xlang-cadence: per tick, 3 policies in kind order, each
        // 2 bytes: b1 = present<<7 | interp<<6 | alphaQ12>>7,
        // b2 = min(coalesced, 255). Hash pinned from the TS reference.
        let pols = [
            CadencePolicy(policy: CadencePolicyKind.latestWins),
            CadencePolicy(policy: CadencePolicyKind.pacedInterpolate),
            CadencePolicy(policy: CadencePolicyKind.burstCoalesce),
        ]
        var bytes = [UInt8](repeating: 0, count: 60_000)
        var state = Int64(0x00c0ffee)
        var latest: Int64 = 0
        var out = 0
        for _ in 0..<10_000 {
            state = xorshift32(state)
            latest += state % 5
            for pol in pols {
                let a = pol.step(latestSeq: latest)
                let b1 = (a.present ? 1 : 0) << 7 | (a.interp ? 1 : 0) << 6 | Int(a.alphaQ12 >> 7)
                bytes[out] = UInt8(b1)
                out += 1
                bytes[out] = UInt8(min(a.coalesced, 255))
                out += 1
            }
        }
        XCTAssertEqual(fnv1a64(bytes), 0x6f654c298cbcc9f4,
                       "PC3 local trace hash (TS reference pin)")
    }
}
