// Weft.swift — Triad Protocol kernel (Swift/iOS reference port)
//
// WHY EXISTS: Implements the corrected single-atomic-exchange Triad Protocol
// (RFC-0001 §4) for Swift. The exchange maps to ManagedAtomic.exchange
// (single RMW, .acquiringAndReleasing — exact equivalent of C AcqRel).
// Per 02-KERNEL §2 and docs/PORTS.md §2. Dependency: apple/swift-atomics
// (WO-P4 decision 3). Per 03-ENVELOPE §1: envelope via manual LE decode.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

import Foundation
import Atomics

/// Magic "WEFT" little-endian: 0x54464557
let WEFT_MAGIC: UInt32 = 0x54464557
let WEFT_VERSION_1: UInt16 = 1

/// Upper bound for payload_max (1 MiB) — the TIER4 §5 validation wall
/// (issue #19). Mirrors WEFT_PAYLOAD_MAX_LIMIT (core/c/weft.h).
let WEFT_PAYLOAD_MAX_LIMIT: Int = 1 << 20

/// Publish result (02 §4).
public enum PubResult { case ok, droppedRevoked, invalid }

/// Decode result (03-ENVELOPE §2).
public enum DecodeResult { case ok, short, badMagic, badHeader }

/// A single Weft: three buffers + the single shared atomic `latest`.
///
/// Per 02 §1: latest=0, w_work=1, r_work=2. Swift-atomics ordering
/// .acquiringAndReleasing = exact C AcqRel equivalent (decision 3).
public final class Weft {
    public let payloadMax: Int
    public let bufSize: Int

    // Three buffers
    private var buffers: [UnsafeMutableRawPointer] = []
    private var bufferDealloc: [(UnsafeMutableRawPointer, Int)] = []

    // The single shared atomic. .acquiringAndReleasing = C AcqRel.
    private let latest: ManagedAtomic<UInt32> = ManagedAtomic(0)

    // Writer-private (advisory; thread-private by contract)
    private var wWork: UInt32 = 1
    // Reader-private (advisory; thread-private by contract)
    private var rWork: UInt32 = 2

    // I6: writer revocation
    private let revoked: ManagedAtomic<Bool> = ManagedAtomic(false)
    private let epoch: ManagedAtomic<UInt32> = ManagedAtomic(0)

    // Telemetry (advisory per AXIOM T)
    private let tPublish = ManagedAtomic<UInt64>(0)
    private let tClaim = ManagedAtomic<UInt64>(0)
    private let tDrop = ManagedAtomic<UInt64>(0)
    private let tInvalid = ManagedAtomic<UInt64>(0)

    public init(payloadMax: Int) {
        // TIER4 §5 validation wall (issue #19): fail fast — an invalid triad
        // geometry is a programmer error in the VM port (the C kernel's -1
        // refusal maps to a precondition; never a half-built object).
        precondition(payloadMax > 0 && payloadMax <= 1 << 20,
                     "Weft: payloadMax must be in [1, \(1 << 20)] (TIER4 §5), got \(payloadMax)")
        self.payloadMax = payloadMax
        self.bufSize = ((16 + payloadMax + 8 + 63) / 64) * 64

        // Allocate 3 buffers, 64-byte aligned
        for _ in 0..<3 {
            let ptr = UnsafeMutableRawPointer.allocate(byteCount: bufSize, alignment: 64)
            memset(ptr, 0, bufSize)
            buffers.append(ptr)
            bufferDealloc.append((ptr, bufSize))

            // Per 04-LITMUS §0.6: null frame with pat(0,i) payload.
            envelopeEncodeV1(ptr, seq: 0, payloadLen: UInt32(payloadMax))
            for j in 0..<payloadMax {
                ptr.advanced(by: 16 + j).storeBytes(of: pat(seq: 0, i: UInt32(j)), as: UInt8.self)
            }
        }
    }

    /// destroy: API parity with C kernel (ARC handles real deallocation).
    public func destroy() { /* ARC deinit handles it */ }

