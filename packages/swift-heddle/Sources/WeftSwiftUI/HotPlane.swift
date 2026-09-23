// HotPlane.swift — HPL1 constants, geometry, and the seqlock consumer over
// POSIX shared memory (or any mapped region). Byte-parity with
// packages/heddle-core/src/layout.js (audited by Tests/ConstantsParity + the
// Node structural audit in audit/static_audit.mjs).
//
// Laws:
//   1  zero allocation on the hot path — readLane fills caller-owned
//      snapshots; no arrays/objects are created per frame
//   2  every multi-byte load is EXPLICIT little-endian
//      (UInt32/UInt64/Double(littleEndian:))
//   3  managed-side only — core/c/weft.{c,h} untouched
//   4  tears/epoch changes are coded outcomes, never silent

import Foundation

/// HPL1 error taxonomy (stable numeric codes — mirrors errors.js).
public enum HPL1Code: Int32, CaseIterable, Sendable {
    case ok = 0
    case badMagic = 1
    case badVersion = 2
    case notLittleEndian = 3
    case capacityMismatch = 4
    case laneOutOfRange = 5
    case tornSeqlock = 6
    case epochChanged = 7
    case planeDetached = 8
    case contextLost = 9
    case tabHidden = 10
    case workerCrash = 11
    case badRenderEngine = 12
    case invalidSample = 13
    case ringUnderrun = 14
}

public struct HPL1 {
    public static let headerSize = 128
    public static let laneCtrlStride = 64
    public static let magicU32: UInt32 = 0x314C_5048 // 'H','P','L','1' LE
    public static let version: UInt32 = 1
    public static let flagLeRequired: UInt32 = 1
    public static let laneFlagActive: UInt32 = 1
    public static let laneFlagManual: UInt32 = 2
    public static let maxLanes = 4096

    // header offsets
    public static let hdrMagic = 0x00
    public static let hdrVersion = 0x04
    public static let hdrFlags = 0x08
    public static let hdrLaneCount = 0x0C
    public static let hdrSamplesPerLane = 0x10
    public static let hdrRingMask = 0x14
    public static let hdrTickHz = 0x18
    public static let hdrReserved0 = 0x1C
    public static let hdrEpoch = 0x20
    public static let hdrPublishSeq = 0x28
    public static let hdrLastPublishNs = 0x30
    public static let hdrFramesDropped = 0x38
    public static let hdrGlobalMin = 0x40
    public static let hdrGlobalMax = 0x48
    public static let hdrGlobalAvg = 0x50
    public static let hdrGlobalCurrent = 0x58
    public static let hdrDirtyWords = 0x60
    public static let hdrLaneCtrlStride = 0x64
    public static let hdrRingBase = 0x68
    public static let hdrTotalBytes = 0x6C

    // lane control block offsets
    public static let laneSeq = 0x00
    public static let laneCurrent = 0x08
    public static let laneMin = 0x10
    public static let laneMax = 0x18
    public static let laneAvg = 0x20
    public static let laneSamplesSeen = 0x28
    public static let laneHead = 0x30
    public static let laneFlags = 0x34
    public static let lanePublishNs = 0x38
    public static let laneDrops = 0x40

    static func align8(_ x: Int) -> Int { (x + 7) & ~7 }
    static func isPow2(_ x: Int) -> Bool { x >= 2 && (x & (x - 1)) == 0 }

    public static func deriveGeometry(laneCount: Int, samplesPerLane: Int) -> HPL1Geometry {
        let dirtyWords = (laneCount + 31) / 32
        let laneCtrlBase = headerSize + align8(4 * dirtyWords)
        let ringBase = align8(laneCtrlBase + laneCtrlStride * laneCount)
        let totalBytes = ringBase + laneCount * samplesPerLane * 8
        return HPL1Geometry(dirtyWords: dirtyWords, laneCtrlBase: laneCtrlBase,
                            ringBase: ringBase, totalBytes: totalBytes,
                            laneCount: laneCount, samplesPerLane: samplesPerLane)
    }

