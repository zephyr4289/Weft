// Mdp1Wire.swift — MDP1 aggregated book snapshot wire, Swift side.
//
// Mirrors packages/fintech/src/mdp1.js byte-for-byte (Pillar 6 / Law 4).
// All multi-byte loads pass .littleEndian EXPLICITLY (Law 2). u64 fields
// compose as UInt64(lo) | UInt64(hi) << 32 — exact 64-bit math, never
// Double. The view is a FLYWEIGHT over the caller's bytes: book state
// never crosses into Swift observable state except through the model's
// edge-triggered publish (Law 4 / W4-06).

import Foundation

public enum Mdp1 {
    public static let size = 304
    public static let version: UInt16 = 1
    public static let topLevels = 10
    public static let magic: UInt32 = 0x3150_444d // "MDP1" LE u32 read

    public static let fBookValid: UInt16 = 1 << 0
    public static let fCrossed: UInt16 = 1 << 1
    public static let fLocked: UInt16 = 1 << 2

    public static let offVersion = 4
    public static let offFlags = 6
    public static let offSeq = 8
    public static let offLastTs = 16
    public static let offBestBid = 24
    public static let offBestAsk = 28
    public static let offBids = 32
    public static let offAsks = 152
    public static let offMsgCount = 272
    public static let offTradeCount = 280
    public static let offLastMatch = 288
    public static let offCrc = 296
    public static let crcEnd = 296

    // Typed failure codes — Law 4 taxonomy (TS decision order).
    public static let ok = 0
    public static let eShort = 1
    public static let eMagic = 2
    public static let eVersion = 3
    public static let eCrc = 4

    static let crcTable: [UInt32] = {
        var t = [UInt32](repeating: 0, count: 256)
        for n in 0..<256 {
            var c = UInt32(n)
            for _ in 0..<8 {
                c = (c & 1) != 0 ? (0xedb8_8320 ^ (c >> 1)) : (c >> 1)
            }
            t[n] = c
        }
        return t
    }()

    /// CRC-32/ISO-HDLC over bytes [start, end) — table-driven, no allocs.
    @inlinable
    public static func crc32(_ bytes: UnsafeRawBufferPointer,
                             _ start: Int = 0,
                             _ end: Int = Mdp1.crcEnd) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        let table = Mdp1.crcTable
        for i in start..<end {
            crc = (crc >> 8) ^ table[Int((crc ^ UInt32(bytes[i])) & 0xFF)]
        }
        return crc ^ 0xFFFF_FFFF
    }
}

/// Reusable flyweight over one MDP1 record.
public struct Mdp1View {
    public let bytes: UnsafeRawBufferPointer

    public init(_ bytes: UnsafeRawBufferPointer) {
        self.bytes = bytes
    }

    /// Fail-closed structural check in the TS decision order
    /// (short -> magic -> version -> CRC). 0 == Mdp1.ok.
    public func validate() -> Int {
        if bytes.count < Mdp1.size { return Mdp1.eShort }
        if loadU32(0) != Mdp1.magic { return Mdp1.eMagic }
        if loadU16(Mdp1.offVersion) != Mdp1.version { return Mdp1.eVersion }
        if loadU32(Mdp1.offCrc) != Mdp1.crc32(bytes) { return Mdp1.eCrc }
        return Mdp1.ok
    }

    @inlinable func loadU16(_ off: Int) -> UInt16 {
        bytes.loadUnaligned(fromByteOffset: off, as: UInt16.self).littleEndian
    }
    @inlinable func loadU32(_ off: Int) -> UInt32 {
        bytes.loadUnaligned(fromByteOffset: off, as: UInt32.self).littleEndian
    }

    public var flags: UInt16 { loadU16(Mdp1.offFlags) }
    public var bookValid: Bool { flags & Mdp1.fBookValid != 0 }
    public var crossed: Bool { flags & Mdp1.fCrossed != 0 }
    public var locked: Bool { flags & Mdp1.fLocked != 0 }

    public var seq: UInt64 {
        UInt64(loadU32(Mdp1.offSeq)) |
            UInt64(loadU32(Mdp1.offSeq + 4)) << 32
    }
    public var lastTsNs: UInt64 {
        UInt64(loadU32(Mdp1.offLastTs)) |
            UInt64(loadU32(Mdp1.offLastTs + 4)) << 32
    }
    public var bestBid: UInt32 { loadU32(Mdp1.offBestBid) }
    public var bestAsk: UInt32 { loadU32(Mdp1.offBestAsk) }

    public func bidPrice(_ i: Int) -> UInt32 { loadU32(Mdp1.offBids + i * 12) }
    public func bidSize(_ i: Int) -> UInt32 { loadU32(Mdp1.offBids + i * 12 + 4) }
    public func bidOrders(_ i: Int) -> UInt32 { loadU32(Mdp1.offBids + i * 12 + 8) }
    public func askPrice(_ i: Int) -> UInt32 { loadU32(Mdp1.offAsks + i * 12) }
    public func askSize(_ i: Int) -> UInt32 { loadU32(Mdp1.offAsks + i * 12 + 4) }
    public func askOrders(_ i: Int) -> UInt32 { loadU32(Mdp1.offAsks + i * 12 + 8) }

    public var msgCount: UInt32 { loadU32(Mdp1.offMsgCount) }
    public var tradeCount: UInt32 { loadU32(Mdp1.offTradeCount) }
    public var lastMatch: UInt64 {
        UInt64(loadU32(Mdp1.offLastMatch)) |
            UInt64(loadU32(Mdp1.offLastMatch + 4)) << 32
    }

    public var crcOk: Bool { loadU32(Mdp1.offCrc) == Mdp1.crc32(bytes) }
}
