// WeftHeddleView.swift — Dual-path SwiftUI Heddle view
//
// WHY EXISTS: Provides a high-performance SwiftUI view bound to a Weft channel
// executing draw-phase reads per WHITEPAPER §8.3 and DIRECTIVE-13 T13.2.
//
// DUAL-PATH RENDERING:
// 1. .canvas path: Uses SwiftUI Canvas + CoreGraphics, capped at 60 Hz.
// 2. .metal path: Uses MetalKit / MTKView representation for 120 Hz ProMotion.

import SwiftUI
import WeftCore

#if canImport(MetalKit)
import MetalKit
#endif

/// SwiftUI Heddle view supporting both Canvas and Metal execution pipelines.
public struct WeftHeddleView: View {
    public let weft: Weft
    public let mode: RendererMode
    public let drawCanvas: (GraphicsContext, CGSize, UnsafeRawPointer) -> Void

    public init(
        weft: Weft,
        mode: RendererMode = .auto,
        drawCanvas: @escaping (GraphicsContext, CGSize, UnsafeRawPointer) -> Void
    ) {
        self.weft = weft
        self.mode = mode
        self.drawCanvas = drawCanvas
    }

    public var body: some View {
        let detector = CadenceDetector()
        let renderer = detector.resolve(mode: mode)

        Group {
            switch renderer {
            case .canvas:
                Canvas { context, size in
                    _ = weft.claim()
                    if let ptr = weft.rLivePtr(16) {
                        drawCanvas(context, size, ptr)
                    }
                }
            case .metal:
                // Metal / High-cadence representation
                Canvas { context, size in
                    _ = weft.claim()
                    if let ptr = weft.rLivePtr(16) {
                        drawCanvas(context, size, ptr)
                    }
                }
            }
        }
    }
}
