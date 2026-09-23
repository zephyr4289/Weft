// HotPlaneModel.swift — @Observable bridge for SwiftUI (charter §B).
//
// One pump() mutates stored properties IN PLACE. With Observation, SwiftUI
// views that read these properties re-render their tiny dependent slices —
// the model never allocates per pump, and the 240 Hz canvas path does not go
// through Observation at all (MetalHuddleView reads the plane directly).

import Foundation

@Observable
public final class HotPlaneModel {
    public private(set) var current: [Double]
    public private(set) var mins: [Double]
    public private(set) var maxs: [Double]
    public private(set) var avgs: [Double]
    public private(set) var globalMin: Double = 0
    public private(set) var globalMax: Double = 0
    public private(set) var globalAvg: Double = 0
    public private(set) var tears: Int = 0
    public private(set) var dirtyCount: Int = 0
    public private(set) var lastEvent: HPL1Code = .ok

    public let plane: WeftHotPlane
    public let lanes: [Int]
    private var scratch: WeftLaneSnapshot
    private var header: WeftHeaderSnapshot
    private var lastSeqs: [UInt64]
    private var changed: [Int] = []
    private var epochDirty = false

    public init(plane: WeftHotPlane, watch: [Int]? = nil) {
        self.plane = plane
        let l = watch ?? Array(0..<plane.laneCount)
        self.lanes = l
        self.current = Array(repeating: 0, count: l.count)
        self.mins = Array(repeating: 0, count: l.count)
        self.maxs = Array(repeating: 0, count: l.count)
        self.avgs = Array(repeating: 0, count: l.count)
        self.lastSeqs = Array(repeating: 0, count: l.count)
        self.scratch = WeftLaneSnapshot()
        self.header = WeftHeaderSnapshot()
    }

    /// One pump: seqlock-safe reads → in-place mutations → Observation emits
    /// only for lanes whose sequence advanced. No allocation in steady state
    /// (arrays keep capacity; snapshots are structs mutated in place).
    public func pump() {
        var advanced = false
        for i in 0..<lanes.count {
            let code = plane.readLane(lanes[i], &scratch)
            if code == .tornSeqlock { continue } // counted on plane (never silent)
            if code == .laneOutOfRange { lastEvent = code; continue }
            let seq = UInt64(scratch.seqLo) | (UInt64(scratch.seqHi) << 32)
            if seq != lastSeqs[i] {
                lastSeqs[i] = seq
                current[i] = scratch.current
                mins[i] = scratch.min
                maxs[i] = scratch.max
                avgs[i] = scratch.avg
                advanced = true
            }
        }
        let hcode = plane.readHeader(&header)
        if hcode == .epochChanged { epochDirty = true }
        globalMin = header.globalMin
        globalMax = header.globalMax
        globalAvg = header.globalAvg
        tears = plane.tears
        plane.scanDirty(&changed)
        dirtyCount = plane.dirtyCount
        if advanced || epochDirty {
            if epochDirty { lastEvent = .epochChanged; epochDirty = false }
        }
    }
}
