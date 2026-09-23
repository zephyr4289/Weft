// WeftHudView.swift — SwiftUI HUD overlay (fail-safe diagnostics) + POSIX shm
// attach helper for the Hot-Plane (Engineer 1's bridge entry point).

import SwiftUI
import Darwin

#if os(macOS)
/// Attach an HPL1 plane from a POSIX shared-memory object (or any file path).
/// The mapping stays alive as long as `WeftSharedMapping` is retained.
public final class WeftSharedMapping {
    public let plane: WeftHotPlane
    private let ptr: UnsafeMutableRawPointer
    private let length: Int
    private let fd: Int32

    public init?(path: String, maxTries: Int = 64) {
        let cPath = path.cString(using: .utf8)!
        fd = open(cPath, O_RDWR)
        guard fd >= 0 else { return nil }
        var st = stat()
        guard fstat(fd, &st) == 0, st.st_size >= Int64(HPL1.headerSize) else {
            close(fd)
            return nil
        }
        length = Int(st.st_size)
        guard let p = mmap(nil, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0),
              p != MAP_FAILED else {
            close(fd)
            return nil
        }
        ptr = p
        let buf = UnsafeRawBufferPointer(start: p, count: length)
        do {
            plane = try WeftHotPlane(bytes: buf)
        } catch {
            munmap(p, length)
            close(fd)
            return nil
        }
    }

    deinit {
        munmap(ptr, length)
        close(fd)
    }
}
#endif

/// SwiftUI diagnostic overlay mirroring <WeftHud /> (React). Reads the model
/// at HUD cadence (≤ 10 Hz) via TimelineView — never at 240 Hz.
public struct WeftHudOverlay: View {
    @State private var model: HotPlaneModel
    private var refresh: Int = 4

    public init(model: HotPlaneModel, refreshHz: Int = 4) {
        _model = State(initialValue: model)
        refresh = refreshHz
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HudRow(label: "FPS(prod)", value: "\(Int(model.globalAvg)) avg")
            HudRow(label: "TEARS", value: "\(model.tears)")
            HudRow(label: "DIRTY", value: "\(model.dirtyCount)")
            HudRow(label: "STATUS", value: model.lastEvent == .ok ? "OK" : "HPL1 \(model.lastEvent.rawValue)")
        }
        .padding(8)
        .background(.black.opacity(0.82), in: RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.gray.opacity(0.35)))
        .task {
            while !Task.isCancelled {
                model.pump()
                try? await Task.sleep(nanoseconds: UInt64(1e9 / Double(refresh)))
            }
        }
    }
}

struct HudRow: View {
    let label: String
    let value: String
    var body: some View {
        HStack {
            Text(label).foregroundStyle(.secondary).font(.system(size: 11, design: .monospaced))
            Spacer()
            Text(value).font(.system(size: 11, design: .monospaced))
        }
        .frame(width: 150)
    }
}
