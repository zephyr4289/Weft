// Steward.swift — Lifecycle manager for Wefts (Swift/iOS)
//
// WHY EXISTS: Manages the lifetime of Weft instances — allocates, binds to
// scope, frees on scope exit. Per 02-KERNEL §7 and WHITEPAPER §7.3:
// @StateObject-scoped (survives SwiftUI re-creation). Per docs/PORTS.md §2
// for the I6 mapping table (ARC: buffer set to nil, not free()).
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

import Foundation
import Atomics
import Combine

public final class Steward: ObservableObject {
    private var wefts: [Int: Weft] = [:]
    private var nextId: Int = 1
    private var released = false

    public init() {}

    public func weft(payloadMax: Int) -> Weft {
        precondition(!released, "Steward is released")
        let w = Weft(payloadMax: payloadMax)
        let id = nextId
        nextId += 1
        wefts[id] = w
        return w
    }

    public func releaseAll() {
        guard !released else { return }
        released = true
        wefts.removeAll() // ARC releases the Weft → deinit frees buffers
    }

    public func stats() -> StewardStats {
        var totalPub: UInt64 = 0
        var totalRead: UInt64 = 0
        for w in wefts.values {
            totalPub += w.tPublishCount()
            totalRead += w.tClaimCount()
        }
        return StewardStats(weftCount: wefts.count, totalPublishes: totalPub, totalReads: totalRead)
    }

    deinit { releaseAll() }
}

public struct StewardStats {
    public let weftCount: Int
    public let totalPublishes: UInt64
    public let totalReads: UInt64
}
