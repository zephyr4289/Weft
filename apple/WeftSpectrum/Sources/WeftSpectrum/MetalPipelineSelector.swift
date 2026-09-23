// MetalPipelineSelector.swift — thermal-pressure-aware pipeline selection
// (Pillar 5, mandate B).
//
// Maps (GPU family class, thermal pressure) -> a render pipeline CLASS. The
// actual MTLRenderPipelineState objects belong to Engineer 2's native
// acceleration drivers; this selector picks WHICH class their dispatch uses
// through the seam — it never authors driver internals (Pillar boundary).
//
// Law 4: a device that fails family queries down-tiers to the conservative
// pipeline; nothing throws.

import Foundation

#if canImport(Metal)
import Metal
#endif

public enum RenderPipelineClass: String, Sendable {
    case ultra = "PIPELINE_ULTRA_240"      // Tier 1: 240 FPS, full SIMD, multi-lane DMA
    case high = "PIPELINE_HIGH_120"        // Tier 2: 120 FPS
    case standard = "PIPELINE_STANDARD_60" // Tier 3 / throttled: 60 FPS
    case conservative = "PIPELINE_CONSERVATIVE_30" // critical thermal / background
}

public struct MetalPipelineSelection: Equatable, Sendable {
    public let pipelineClass: RenderPipelineClass
    public let maxFrameRate: Int
    public let simdWidth: Int
    public let dmaLaneBudget: Int
}

public enum MetalPipelineSelector {
    /// Family class from the device's GPU (E2 driver seam feeds the real
    /// device; on hosts without Metal this stays conservative).
    public static func familyClass(gpuFamily: UInt32) -> Int {
        switch gpuFamily {
        case 1: return 3 // APPLE
        case 2: return 3 // DESKTOP_DISCRETE
        case 4: return 2 // CONSOLE
        case 3: return 1 // MOBILE_INTEGRATED
        default: return 1
        }
    }

    /// Thermal pressure (SHP1 codes) + family class -> pipeline selection.
    public static func select(gpuFamily: UInt32, thermalCode: UInt32,
                              cadenceCap: Int, dmaLanes: UInt32) -> MetalPipelineSelection {
        let family = familyClass(gpuFamily: gpuFamily)
        let critical = thermalCode >= thermalCritical
        let severe = thermalCode >= thermalSevere

        let pipelineClass: RenderPipelineClass
        if critical {
            pipelineClass = .conservative
        } else if severe || cadenceCap <= 30 {
            pipelineClass = cadenceCap <= 30 ? .conservative : .standard
        } else if cadenceCap <= 60 {
            pipelineClass = .standard
        } else if cadenceCap <= 120 {
            pipelineClass = family >= 2 ? .high : .standard
        } else {
            pipelineClass = family >= 3 ? .ultra : (family >= 2 ? .high : .standard)
        }

        let maxRate: Int
        switch pipelineClass {
        case .ultra: maxRate = 240
        case .high: maxRate = 120
        case .standard: maxRate = 60
        case .conservative: maxRate = 30
        }
        let simd = family >= 3 ? 512 : (family >= 2 ? 256 : 128)
        let lanes = critical ? 1 : max(1, Int(dmaLanes))
        return MetalPipelineSelection(
            pipelineClass: pipelineClass,
            maxFrameRate: maxRate,
            simdWidth: simd,
            dmaLaneBudget: lanes
        )
    }
}
