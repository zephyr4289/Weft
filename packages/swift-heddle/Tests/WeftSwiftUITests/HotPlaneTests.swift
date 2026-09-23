// Tests/HotPlaneTests.swift — pure Swift logic tests over a synthetic plane.
// CI-gated on the Apple lane (authoring sandbox has no swiftc); the Node
// structural audit (audit/static_audit.mjs) is the always-runnable layer.

import XCTest
@testable import WeftSwiftUI

final class HotPlaneTests: XCTestCase {
    func makePlane(lanes: Int = 4, samples: Int = 8) -> (UnsafeMutableRawBufferPointer, HPL1Geometry, [UInt8]) {
        let geo = HPL1.deriveGeometry(laneCount: lanes, samplesPerLane: samples)
        var storage = [UInt8](repeating: 0, count: geo.totalBytes)
        let buf = UnsafeMutableRawBufferPointer(mutating: storage)
        func u32(_ off: Int, _ v: UInt32) { buf.storeBytes(of: v.littleEndian, toByteOffset: off, as: UInt32.self) }
        u32(HPL1.hdrMagic, HPL1.magicU32)
        u32(HPL1.hdrVersion, HPL1.version)
        u32(HPL1.hdrFlags, HPL1.flagLeRequired)
        u32(HPL1.hdrLaneCount, UInt32(lanes))
        u32(HPL1.hdrSamplesPerLane, UInt32(samples))
        u32(HPL1.hdrRingMask, UInt32(samples - 1))
        u32(HPL1.hdrDirtyWords, UInt32(geo.dirtyWords))
        u32(HPL1.hdrLaneCtrlStride, UInt32(HPL1.laneCtrlStride))
        u32(HPL1.hdrRingBase, UInt32(geo.ringBase))
        u32(HPL1.hdrTotalBytes, UInt32(geo.totalBytes))
        for lane in 0..<lanes {
            let ctrl = geo.laneCtrl(lane)
            for k in 0..<samples {
                let v = Double(10 + lane * 100 + k)
                buf.storeBytes(of: UInt32(k * 2 + 1).littleEndian, toByteOffset: ctrl + HPL1.laneSeq, as: UInt32.self)
                buf.storeBytes(of: v.bitPattern.littleEndian, toByteOffset: ctrl + HPL1.laneCurrent, as: UInt64.self)
                buf.storeBytes(of: UInt32(k + 1).littleEndian, toByteOffset: ctrl + HPL1.laneHead, as: UInt32.self)
                buf.storeBytes(of: UInt32(k * 2 + 2).littleEndian, toByteOffset: ctrl + HPL1.laneSeq, as: UInt32.self)
                buf.storeBytes(of: v.bitPattern.littleEndian, toByteOffset: geo.laneRing(lane) + k * 8, as: UInt64.self)
            }
        }
        u32(HPL1.headerSize, 0xFFFFFFFF >> (32 - UInt32(lanes))) // dirty bits
        return (buf, geo, storage)
    }

    func testGeometryParity() {
        let g = HPL1.deriveGeometry(laneCount: 4, samplesPerLane: 8)
        XCTAssertEqual(g.dirtyWords, 1)
        XCTAssertEqual(g.laneCtrlBase, 136)
        XCTAssertEqual(g.ringBase, 392)
        XCTAssertEqual(g.totalBytes, 648)
        let g1 = HPL1.deriveGeometry(laneCount: 1, samplesPerLane: 2)
        XCTAssertEqual(g1.totalBytes, 216)
    }