    static func validate(_ bytes: UnsafeRawBufferPointer, byteOffset: Int = 0) throws -> HPL1Geometry {
        let available = bytes.count - byteOffset
        guard available >= headerSize else { throw HPL1Error(.planeDetached, "buffer too small") }
        let base = byteOffset
        func u32(_ off: Int) -> UInt32 { bytes.loadUnaligned(fromByteOffset: base + off, as: UInt32.self).littleEndian }
        guard u32(hdrMagic) == magicU32 else { throw HPL1Error(.badMagic, "magic mismatch") }
        guard u32(hdrVersion) == version else { throw HPL1Error(.badVersion, "version mismatch") }
        guard u32(hdrFlags) & flagLeRequired != 0 else { throw HPL1Error(.notLittleEndian, "LE_REQUIRED clear") }
        let laneCount = Int(u32(hdrLaneCount))
        let samplesPerLane = Int(u32(hdrSamplesPerLane))
        guard (1...maxLanes).contains(laneCount) else { throw HPL1Error(.capacityMismatch, "laneCount range") }
        guard isPow2(samplesPerLane) else { throw HPL1Error(.capacityMismatch, "samplesPerLane not pow2") }
        guard u32(hdrRingMask) == UInt32(samplesPerLane - 1) else { throw HPL1Error(.capacityMismatch, "ringMask drift") }
        let geo = deriveGeometry(laneCount: laneCount, samplesPerLane: samplesPerLane)
        guard u32(hdrDirtyWords) == UInt32(geo.dirtyWords),
              u32(hdrLaneCtrlStride) == UInt32(laneCtrlStride),
              u32(hdrRingBase) == UInt32(geo.ringBase),
              u32(hdrTotalBytes) == UInt32(geo.totalBytes) else {
            throw HPL1Error(.capacityMismatch, "derived vs stored drift")
        }
        guard available >= geo.totalBytes else { throw HPL1Error(.planeDetached, "buffer smaller than plane") }
        return geo
    }
}

public struct HPL1Geometry: Sendable {
    public let dirtyWords: Int
    public let laneCtrlBase: Int
    public let ringBase: Int
    public let totalBytes: Int
    public let laneCount: Int
    public let samplesPerLane: Int
    public func laneCtrl(_ lane: Int) -> Int { laneCtrlBase + lane * HPL1.laneCtrlStride }
    public func laneRing(_ lane: Int) -> Int { ringBase + lane * samplesPerLane * 8 }
}

public struct HPL1Error: Error, Sendable {
    public let code: HPL1Code
    public let detail: String
    public init(_ code: HPL1Code, _ detail: String) { self.code = code; self.detail = detail }
}

/// Caller-owned lane snapshot (allocate once, reuse — Law 1).
public struct WeftLaneSnapshot {
    public var seqLo: UInt32 = 0, seqHi: UInt32 = 0
    public var current: Double = 0, min: Double = 0, max: Double = 0, avg: Double = 0
    public var samplesSeenLo: UInt32 = 0, samplesSeenHi: UInt32 = 0
    public var head: UInt32 = 0, flags: UInt32 = 0
    public var publishNsLo: UInt32 = 0, publishNsHi: UInt32 = 0
    public var drops: UInt32 = 0
    public var samplesSeen: UInt64 { UInt64(samplesSeenLo) | (UInt64(samplesSeenHi) << 32) }
}

public struct WeftHeaderSnapshot {
    public var publishSeqLo: UInt32 = 0, publishSeqHi: UInt32 = 0
    public var epochLo: UInt32 = 0, epochHi: UInt32 = 0
    public var lastPublishNsLo: UInt32 = 0, lastPublishNsHi: UInt32 = 0
    public var globalMin: Double = 0, globalMax: Double = 0, globalAvg: Double = 0, globalCurrent: Double = 0
    public var tickHz: UInt32 = 0, flags: UInt32 = 0
}