    deinit {
        for (ptr, _) in bufferDealloc {
            ptr.deallocate()
        }
    }

    // --- Writer ---

    /// Write pointer for the writer's working buffer.
    public var wBegin: UnsafeMutableRawPointer {
        buffers[Int(wWork)].advanced(by: 16)
    }

    /// Publish: write envelope + canary, then exchange latest.
    /// Per 02 §2 + §6: revoked checked FIRST; exchange is THE atomic.
    public func publish(seq: UInt32, payloadLen: UInt32) -> PubResult {
        if revoked.load(ordering: .relaxed) {
            epoch.wrappingIncrement(by: 1, ordering: .acquiringAndReleasing)
            tDrop.wrappingIncrement(by: 1, ordering: .relaxed)
            return .droppedRevoked
        }

        if payloadLen < 0 || payloadLen > payloadMax {
            tInvalid += 1
            return .invalid
        }

        let buf = buffers[Int(wWork)]
        envelopeEncodeV1(buf, seq: seq, payloadLen: payloadLen)
        // Canary at buf_size-8 (u64 LE = seq)
        buf.advanced(by: bufSize - 8).storeBytes(of: UInt64(seq).littleEndian, as: UInt64.self)

        // THE atomic: latest.exchange(w_work, .acquiringAndReleasing)
        let old = latest.exchange(wWork, ordering: .acquiringAndReleasing)
        wWork = old

        tPublish.wrappingIncrement(by: 1, ordering: .relaxed)
        return .ok
    }

    // --- Reader ---

    /// Claim the freshest published buffer. NEVER fails.
    public func claim() -> UInt32 {
        let r = rWork
        let mine = latest.exchange(r, ordering: .acquiringAndReleasing)
        rWork = mine
        tClaim.wrappingIncrement(by: 1, ordering: .relaxed)
        return mine
    }

    public func rSeq() -> UInt32 {
        buffers[Int(rWork)].advanced(by: 8).loadUnaligned(as: UInt32.self).littleEndian
    }
    public func rMagic() -> UInt32 {
        buffers[Int(rWork)].loadUnaligned(as: UInt32.self).littleEndian
    }
    public func rPayloadLen() -> UInt32 {
        buffers[Int(rWork)].advanced(by: 12).loadUnaligned(as: UInt32.self).littleEndian
    }

    public func rLivePtr(_ offset: Int) -> UnsafeRawPointer? {
        guard offset < bufSize else { return nil }
        return UnsafeRawPointer(buffers[Int(rWork)].advanced(by: offset))
    }

    // --- I6 handshake ---

    public func revoke() { revoked.store(true, ordering: .releasing) }

    public func reclaim(preRevokeEpoch: UInt32, timeoutMs: Int) -> Bool {
        // TIER4 §4 (issue #19): effective bound = min(timeoutMs, ceiling).
        // Ceiling 0 disables itself. Timeouts are counted, never silent; the
        // caller must NOT poison/free after false (the writer has not ACKed).
        var effectiveMs = timeoutMs
        if maxReclaimTimeoutMs != 0 && effectiveMs > maxReclaimTimeoutMs {
            effectiveMs = maxReclaimTimeoutMs
        }
        let start = Date()
        while true {
            if epoch.load(ordering: .acquiring) != preRevokeEpoch { return true }
            if Date().timeIntervalSince(start) > Double(effectiveMs) / 1000.0 {
                tReclaimTimeouts.wrappingIncrement(by: 1, ordering: .relaxed)
                return false
            }
            usleep(100)
        }
    }

    /// TIER4 §4: runtime-configurable reclaim ceiling (ms); 0 disables.
    /// Mirrors weft_set_max_reclaim_timeout (core/c/weft.h).
    private var maxReclaimTimeoutMs: Int = 1000
    public func setMaxReclaimTimeout(_ maxMs: Int) { maxReclaimTimeoutMs = maxMs }
    public func maxReclaimTimeout() -> Int { maxReclaimTimeoutMs }
    private let tReclaimTimeouts = ManagedAtomic<UInt64>(0)
    public func tReclaimTimeoutsCount() -> UInt64 { tReclaimTimeouts.load(ordering: .relaxed) }

