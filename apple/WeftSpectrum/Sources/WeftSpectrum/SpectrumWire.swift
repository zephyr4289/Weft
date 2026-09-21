// SpectrumWire.swift — SHP1 zero-copy decode (Pillar 5, Swift lane).
//
// Normative layout: docs/spectrum/SPECTRUM-WIRE-V1.md — offsets/codes frozen
// and identical to wire.js / wire.py / spectrum_wire.dart (CI audit + parity
// stage enforce).
//
// Law 1: ProfileFlyweight is a final class with mutable Int fields — created
// once, mutated in place forever. snapshot(into:) allocates nothing.
// Law 2: every multi-byte load is an explicit `.littleEndian` byte-swap init
// over an unaligned load — no native endianness assumptions, ever.
// Law 4: validate() returns frozen §6 codes; a torn record can never
// silently decode as garbage.

import Foundation

// §6 Law 4 error taxonomy (frozen; identical across all managed runtimes)
public let eBadMagic: Int32 = 1
public let eBadVersion: Int32 = 2
public let eBadSize: Int32 = 3
public let eCrcMismatch: Int32 = 4
public let eReservedDirty: Int32 = 5
public let eProbeUnavailable: Int32 = 6
public let eDeviceLost: Int32 = 7
public let eFfiTimeout: Int32 = 8
public let eHeapPressure: Int32 = 9
public let eListenerLeak: Int32 = 10
public let eAlignInvalid: Int32 = 11
public let eHudContextLost: Int32 = 12
public let eUnmarshalFailed: Int32 = 13
public let eTierExhausted: Int32 = 14
public let eHudRecovered: Int32 = 15

public func errorName(_ code: Int32) -> String {
    switch code {
    case eBadMagic: return "E_BAD_MAGIC"
    case eBadVersion: return "E_BAD_VERSION"
    case eBadSize: return "E_BAD_SIZE"
    case eCrcMismatch: return "E_CRC_MISMATCH"
    case eReservedDirty: return "E_RESERVED_DIRTY"
    case eProbeUnavailable: return "E_PROBE_UNAVAILABLE"
    case eDeviceLost: return "E_DEVICE_LOST"
    case eFfiTimeout: return "E_FFI_TIMEOUT"
    case eHeapPressure: return "E_HEAP_PRESSURE"
    case eListenerLeak: return "E_LISTENER_LEAK"
    case eAlignInvalid: return "E_ALIGN_INVALID"
    case eHudContextLost: return "E_HUD_CONTEXT_LOST"
    case eUnmarshalFailed: return "E_UNMARSHAL_FAILED"
    case eTierExhausted: return "E_TIER_EXHAUSTED"
    case eHudRecovered: return "E_HUD_RECOVERED"
    default: return "E_UNKNOWN_\(code)"
    }
}

// §2 record geometry (frozen)
public let shp1RecordSize: Int = 192
public let shp1CrcOffset: Int = 188
public let shp1LayoutVersion: UInt16 = 1

// §3 feature bits
public let featWasmSimd128: UInt8 = 0
public let featSharedArrayBuffer: UInt8 = 1
public let featWebgpu: UInt8 = 2
public let featWebgl2: UInt8 = 3
public let featAvx512: UInt8 = 4
public let featAvx2: UInt8 = 5
public let featSse42: UInt8 = 6
public let featNeon: UInt8 = 7
public let featSve2: UInt8 = 8
public let featRvv: UInt8 = 9
public let featMetal3: UInt8 = 10
public let featCuda: UInt8 = 11
public let featAppleMps: UInt8 = 12
public let featOpenvino: UInt8 = 13
public let featFastRpcDsp: UInt8 = 14
public let featNeuropilot: UInt8 = 15
public let featMultilaneDma: UInt8 = 16
public let featBigLittle: UInt8 = 17
public let featThermalSensor: UInt8 = 18
public let featDlpackExport: UInt8 = 19

