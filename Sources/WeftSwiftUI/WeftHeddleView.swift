// WeftHeddleView.swift — Dual-path SwiftUI Heddle view
//
// WHY EXISTS: Provides a high-performance SwiftUI view bound to a Weft channel
// executing draw-phase reads per WHITEPAPER §8.3 and DIRECTIVE-13 T13.2.
//
// DUAL-PATH RENDERING (now real, not two identical Canvas branches):
// 1. .canvas path: SwiftUI Canvas + CoreGraphics, frame-paced by SwiftUI
//    (60 Hz on standard displays).
// 2. .metal path: an MTKView-backed representable whose delegate claims the
//    Weft once per display refresh and hands the reader-held payload pointer
//    to a Metal render-command closure. `preferredFramesPerSecond` is set
//    from the display probe (120 on ProMotion). Requires a `drawMetal`
//    closure; without one the view honestly falls back to the Canvas path.
//
// HONESTY NOTICE (updated when the probe landed): the Metal path is
// MEASURED now — MTKViewCadenceProbe renders real MTKView frames with a
// live kernel stream in CI (macOS runner + iOS-simulator legs,
// MetalProbeTests.swift): draw-phase claims == frames rendered, zero torn
// frames accepted, requested cadence plumbing proven. What remains
// device-deferred is the ProMotion 120 Hz RATE itself (VMs and simulators
// cap the display link; the report carries measured-vs-requested so the
// gap is visible, never greenwashed). See Tests/WeftTests/MetalProbeTests.swift.

import SwiftUI
import WeftCore

#if canImport(MetalKit) && !os(watchOS)
import MetalKit
#endif

/// SwiftUI Heddle view supporting both Canvas and Metal execution pipelines.
public struct WeftHeddleView: View {
    public let weft: Weft
    public let mode: RendererMode
    public let drawCanvas: (GraphicsContext, CGSize, UnsafeRawPointer) -> Void
    /// Metal render closure (Metal path only). Receives the device, an active
    /// render command encoder, the CPU pointer to the reader-held PAYLOAD
    /// (absolute index 0 = payload start, parity with rLivePtr(16)), and the
    /// drawable size. Encode your commands; the delegate ends the encoding
    /// and presents.
    #if canImport(MetalKit) && !os(watchOS)
    public let drawMetal: ((MTLDevice, MTLRenderCommandEncoder, UnsafeRawPointer, CGSize) -> Void)?

    public init(
        weft: Weft,
        mode: RendererMode = .auto,
        drawCanvas: @escaping (GraphicsContext, CGSize, UnsafeRawPointer) -> Void,
        drawMetal: ((MTLDevice, MTLRenderCommandEncoder, UnsafeRawPointer, CGSize) -> Void)? = nil
    ) {
        self.weft = weft
        self.mode = mode
        self.drawCanvas = drawCanvas
        self.drawMetal = drawMetal
    }
    #else
    public init(
        weft: Weft,
        mode: RendererMode = .auto,
        drawCanvas: @escaping (GraphicsContext, CGSize, UnsafeRawPointer) -> Void
    ) {
        self.weft = weft
        self.mode = mode
        self.drawCanvas = drawCanvas
    }
    #endif

    public var body: some View {
        // Resolve from the REAL display cadence (was a hardcoded 60 Hz
        // default inside body, which made .auto always pick Canvas).
        let detector = CadenceDetector.forCurrentDisplay()
        let renderer = detector.resolve(mode: mode)

        Group {
            switch renderer {
            case .metal:
                #if canImport(MetalKit) && !os(watchOS)
                if let metalDraw = drawMetal {
                    WeftMetalHost(
                        weft: weft,
                        preferredFPS: detector.preferredFPS(for: .metal),
                        drawMetal: metalDraw
                    )
                } else {
                    canvasView
                }
                #else
                canvasView
                #endif
            case .canvas:
                canvasView
            }
        }
    }

    /// The Canvas draw-phase binding: claim freshest frame, read the
    /// READER-HELD buffer live (rLivePtr), draw. Zero recomposition.
    private var canvasView: some View {
        Canvas { context, size in
            _ = weft.claim()
            if let ptr = weft.rLivePtr(16) {
                drawCanvas(context, size, ptr)
            }
        }
    }
}

