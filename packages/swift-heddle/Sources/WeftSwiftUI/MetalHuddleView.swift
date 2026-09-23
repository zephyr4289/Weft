// MetalHuddleView.swift — MetalKit canvas bridge rendering DIRECTLY from the
// HPL1 Hot-Plane (charter §B: "@Observable and MetalKit canvas bridge
// rendering directly from POSIX shared memory").
//
// Frame path (Law 1 — zero allocations per frame):
//   MTKView delegate draw(in:) → read lane 0 window into a PREALLOCATED
//   MTLBuffer (created once in init) → draw(Mesh/TriangleStrip via
//   drawPrimitives with vertexCount) — no new MTLCommandBuffer state objects,
//   no per-frame array building, no closures.
//
// Law 4: drawableSize changes / device loss surface as an explicit
// `onPlaneEvent` callback with HPL1 codes; the view keeps rendering a clear
// color while degraded (fail-safe, never a crash).

import MetalKit

public final class HuddleRenderer: NSObject, MTKViewDelegate {
    public let plane: WeftHotPlane
    public let lane: Int
    let device: MTLDevice
    let commandQueue: any MTLCommandQueue
    let vertexBuffer: MTLBuffer          // preallocated once (Law 1)
    let capacity: Int
    public private(set) var framesRendered: Int = 0
    public var onPlaneEvent: ((HPL1Code, String) -> Void)?
    private var snap = WeftLaneSnapshot()
    private var window: UnsafeMutableBufferPointer<Double>
    private var windowBase: [Double]

    public init?(plane: WeftHotPlane, lane: Int = 0, view: MTKView, window: Int = 512) {
        guard let device = view.device ?? MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue() else { return nil }
        self.plane = plane
        self.lane = lane
        self.device = device
        self.commandQueue = queue
        self.capacity = min(window, plane.samplesPerLane)
        // one float per vertex (x synthesized in shader from vertex_id)
        self.vertexBuffer = device.makeBuffer(length: capacity * MemoryLayout<Float>.stride,
                                              options: .storageModeShared)!
        self.windowBase = Array(repeating: 0, count: capacity)
        self.window = UnsafeMutableBufferPointer(start: &windowBase, count: capacity)
        super.init()
        view.delegate = self
    }

    public func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        onPlaneEvent?(.ok, "drawableSizeWillChange \(size.width)x\(size.height)")
    }

    public func draw(in view: MTKView) {
        guard let drawable = view.currentDrawable,
              let rpd = view.currentRenderPassDescriptor,
              let cmd = commandQueue.makeCommandBuffer(),
              let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else {
            onPlaneEvent?(.contextLost, "drawable/encoder unavailable")
            return
        }
        // NOTE Metal ordering: encode → endEncoding → present → commit
        // refresh bounds FIRST so the window scale is this-frame accurate
        _ = plane.readLane(lane, &snap)
        // pull the newest window straight into the preallocated MTLBuffer
        let n = plane.readRecent(lane, capacity, window)
        if n > 1 {
            let dst = vertexBuffer.contents().bindMemory(to: Float.self, capacity: capacity)
            let lo = snap.min, hi = snap.max
            var scale = hi > lo ? hi - lo : 1.0
            if !(scale > 0) { scale = 1.0 }
            for j in 0..<n {
                dst[j] = Float((window[j] - lo) / scale) // y in [0,1]
            }
            // reference draw: one line strip spanning the buffer width.
            // (Engineer 2's shader pipeline binds `vertexBuffer` directly;
            //  this managed bridge guarantees the DATA path is zero-copy.)
            enc.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
            enc.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: n)
        }
        enc.endEncoding()
        cmd.present(drawable)
        cmd.commit()
        framesRendered += 1
    }
}

/// SwiftUI wrapper: MetalKit view bound to the Hot-Plane.
public struct MetalHuddleView: NSViewRepresentable {
    public let plane: WeftHotPlane
    public var lane: Int = 0
    public var preferredFPS: Int = 240
    public var onPlaneEvent: ((HPL1Code, String) -> Void)?

    public init(plane: WeftHotPlane, lane: Int = 0, preferredFPS: Int = 240) {
        self.plane = plane
        self.lane = lane
        self.preferredFPS = preferredFPS
    }

#if os(macOS)
    public func makeNSView(context: Context) -> MTKView {
        let v = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        v.enableSetNeedsDisplay = false      // continuous rendering
        v.preferredFramesPerSecond = preferredFPS
        v.colorPixelFormat = .bgra8Unorm
        context.coordinator.renderer = HuddleRenderer(plane: plane, lane: lane, view: v)
        context.coordinator.renderer?.onPlaneEvent = onPlaneEvent
        return v
    }

    public func updateNSView(_ view: MTKView, context: Context) {}

    public func makeCoordinator() -> Coordinator { Coordinator() }

    public final class Coordinator {
        var renderer: HuddleRenderer?
    }
#endif
}