// Enum domains
public let tierUnknown: UInt32 = 0
public let tierFlagship: UInt32 = 1
public let tierMid: UInt32 = 2
public let tierBudget: UInt32 = 3
public let thermalNominal: UInt32 = 0
public let thermalLight: UInt32 = 1
public let thermalModerate: UInt32 = 2
public let thermalSevere: UInt32 = 3
public let thermalCritical: UInt32 = 4
public let visVisible: UInt32 = 0
public let visHidden: UInt32 = 1
public let visUnknown: UInt32 = 2
public let chargingNo: UInt32 = 0
public let chargingYes: UInt32 = 1
public let chargingUnknown: UInt32 = 2
public let batteryUnknown: UInt32 = 0xFFFF

// CRC-32 (IEEE 802.3, reflected) — identical arithmetic to wire.js / wire.py.
enum Crc32 {
    static let table: [UInt32] = {
        var t = [UInt32](repeating: 0, count: 256)
        for n in 0..<256 {
            var c: UInt32 = UInt32(n)
            for _ in 0..<8 {
                c = (c & 1) != 0 ? (0xEDB88320 ^ (c >> 1)) : (c >> 1)
            }
            t[n] = c
        }
        return t
    }()

    static func digest(_ bytes: UnsafeRawBufferPointer, _ len: Int) -> UInt32 {
        var c: UInt32 = 0xFFFFFFFF
        for i in 0..<len {
            c = table[Int((c ^ UInt32(bytes[i])) & 0xFF)] ^ (c >> 8)
        }
        return c ^ 0xFFFFFFFF
    }
}

public func crc32Shp1(_ bytes: [UInt8]) -> UInt32 {
    bytes.withUnsafeBufferPointer { Crc32.digest(UnsafeRawBufferPointer($0), bytes.count) }
}

/// Reusable decode target (Law 1).
public final class ProfileFlyweight {
    public var layoutVersion: UInt16 = 0
    public var recordSize: UInt16 = 0
    public var featureFlagsLo: UInt32 = 0
    public var featureFlagsHi: UInt32 = 0
    public var siliconTier: UInt32 = tierUnknown
    public var thermalState: UInt32 = thermalNominal
    public var perfCores: UInt32 = 0
    public var effCores: UInt32 = 0
    public var gpuFamily: UInt32 = 0
    public var cacheLineBytes: UInt32 = 64
    public var cpuMaxClockKhz: UInt64 = 0
    public var memoryTotalBytes: UInt64 = 0
    public var memoryBudgetBytes: UInt64 = 0
    public var simdWidthBits: UInt32 = 0
    public var frameBudgetUs: UInt32 = 0
    public var maxFrameRateMilliHz: UInt64 = 0
    public var batteryPermille: UInt32 = batteryUnknown
    public var batteryCharging: UInt32 = chargingUnknown
    public var visibility: UInt32 = visUnknown
    public var dmaLaneCount: UInt32 = 0
    public var vendorId: UInt32 = 0
    public var deviceId: UInt32 = 0

    public init() {}

    public func hasFeatureBit(_ bit: UInt8) -> Bool {
        bit < 32
            ? (featureFlagsLo & (1 << bit)) != 0
            : (featureFlagsHi & (1 << (bit - 32))) != 0
    }
}

/// Zero-copy window over ONE 192-byte SHP1 record.
public struct ProfileView {
    let bytes: UnsafeRawBufferPointer

    /// Caller owns `base`; it must outlive the view and expose >= 192 bytes.
    public init(_ base: UnsafeRawPointer) {
        bytes = UnsafeRawBufferPointer(start: base, count: shp1RecordSize)
    }

    public init(_ data: [UInt8]) {
        precondition(data.count >= shp1RecordSize, "SHP1 record too small")
        bytes = UnsafeRawBufferPointer(data)
    }