/// WeftHotPlane — seqlock consumer over mapped memory. NOT Sendable: each
/// thread owns its instance (tears/epoch caches are per-reader by design).
public final class WeftHotPlane {
    public let bytes: UnsafeRawBufferPointer
    public let ownsMemory: Bool
    public let geo: HPL1Geometry
    public let maxTries: Int
    public internal(set) var tears: Int = 0
    var lastEpochLo: UInt32 = .max // .max ≡ "primed" sentinel (0xFFFFFFFF never a real epoch-lo start)
    var lastEpochHi: UInt32 = .max
    var lastMask: [UInt32]
    public internal(set) var dirtyCount: Int = 0

    /// Attach an existing mapped region (Engineer 1's bridge / POSIX shm).
    /// The buffer is NOT copied and NOT freed by this class (ownsMemory=false).
    public init(bytes: UnsafeRawBufferPointer, byteOffset: Int = 0, maxTries: Int = 64) throws {
        self.bytes = UnsafeRawBufferPointer(rebasing: bytes[byteOffset...])
        self.ownsMemory = false
        self.maxTries = maxTries
        self.geo = try HPL1.validate(self.bytes)
        self.lastMask = Array(repeating: 0, count: geo.dirtyWords)
    }

    deinit {
        // ownership of externally-provided memory stays with the caller
    }

    var laneCount: Int { geo.laneCount }
    var samplesPerLane: Int { geo.samplesPerLane }

    func loadSeqLo(_ byteOff: Int) -> UInt32 {
        bytes.loadUnaligned(fromByteOffset: byteOff, as: UInt32.self).littleEndian
    }

    /// Header seqlock acquire → fills `out` in place. Returns .ok,
    /// .epochChanged (out still filled) or .tornSeqlock (out untouched).
    @discardableResult
    public func readHeader(_ out: inout WeftHeaderSnapshot) -> HPL1Code {
        for _ in 0..<maxTries {
            let s1 = loadSeqLo(HPL1.hdrPublishSeq)
            if s1 & 1 != 0 { tears += 1; continue }
            out.publishSeqLo = s1
            out.publishSeqHi = bytes.loadUnaligned(fromByteOffset: HPL1.hdrPublishSeq + 4, as: UInt32.self).littleEndian
            out.epochLo = bytes.loadUnaligned(fromByteOffset: HPL1.hdrEpoch, as: UInt32.self).littleEndian
            out.epochHi = bytes.loadUnaligned(fromByteOffset: HPL1.hdrEpoch + 4, as: UInt32.self).littleEndian
            out.lastPublishNsLo = bytes.loadUnaligned(fromByteOffset: HPL1.hdrLastPublishNs, as: UInt32.self).littleEndian
            out.globalMin = bytes.loadUnaligned(fromByteOffset: HPL1.hdrGlobalMin, as: UInt64.self).littleEndian.bitPattern
            out.globalMax = bytes.loadUnaligned(fromByteOffset: HPL1.hdrGlobalMax, as: UInt64.self).littleEndian.bitPattern
            out.globalAvg = bytes.loadUnaligned(fromByteOffset: HPL1.hdrGlobalAvg, as: UInt64.self).littleEndian.bitPattern
            out.globalCurrent = bytes.loadUnaligned(fromByteOffset: HPL1.hdrGlobalCurrent, as: UInt64.self).littleEndian.bitPattern
            out.tickHz = bytes.loadUnaligned(fromByteOffset: HPL1.hdrTickHz, as: UInt32.self).littleEndian
            out.flags = bytes.loadUnaligned(fromByteOffset: HPL1.hdrFlags, as: UInt32.self).littleEndian
            let s2 = loadSeqLo(HPL1.hdrPublishSeq)
            if s1 != s2 { tears += 1; continue }
            if out.epochLo != lastEpochLo || out.epochHi != lastEpochHi {
                let first = lastEpochLo == .max && lastEpochHi == .max
                lastEpochLo = out.epochLo
                lastEpochHi = out.epochHi
                if !first { return .epochChanged }
            }
            return .ok
        }
        return .tornSeqlock
    }