    // --- Telemetry (advisory) ---
    public func tPublishCount() -> UInt64 { tPublish.load(ordering: .relaxed) }
    public func tClaimCount() -> UInt64 { tClaim.load(ordering: .relaxed) }
    public func tDropCount() -> UInt64 { tDrop.load(ordering: .relaxed) }
    public func tInvalidCount() -> UInt64 { tInvalid.load(ordering: .relaxed) }

    /// debug: API parity with C kernel (WO-P2 T1 mirror).
    public func debugState() -> [String: Any] {
        return [
            "latest": latest.load(ordering: .relaxed),
            "wWork": wWork,
            "rWork": rWork,
            "revoked": revoked.load(ordering: .relaxed),
            "epoch": epoch.load(ordering: .acquiring),
            "tPublish": tPublish.load(ordering: .relaxed),
            "tClaim": tClaim.load(ordering: .relaxed),
            "tDrop": tDrop.load(ordering: .relaxed),
            "advisory": true
        ]
    }

    // --- Buffer access for tools ---
    public func bufferPtr(_ i: Int) -> UnsafeMutableRawPointer? {
        guard i < 3 else { return nil }
        return buffers[i]
    }
}

// --- Envelope pure functions ---

public func envelopeEncodeV1(_ buf: UnsafeMutableRawPointer, seq: UInt32, payloadLen: UInt32) {
    envelopeEncode(buf, version: WEFT_VERSION_1, headerSize: 16, seq: seq, payloadLen: payloadLen)
}

public func envelopeEncode(_ buf: UnsafeMutableRawPointer, version: UInt16, headerSize: UInt16, seq: UInt32, payloadLen: UInt32) {
    buf.storeBytes(of: WEFT_MAGIC.littleEndian, as: UInt32.self)
    buf.advanced(by: 4).storeBytes(of: version.littleEndian, as: UInt16.self)
    buf.advanced(by: 6).storeBytes(of: headerSize.littleEndian, as: UInt16.self)
    buf.advanced(by: 8).storeBytes(of: seq.littleEndian, as: UInt32.self)
    buf.advanced(by: 12).storeBytes(of: payloadLen.littleEndian, as: UInt32.self)
    for i in 16..<Int(headerSize) {
        buf.advanced(by: i).storeBytes(of: 0xAA as UInt8, as: UInt8.self)
    }
}

public func envelopeDecode(_ buf: UnsafeRawPointer, avail: Int) -> DecodeResult {
    if avail < 16 { return .short }
    if buf.loadUnaligned(as: UInt32.self).littleEndian != WEFT_MAGIC { return .badMagic }
    let hs = buf.advanced(by: 6).loadUnaligned(as: UInt16.self).littleEndian
    if hs < 16 || Int(hs) > avail { return .badHeader }
    let pl = buf.advanced(by: 12).loadUnaligned(as: UInt32.self).littleEndian
    if Int(pl) > avail - Int(hs) { return .short }
    return .ok
}

public func negotiate(writerVersion: UInt16, readerVersions: [UInt16]) -> UInt16 {
    var chosen: UInt16 = 0
    for rv in readerVersions {
        if rv <= writerVersion && rv > chosen { chosen = rv }
    }
    return chosen
}

// --- Payload pattern (04-LITMUS §0.1) ---

public func mix32(_ x: UInt32) -> UInt32 {
    var x = x
    x ^= x >> 16
    x &*= 0x7FEB352D
    x ^= x >> 15
    x &*= 0x846CA68B
    x ^= x >> 16
    return x
}

public func pat(seq: UInt32, i: UInt32) -> UInt8 {
    let x = seq &* 2654435761 &+ i &* 2246822519
    return UInt8(mix32(x) & 0xFF)
}
