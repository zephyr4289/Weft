// CadenceDetector.swift — Display refresh rate detection and renderer selection
//
// WHY EXISTS: Implements the dual-path Heddle cadence selection (60 Hz Canvas vs
// 120 Hz Metal on ProMotion) per WHITEPAPER §8.3 and DIRECTIVE-13 T13.2.
//
// HONESTY NOTICE:
// Hardware frame pacing on physical ProMotion displays is unverified by design
// in this sandbox/simulator environment. Selection logic is deterministic and
// unit-tested.

import Foundation

// Conditional platform imports: screenMaxRefreshRate() uses UIScreen under
// UIKit and NSScreen under AppKit behind matching canImport guards — the
// symbols need their frameworks imported under the same conditions. The
// AppKit arm compiled without its import on macOS ("cannot find 'NSScreen'
// in scope", apple CI log for 8d6eebf — masked until now by the tee-pipe
// exit-code swallow in the CI steps).
#if canImport(UIKit)
import UIKit
#elseif canImport(AppKit)
import AppKit
#endif

/// Active rendering pipeline mode.
public enum RendererMode: String, CaseIterable, Equatable, Sendable {
    case auto
    case canvas60
    case metal120
}

/// Resolved concrete renderer engine.
public enum ConcreteRenderer: String, Equatable, Sendable {
    case canvas
    case metal
}

/// Cadence detector and renderer resolver.
public struct CadenceDetector: Sendable {
    public let maxRefreshRate: Double

    public init(maxRefreshRate: Double = 60.0) {
        self.maxRefreshRate = maxRefreshRate
    }

    /// Probe the host display's maximum refresh rate so `.auto` resolves from
    /// REAL hardware cadence. The previous revision constructed
    /// `CadenceDetector()` with the 60 Hz default inside `body`, which made
    /// `.auto` resolve to Canvas on every device — ProMotion was unreachable.
    ///
    /// - iOS/tvOS: `UIScreen.main.maximumFramesPerSecond` (ProMotion reports 120).
    /// - macOS: `NSScreen.main?.maximumFramesPerSecond`.
    /// - Elsewhere: 60 (honest default; claimed, not probed).
    public static func screenMaxRefreshRate() -> Double {
        #if canImport(UIKit)
        return Double(UIScreen.main.maximumFramesPerSecond)
        #elseif canImport(AppKit)
        return Double(NSScreen.main?.maximumFramesPerSecond ?? 60)
        #else
        return 60.0
        #endif
    }

    /// Convenience: a detector resolved from the real display.
    public static func forCurrentDisplay() -> CadenceDetector {
        CadenceDetector(maxRefreshRate: screenMaxRefreshRate())
    }

    /// Resolve the concrete renderer given the configured mode.
    public func resolve(mode: RendererMode) -> ConcreteRenderer {
        switch mode {
        case .canvas60:
            return .canvas
        case .metal120:
            return .metal
        case .auto:
            return maxRefreshRate >= 119.0 ? .metal : .canvas
        }
    }

    /// Target preferred frames per second for display link configuration.
    public func preferredFPS(for renderer: ConcreteRenderer) -> Int {
        switch renderer {
        case .canvas:
            return 60
        case .metal:
            return maxRefreshRate >= 119.0 ? 120 : 60
        }
    }
}