    /// Law 4 gate: 0 when intact, else a §6 code (cheapest checks first).
    public func validate() -> Int32 {
        if bytes.count < shp1RecordSize { return eBadSize }
        guard bytes[0] == 0x53, bytes[1] == 0x48, bytes[2] == 0x50, bytes[3] == 0x31 else {
            return eBadMagic
        }
        if version != shp1LayoutVersion { return eBadVersion }
        if recordSize != UInt16(shp1RecordSize) { return eBadSize }
        for i in 104..<shp1CrcOffset where bytes[i] != 0 { return eReservedDirty }
        if Crc32.digest(bytes, shp1CrcOffset) != crc32 { return eCrcMismatch }
        return 0
    }

    var version: UInt16 {
        UInt16(littleEndian: bytes.loadUnaligned(fromByteOffset: 4, as: UInt16.self))
    }
    var recordSize: UInt16 {
        UInt16(littleEndian: bytes.loadUnaligned(fromByteOffset: 6, as: UInt16.self))
    }
    public var featureFlagsLo: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 8, as: UInt32.self))
    }
    public var featureFlagsHi: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 12, as: UInt32.self))
    }
    public var siliconTier: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 16, as: UInt32.self))
    }
    public var thermalState: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 20, as: UInt32.self))
    }
    public var perfCores: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 24, as: UInt32.self))
    }
    public var effCores: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 28, as: UInt32.self))
    }
    public var gpuFamily: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 32, as: UInt32.self))
    }
    public var cacheLineBytes: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 36, as: UInt32.self))
    }
    public var cpuMaxClockKhz: UInt64 {
        UInt64(littleEndian: bytes.loadUnaligned(fromByteOffset: 40, as: UInt64.self))
    }
    public var memoryTotalBytes: UInt64 {
        UInt64(littleEndian: bytes.loadUnaligned(fromByteOffset: 48, as: UInt64.self))
    }
    public var memoryBudgetBytes: UInt64 {
        UInt64(littleEndian: bytes.loadUnaligned(fromByteOffset: 56, as: UInt64.self))
    }
    public var simdWidthBits: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 64, as: UInt32.self))
    }
    public var frameBudgetUs: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 68, as: UInt32.self))
    }
    public var maxFrameRateMilliHz: UInt64 {
        UInt64(littleEndian: bytes.loadUnaligned(fromByteOffset: 72, as: UInt64.self))
    }
    public var batteryPermille: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 80, as: UInt32.self))
    }
    public var batteryCharging: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 84, as: UInt32.self))
    }
    public var visibility: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 88, as: UInt32.self))
    }
    public var dmaLaneCount: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 92, as: UInt32.self))
    }
    public var vendorId: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 96, as: UInt32.self))
    }
    public var deviceId: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: 100, as: UInt32.self))
    }
    public var crc32: UInt32 {
        UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: shp1CrcOffset, as: UInt32.self))
    }

    /// Copies all fields into the caller-owned flyweight (no allocation).
    public func snapshot(into dst: ProfileFlyweight) {
        dst.layoutVersion = version
        dst.recordSize = recordSize
        dst.featureFlagsLo = featureFlagsLo
        dst.featureFlagsHi = featureFlagsHi
        dst.siliconTier = siliconTier
        dst.thermalState = thermalState
        dst.perfCores = perfCores
        dst.effCores = effCores
        dst.gpuFamily = gpuFamily
        dst.cacheLineBytes = cacheLineBytes
        dst.cpuMaxClockKhz = cpuMaxClockKhz
        dst.memoryTotalBytes = memoryTotalBytes
        dst.memoryBudgetBytes = memoryBudgetBytes
        dst.simdWidthBits = simdWidthBits
        dst.frameBudgetUs = frameBudgetUs
        dst.maxFrameRateMilliHz = maxFrameRateMilliHz
        dst.batteryPermille = batteryPermille
        dst.batteryCharging = batteryCharging
        dst.visibility = visibility
        dst.dmaLaneCount = dmaLaneCount
        dst.vendorId = vendorId
        dst.deviceId = deviceId
    }
}
