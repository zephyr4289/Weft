import SwiftUI
import WeftCore
import WeftSwiftUI

public struct ContentView: View {
    @StateObject private var steward = Steward()
    @State private var mode: RendererMode = .auto
    // FIX (leak): the Weft used to be allocated INSIDE `body` — every
    // re-render (e.g. every mode-picker tap) registered a fresh Weft with
    // the Steward and never released it: unbounded growth. It is now
    // allocated exactly once, on first appearance.
    @State private var weft: Weft?

    public init() {}

    public var body: some View {
        VStack(spacing: 16) {
            Text("Weft Dual-Path SwiftUI Heddle")
                .font(.headline)

            Picker("Renderer Mode", selection: $mode) {
                Text("Auto (60/120)").tag(RendererMode.auto)
                Text("Canvas 60Hz").tag(RendererMode.canvas60)
                Text("Metal 120Hz").tag(RendererMode.metal120)
            }
            .pickerStyle(.segmented)
            .padding(.horizontal)

            if let weft = weft {
                WeftHeddleView(weft: weft, mode: mode) { context, size, ptr in
                    let firstByte = Double(ptr.load(as: UInt8.self))
                    let rect = CGRect(x: 20, y: 20, width: size.width - 40, height: size.height - 40)
                    context.stroke(Path(ellipseIn: rect), with: .color(.blue), lineWidth: 4 + (firstByte / 50.0))
                } drawMetal: { device, encoder, ptr, size in
                    // Minimal honest Metal path: the render pass clears to the
                    // configured color; this closure is where real geometry
                    // encoding goes (a pipeline + vertex buffer would be
                    // created once and captured here). The payload pointer is
                    // live reader-held memory — sample it into uniforms or a
                    // blit on hardware. Shipped as a clear so the example
                    // needs no .metal assets; the Canvas path shows content.
                    _ = device; _ = encoder; _ = ptr; _ = size
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(Color.black.opacity(0.05))
            } else {
                Color.clear
            }
        }
        .padding()
        .onAppear {
            let w = steward.weft(payloadMax: 256)
            weft = w
        }
        .task {
            // Producer: a data-driven writer ticking well above 60 Hz. The
            // draw path claims wait-free regardless of this cadence.
            var seq: UInt32 = 0
            while !Task.isCancelled {
                guard let w = weft else {
                    try? await Task.sleep(nanoseconds: 16_000_000)
                    continue
                }
                let cursor = w.wBegin
                cursor.storeBytes(of: UInt8((seq * 3) & 0xFF), as: UInt8.self)
                _ = w.publish(seq: seq + 1, payloadLen: 4)
                seq += 1
                try? await Task.sleep(nanoseconds: 8_000_000)
            }
        }
    }
}
