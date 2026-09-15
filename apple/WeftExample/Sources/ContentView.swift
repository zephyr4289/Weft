import SwiftUI
import WeftCore
import WeftSwiftUI

public struct ContentView: View {
    @StateObject private var steward = Steward()
    @State private var mode: RendererMode = .auto

    public init() {}

    public var body: some View {
        let weft = steward.weft(payloadMax: 256)

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

            WeftHeddleView(weft: weft, mode: mode) { context, size, ptr in
                let firstByte = Double(ptr.load(as: UInt8.self))
                let rect = CGRect(x: 20, y: 20, width: size.width - 40, height: size.height - 40)
                context.stroke(Path(ellipseIn: rect), with: .color(.blue), lineWidth: 4 + (firstByte / 50.0))
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color.black.opacity(0.05))
        }
        .padding()
    }
}