    /// Lane seqlock acquire → fills `out` in place. .ok | .tornSeqlock.
    @discardableResult
    public func readLane(_ lane: Int, _ out: inout WeftLaneSnapshot) -> HPL1Code {
        guard lane >= 0 && lane < geo.laneCount else {
            return .laneOutOfRange // caller error surfaced as a typed code
        }
        let ctrl = geo.laneCtrl(lane)
        for _ in 0..<maxTries {
            let s1 = loadSeqLo(ctrl + HPL1.laneSeq)
            if s1 & 1 != 0 { tears += 1; continue }
            out.seqLo = s1
            out.seqHi = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.laneSeq + 4, as: UInt32.self).littleEndian
            out.current = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.laneCurrent, as: UInt64.self).littleEndian.bitPattern
            out.min = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.laneMin, as: UInt64.self).littleEndian.bitPattern
            out.max = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.laneMax, as: UInt64.self).littleEndian.bitPattern
            out.avg = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.laneAvg, as: UInt64.self).littleEndian.bitPattern
            out.samplesSeenLo = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.laneSamplesSeen, as: UInt32.self).littleEndian
            out.samplesSeenHi = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.laneSamplesSeen + 4, as: UInt32.self).littleEndian
            out.head = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.laneHead, as: UInt32.self).littleEndian
            out.flags = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.laneFlags, as: UInt32.self).littleEndian
            out.publishNsLo = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.lanePublishNs, as: UInt32.self).littleEndian
            out.drops = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.laneDrops, as: UInt32.self).littleEndian
            let s2 = loadSeqLo(ctrl + HPL1.laneSeq)
            if s1 != s2 { tears += 1; continue }
            return .ok
        }
        return .tornSeqlock
    }

    /// Copy the k NEWEST samples into `out` (index 0 = newest).
    /// Returns count written (< k ⇒ underrun), or -1 when torn.
    public func readRecent(_ lane: Int, _ k: Int, _ out: UnsafeMutableBufferPointer<Double>) -> Int {
        guard lane >= 0 && lane < geo.laneCount else { return -2 }
        let ctrl = geo.laneCtrl(lane)
        let ringByte = geo.laneRing(lane)
        let mask = geo.samplesPerLane - 1
        for _ in 0..<maxTries {
            let s1 = loadSeqLo(ctrl + HPL1.laneSeq)
            if s1 & 1 != 0 { tears += 1; continue }
            let head = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.laneHead, as: UInt32.self).littleEndian
            let seen = bytes.loadUnaligned(fromByteOffset: ctrl + HPL1.laneSamplesSeen, as: UInt32.self).littleEndian
            let s2 = loadSeqLo(ctrl + HPL1.laneSeq)
            if s1 != s2 { tears += 1; continue }
            let avail = Int(min(UInt(seen), UInt(geo.samplesPerLane)))
            let n = min(k, avail)
            for j in 0..<n {
                // NOTE: Swift's `&` binds LOOSER than `*` — mask first, then scale
                let slot = ((Int(head) - 1 - j) & mask) * 8
                let bits = bytes.loadUnaligned(fromByteOffset: ringByte + slot, as: UInt64.self).littleEndian
                out[j] = bits.bitPattern
            }
            return n
        }
        return -1
    }

    /// Dirty-mask transition scan; newly-dirty lanes returned (HPL1 §3 —
    /// consumers NEVER write the mask).
    public func scanDirty(_ changedOut: inout [Int]) -> Int {
        changedOut.removeAll(keepingCapacity: true)
        var count = 0
        for w in 0..<geo.dirtyWords {
            let cur = bytes.loadUnaligned(fromByteOffset: HPL1.headerSize + w * 4, as: UInt32.self).littleEndian
            let fresh = cur & ~lastMask[w]
            lastMask[w] = cur
            if fresh != 0 {
                var bits = fresh
                while bits != 0 {
                    let bit = bits & (~bits &+ 1)
                    let lane = w * 32 + bit.trailingZeroBitCount
                    changedOut.append(lane)
                    bits &= bits &- 1
                }
            }
            count += cur.nonzeroBitCount
        }
        dirtyCount = count
        return changedOut.count
    }
}
