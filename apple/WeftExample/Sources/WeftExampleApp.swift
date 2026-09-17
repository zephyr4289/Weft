import SwiftUI
import WeftCore
#if canImport(UIKit)
import UIKit
#endif

@main
struct WeftExampleApp: App {
    /// SERIES 7 — the memory-pressure backstop wiring (RFC-0009 work
    /// order): forward iOS's memory warning to every Weft recycler (FREE
    /// slots drop; LIVE slots are never touched — a raster mid-blend
    /// cannot lose its buffer; the next acquire lazily reallocates and
    /// counts it, per AXIOM T). One observer, installed once.
    private final class MemoryPressureBridge: ObservableObject {
        private var observer: NSObjectProtocol?
        init() {
            #if canImport(UIKit)
            observer = NotificationCenter.default.addObserver(
                forName: UIApplication.didReceiveMemoryWarningNotification,
                object: nil,
                queue: .main
            ) { _ in
                WeftMemoryPressureCenter.shared.handleMemoryWarning()
            }
            #endif
        }
        deinit {
            if let observer = observer {
                NotificationCenter.default.removeObserver(observer)
            }
        }
    }
    @StateObject private var memoryBridge = MemoryPressureBridge()

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
