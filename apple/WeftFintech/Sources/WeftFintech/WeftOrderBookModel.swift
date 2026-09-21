// WeftOrderBookModel.swift — edge-triggered @Observable order book model.
//
// The view layer reads PREALLOCATED level scratch; a new MDP1 record
// publishes ONLY when its seq advanced past the last published one
// (edge-trigger), so a 240 Hz feed cannot storm SwiftUI observation.
// The decode itself never allocates: all reads are flyweight loads.

import Foundation
import Observation

@MainActor
@Observable
public final class WeftOrderBookModel {
    // Observation-facing state — written ONLY inside publish().
    public private(set) var seq: UInt64 = 0
    public private(set) var lastTsNs: UInt64 = 0
    public private(set) var bestBid: UInt32 = 0
    public private(set) var bestAsk: UInt32 = 0
    public private(set) var bookValid = false
    public private(set) var crossed = false
    public private(set) var locked = false

    /// Preallocated level scratch: 0..9 bids, 10..19 asks, stride 3
    /// (price, size, orders). Geometry lives here; SwiftUI Canvas draws
    /// straight from it.
    public var levels = [UInt32](repeating: 0, count: Mdp1.topLevels * 2 * 3)

    public private(set) var lastSeq: UInt64 = 0
    private var primed = false

    public init() {}

    /// Decode + edge-triggered publish. Returns true when observers were
    /// invalidated (new seq). A structurally invalid record flips the
    /// model into FALLBACK (bookValid == false) exactly once.
    @discardableResult
    public func ingest(_ record: UnsafeRawBufferPointer) -> Bool {
        let view = Mdp1View(record)
        let code = view.validate()
        guard code == Mdp1.ok else {
            if !primed || bookValid {
                bookValid = false
                primed = true
                return true
            }
            return false
        }
        let s = view.seq
        if primed && s == lastSeq {
            return false // edge gate: no rebuild storms on repeated seq
        }
        seq = s
        lastTsNs = view.lastTsNs
        bestBid = view.bestBid
        bestAsk = view.bestAsk
        crossed = view.crossed
        locked = view.locked
        bookValid = true
        primed = true
        for i in 0..<Mdp1.topLevels {
            let b = i * 3, a = (Mdp1.topLevels + i) * 3
            levels[b] = view.bidPrice(i)
            levels[b + 1] = view.bidSize(i)
            levels[b + 2] = view.bidOrders(i)
            levels[a] = view.askPrice(i)
            levels[a + 1] = view.askSize(i)
            levels[a + 2] = view.askOrders(i)
        }
        lastSeq = s
        return true
    }
}