#if canImport(MetalKit) && !os(watchOS)

/// MTKView-backed host. The coordinator is the MTKViewDelegate: one claim per
/// display refresh, payload pointer handed to the user's Metal closure.
/// macOS drives via NSViewRepresentable; iOS/tvOS/visionOS via
/// UIViewRepresentable.
public final class WeftMetalCoordinator: NSObject, MTKViewDelegate {
    private let weft: Weft
    private let drawMetal: (MTLDevice, MTLRenderCommandEncoder, UnsafeRawPointer, CGSize) -> Void
    private var commandQueue: MTLCommandQueue?

    init(
        weft: Weft,
        drawMetal: @escaping (MTLDevice, MTLRenderCommandEncoder, UnsafeRawPointer, CGSize) -> Void
    ) {
        self.weft = weft
        self.drawMetal = drawMetal
        super.init()
    }

    public func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    public func draw(in view: MTKView) {
        // Draw-phase read: claim the freshest complete frame, then read the
        // READER-HELD buffer in place (A3 — never a snapshot).
        _ = weft.claim()
        guard let device = view.device,
              let ptr = weft.rLivePtr(16),
              let descriptor = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable
        else { return }

        // MTKView does not own a command queue; hold one lazily per device.
        if commandQueue == nil {
            commandQueue = device.makeCommandQueue()
        }
        guard let queue = commandQueue,
              let commandBuffer = queue.makeCommandBuffer(),
              let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: descriptor)
        else { return }

        let size = CGSize(width: view.drawableSize.width, height: view.drawableSize.height)
        drawMetal(device, encoder, ptr, size)
        encoder.endEncoding()
        commandBuffer.present(drawable)
        commandBuffer.commit()
    }
}

#if os(macOS)
public struct WeftMetalHost: NSViewRepresentable {
    public let weft: Weft
    public let preferredFPS: Int
    public let drawMetal: (MTLDevice, MTLRenderCommandEncoder, UnsafeRawPointer, CGSize) -> Void

    public init(
        weft: Weft,
        preferredFPS: Int,
        drawMetal: @escaping (MTLDevice, MTLRenderCommandEncoder, UnsafeRawPointer, CGSize) -> Void
    ) {
        self.weft = weft
        self.preferredFPS = preferredFPS
        self.drawMetal = drawMetal
    }

    public func makeCoordinator() -> WeftMetalCoordinator {
        WeftMetalCoordinator(weft: weft, drawMetal: drawMetal)
    }

    public func makeNSView(context: Context) -> MTKView {
        makeMTKView(context: context)
    }
    public func updateNSView(_ view: MTKView, context: Context) {
        view.preferredFramesPerSecond = preferredFPS
    }

    private func makeMTKView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.delegate = context.coordinator
        view.enableSetNeedsDisplay = false
        view.isPaused = false
        view.preferredFramesPerSecond = preferredFPS
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        return view
    }
}
#else
public struct WeftMetalHost: UIViewRepresentable {
    public let weft: Weft
    public let preferredFPS: Int
    public let drawMetal: (MTLDevice, MTLRenderCommandEncoder, UnsafeRawPointer, CGSize) -> Void

    public init(
        weft: Weft,
        preferredFPS: Int,
        drawMetal: @escaping (MTLDevice, MTLRenderCommandEncoder, UnsafeRawPointer, CGSize) -> Void
    ) {
        self.weft = weft
        self.preferredFPS = preferredFPS
        self.drawMetal = drawMetal
    }

    public func makeCoordinator() -> WeftMetalCoordinator {
        WeftMetalCoordinator(weft: weft, drawMetal: drawMetal)
    }

    public func makeUIView(context: Context) -> MTKView {
        makeMTKView(context: context)
    }
    public func updateUIView(_ view: MTKView, context: Context) {
        view.preferredFramesPerSecond = preferredFPS
    }

    private func makeMTKView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.delegate = context.coordinator
        view.enableSetNeedsDisplay = false
        view.isPaused = false
        view.preferredFramesPerSecond = preferredFPS
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        return view
    }
}
#endif

#endif
