// WeftSpectrumModel.swift — @Observable hardware model + thermal guard
// (Pillar 5, mandate B).
//
// SWIFT CONCURRENCY + OBSERVATION: a @MainActor @Observable final class —
// SwiftUI views observe `tier`, `thermalCode`, and `capHz` THROUGH THE
// FRAMEWORK's observation channel. The Zero Re-Render Rule is satisfied by
// CONSTRUCTION: telemetry writes edge-trigger the observation only when a
// published field actually changes (identical grammar to the React/Flutter
// lanes — direct channel, no rogue reconciliation storms).
//
// THERMAL GUARD: ProcessInfo.processInfo.thermalState (nominal/fair/serious/
// critical) maps onto the SHP1 thermal codes and feeds the SAME cadence
// governor every other runtime uses — one frozen transition table everywhere.
//
// METAL SELECTION: MetalPipelineSelector picks the render pipeline class
// from the current thermal pressure + detected GPU family (see
// MetalPipelineSelector.swift).

import Foundation

#if canImport(Observation)
import Observation
#endif

// ProcessInfo.ThermalState -> SHP1 thermal codes
public func shp1ThermalCode(_ state: ProcessInfo.ThermalState) -> UInt32 {
    switch state {
    case .nominal: return thermalNominal
    case .fair: return thermalLight
    case .serious: return thermalSevere
    case .critical: return thermalCritical
    @unknown default: return thermalModerate
    }
}

#if canImport(Observation)
@MainActor
@Observable
public final class WeftSpectrumModel {
    public private(set) var tier: UInt32 = tierUnknown
    public private(set) var thermalCode: UInt32 = thermalNominal
    public private(set) var capHz: Int = 240
    public private(set) var lastErrorCode: Int32 = 0
    public private(set) var probeAvailable: Bool = false
    public private(set) var memoryBudgetBytes: UInt64 = 0

    // Flyweights (owned once — the tick path never allocates)
    public let cadence = CadenceState()
    let input = GovernorInput()
    let profile = ProfileFlyweight()

    public init() {}

    /// Attach from an SHP1 record buffer (E1 seam output or fixture).
    /// Fail-soft: an invalid record surfaces a §6 code and keeps a fallback.
    public func attach(record: [UInt8]) {
        guard record.count >= shp1RecordSize else {
            lastErrorCode = eBadSize
            return
        }
        let view = ProfileView(record)
        let code = view.validate()
        if code == 0 {
            view.snapshot(into: profile)
            tier = profile.siliconTier
            memoryBudgetBytes = profile.memoryBudgetBytes
            probeAvailable = true
            lastErrorCode = 0
        } else {
            lastErrorCode = code
            profile.siliconTier = tierFlagship
            tier = tierFlagship
            memoryBudgetBytes = 8589934592
        }
    }

    /// One steady-state telemetry tick driven by the host's thermal guard.
    /// Observation only fires on REAL field changes (edge-triggered).
    public func tick() {
        let thermal = shp1ThermalCode(ProcessInfo.processInfo.thermalState)
        input.thermalState = thermal
        input.tierMaxHzCap = tierMaxHz[profile.siliconTier] ?? 60
        cadenceTick(cadence, input)
        if cadence.capHz != capHz { capHz = cadence.capHz }
        if thermal != thermalCode { thermalCode = thermal }
    }

    /// Transparent down-tier on device-lost / FFI-timeout / heap pressure.
    public func applyPressureEvent(_ code: Int32) {
        lastErrorCode = code
        input.heapPressure = 1
        tierTick(cadence, input)
        input.heapPressure = 0
        memoryBudgetBytes = UInt64(effectiveBudgetBytes(
            Int(profile.memoryBudgetBytes == 0 ? 8589934592 : profile.memoryBudgetBytes),
            cadence.tierStage))
        tier = profile.siliconTier + UInt32(cadence.tierStage) > tierBudget
            ? tierBudget
            : profile.siliconTier + UInt32(cadence.tierStage)
    }
}
#else
/// Observation-unavailable fallback (pre-14 macOS / pre-17 iOS builds):
/// same fields, manual polling grammar. Feature parity, no observation.
@MainActor
public final class WeftSpectrumModel {
    public private(set) var tier: UInt32 = tierUnknown
    public private(set) var thermalCode: UInt32 = thermalNominal
    public private(set) var capHz: Int = 240
    public private(set) var lastErrorCode: Int32 = 0
    public private(set) var probeAvailable: Bool = false
    public private(set) var memoryBudgetBytes: UInt64 = 0

    public let cadence = CadenceState()
    let input = GovernorInput()
    let profile = ProfileFlyweight()

    public init() {}

    public func attach(record: [UInt8]) {
        guard record.count >= shp1RecordSize else {
            lastErrorCode = eBadSize
            return
        }
        let view = ProfileView(record)
        let code = view.validate()
        if code == 0 {
            view.snapshot(into: profile)
            tier = profile.siliconTier
            memoryBudgetBytes = profile.memoryBudgetBytes
            probeAvailable = true
            lastErrorCode = 0
        } else {
            lastErrorCode = code
            profile.siliconTier = tierFlagship
            tier = tierFlagship
            memoryBudgetBytes = 8589934592
        }
    }

    public func tick() {
        let thermal = shp1ThermalCode(ProcessInfo.processInfo.thermalState)
        input.thermalState = thermal
        input.tierMaxHzCap = tierMaxHz[profile.siliconTier] ?? 60
        cadenceTick(cadence, input)
        capHz = cadence.capHz
        thermalCode = thermal
    }

    public func applyPressureEvent(_ code: Int32) {
        lastErrorCode = code
        input.heapPressure = 1
        tierTick(cadence, input)
        input.heapPressure = 0
        memoryBudgetBytes = UInt64(effectiveBudgetBytes(
            Int(profile.memoryBudgetBytes == 0 ? 8589934592 : profile.memoryBudgetBytes),
            cadence.tierStage))
        tier = profile.siliconTier + UInt32(cadence.tierStage) > tierBudget
            ? tierBudget
            : profile.siliconTier + UInt32(cadence.tierStage)
    }
}
#endif
