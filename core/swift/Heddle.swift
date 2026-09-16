// Heddle.swift — Draw-phase binding for SwiftUI (Swift)
//
// WHY EXISTS: Binds a Weft to the SwiftUI Draw phase. Per WHITEPAPER §8.3:
// two paths — Canvas + CADisplayLink at 60 Hz, MTKView at 120 Hz on ProMotion.
// The Steward probes the device at bind() time; the dev writes one closure
// either way. Per 02-KERNEL §4.2: reads a Weft during the Draw phase only.
//
// MTKView PATH — now REAL, not comment-only: WeftHeddleView (WeftSwiftUI)
// implements the Metal route end-to-end, and MTKViewCadenceProbe measures
// it (frames rendered, draw-phase claims, measured vs requested cadence)
// in CI — see Tests/WeftTests/MetalProbeTests.swift. The 120 Hz RATE on
// real ProMotion hardware remains deferred to the Hardware Deferral List;
// every other surface of the path is CI-proven.
//
// STATUS: CI-PROVEN (Canvas + Metal paths; ProMotion device rate deferred).

import SwiftUI
import QuartzCore

/// WeftCanvas: a SwiftUI view that draws from a Weft on every VSYNC.
/// The closure receives the raw buffer pointer and the claimed buffer index.
public struct WeftCanvas: View {
    public let weft: Weft
    public let draw: (CGContext, UnsafeRawPointer) -> Void

    public init(weft: Weft, draw: @escaping (CGContext, UnsafeRawPointer) -> Void) {
        self.weft = weft
        self.draw = draw
    }

    public var body: some View {
        // 60 Hz path: SwiftUI Canvas + CADisplayLink
        // 120 Hz path on ProMotion: MTKView — IMPLEMENTED in WeftSwiftUI
        // (WeftHeddleView WeftMetalHost) and MEASURED by MTKViewCadenceProbe
        // in CI (MetalProbeTests). This core/swift Canvas path stays the
        // dependency-free default; the Metal route lives behind WeftSwiftUI.
        Canvas { context, size in
            _ = weft.claim()
            if let ptr = weft.rLivePtr(16) {
                // SAFETY: the claimed buffer is exclusively owned by the
                // reader until the next claim(). Per RFC-0001 §4:
                // "reader owns claimed buffer until next claim."
                context.withCGContext { cgContext in
                    draw(cgContext, ptr)
                }
            }
        }
    }
}