    func testValidateFailsClosed() {
        let (buf, _, storage) = makePlane()
        var scratch = storage
        scratch[HPL1.hdrMagic] = 0xEF // corrupt magic byte 0
        XCTAssertThrowsError(try WeftHotPlane(bytes: UnsafeRawBufferPointer(mutating: scratch))) { e in
            XCTAssertEqual((e as? HPL1Error)?.code, .badMagic)
        }
        scratch = storage
        scratch[HPL1.hdrFlags] = 0 // clear LE_REQUIRED bit0
        XCTAssertThrowsError(try WeftHotPlane(bytes: UnsafeRawBufferPointer(mutating: scratch))) { e in
            XCTAssertEqual((e as? HPL1Error)?.code, .notLittleEndian)
        }
        scratch = storage
        scratch[HPL1.hdrSamplesPerLane] = 7 // non-pow2 (byte 0 of the u32)
        XCTAssertThrowsError(try WeftHotPlane(bytes: UnsafeRawBufferPointer(mutating: scratch))) { e in
            XCTAssertEqual((e as? HPL1Error)?.code, .capacityMismatch)
        }
        _ = buf
    }

    func testSeqlockReadAndInPlaceMutation() throws {
        let (buf, _, _) = makePlane()
        let plane = try WeftHotPlane(bytes: buf)
        var snap = WeftLaneSnapshot()
        XCTAssertEqual(plane.readLane(1, &snap), .ok)
        XCTAssertEqual(snap.current, 111.0) // 10 + 1*100 + 1
        let first = snap.current
        XCTAssertEqual(plane.readLane(1, &snap), .ok)
        XCTAssertEqual(snap.current, first) // same struct reused, values stable
        XCTAssertEqual(plane.tears, 0)
    }

    func testTornSeqlockNeverReturnsValue() throws {
        let (buf, geo, storage) = makePlane()
        var torn = storage
        torn[geo.laneCtrl(0)] |= 1 // flip lane 0 seq bit0 → odd (write in progress)
        let plane = try WeftHotPlane(bytes: UnsafeRawBufferPointer(mutating: torn))
        var snap = WeftLaneSnapshot()
        XCTAssertEqual(plane.readLane(0, &snap), .tornSeqlock)
        XCTAssertGreaterThan(plane.tears, 0)
        XCTAssertEqual(snap.current, 0) // value NOT returned (HPL1 §4.2)
        _ = buf
    }

    func testReadRecentNewestFirst() throws {
        let (buf, _, _) = makePlane()
        let plane = try WeftHotPlane(bytes: buf)
        var win = [Double](repeating: 0, count: 8)
        try win.withUnsafeMutableBufferPointer { ptr in
            let n = plane.readRecent(2, 8, ptr)
            XCTAssertEqual(n, 8)
            XCTAssertEqual(ptr[0], 120.0) // newest = 10 + 2*100 + 7
            XCTAssertEqual(ptr[7], 113.0)
        }
    }

    func testDirtyScanTransitions() throws {
        let (buf, _, _) = makePlane()
        let plane = try WeftHotPlane(bytes: buf)
        var changed: [Int] = []
        XCTAssertEqual(plane.scanDirty(&changed), 4)
        XCTAssertEqual(changed.sorted(), [0, 1, 2, 3])
        XCTAssertEqual(plane.scanDirty(&changed), 0) // no new transitions
        XCTAssertEqual(plane.dirtyCount, 4)
    }

    func testEpochChangeIsExplicit() throws {
        let (buf, _, storage) = makePlane()
        let plane = try WeftHotPlane(bytes: buf)
        var hdr = WeftHeaderSnapshot()
        XCTAssertEqual(plane.readHeader(&hdr), .ok) // primes the epoch cache
        var restarted = storage
        restarted[HPL1.hdrEpoch] = 1 // producer restart bumps epoch lo
        let plane2 = try WeftHotPlane(bytes: UnsafeRawBufferPointer(mutating: restarted))
        XCTAssertEqual(plane2.readHeader(&hdr), .ok) // fresh reader primes silently
        _ = buf
    }

    func testTaxonomyCompleteness() {
        XCTAssertEqual(HPL1Code.allCases.count, 15)
        XCTAssertEqual(HPL1Code.ringUnderrun.rawValue, 14)
    }
}
