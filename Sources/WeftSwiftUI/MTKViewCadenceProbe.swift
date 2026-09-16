// MTKViewCadenceProbe.swift — the real 120 Hz MTKView cadence PROBE.
//
// WHY EXISTS: WHITEPAPER §8.3 publishes the two-path honesty (Canvas at
// 60 Hz, MTKView at 120 Hz on ProMotion) and WeftHeddleView implements the
// Metal path — but until now "no on-device 120 Hz measurement exists
// anywhere in the repo" (the view's own HONESTY NOTICE). A cadence claim
// without a measurement is a comment, not a fact. This probe turns the
// claim into a falsifiable number:
//
//   1. An off-screen MTKView is configured at the requested cadence
//      (preferredFramesPerSecond = 120 — the ProMotion path).
//   2. Its delegate renders N frames; each draw(in:) performs the FULL
//      draw-phase contract: claim the kernel, read the READER-HELD buffer
//      live (rLivePtr — A3, never a snapshot), verify the payload against
//      the canonical pat() pattern (04-LITMUS §0.1).
//   3. A concurrent writer publishes fresh pat()-filled frames as fast as
//      the display link consumes them — the same 1W/1R stream a real
//      visualizer runs.
//   4. Frame timestamps (CACurrentMediaTime) yield measured FPS, median
//      and p95 frame intervals; the report carries the honest environment
//      note so a VM or simulator number can never masquerade as a device
//      number.
//
// WHAT IS PROVEN WHERE: the probe mechanics (Metal device, MTKView pacing
// request, draw-phase claim loop, zero torn frames over N frames) run in
// CI on macOS (Metal-capable runners) and on the iOS simulator leg. The
// 120 Hz RATE ITSELF is proven only where a ProMotion display exists —
// the report distinguishes "requested cadence accepted" (plumbing proven)
// from "measured 120 fps" (device-proven). Banners stay honest per that
// split; the device-rate banner flips only on real-device evidence.
//
// STATUS: CI-PROVEN (macOS runner + iOS-simulator legs); ProMotion device
// rate still deferred (Hardware Deferral List, WHITEPAPER §8.3).

import Foundation
import CoreVideo
import WeftCore

#if canImport(MetalKit) && !os(watchOS)
import MetalKit

/// Immutable probe result — every number a reviewer can demand, plus the
/// environment honesty block.
public struct CadenceProbeReport {
    /// Cadence requested from the view (120 = ProMotion path).
    public let requestedFPS: Int
    /// Frames the view actually rendered inside the timeout.
    public let framesRendered: Int
    /// measured frames / total time across the measured window.
    public let measuredFPS: Double
    /// Median gap between consecutive frames (ms).
    public let medianIntervalMs: Double
    /// 95th-percentile gap (ms) — the pacing-jitter surface RFC-0007
    /// measures for the CMP rig comparison.
    public let p95IntervalMs: Double
    /// draw(in:) invocations that performed a kernel claim — the
    /// draw-phase read proof (must equal framesRendered).
    public let claimsInDrawPhase: Int
    /// Fresh claims whose payload FAILED the pat() check — must be 0
    /// (the kernel's latest-wins contract under display pacing).
    public let tornAccepted: Int
    /// Whether the view accepted the requested cadence (plumbing proof —
    /// NOT a device-rate proof; VMs and sims cap the measured rate).
    public let preferredAccepted: Bool
    /// MTLDevice.name, or nil when no device exists.
    public let deviceName: String?
    /// Honest environment tag: "macOS-runner", "iOS-simulator",
    /// "macOS-device", "iOS-device" — best effort, never claimed deeper
    /// than the process can prove.
    public let environment: String
    /// Reviewer notes (missing device, timeout, capped rate...).
    public let notes: [String]

    public var ok: Bool {
        framesRendered > 0 && claimsInDrawPhase == framesRendered && tornAccepted == 0
    }
}

public final class MTKViewCadenceProbe: NSObject, MTKViewDelegate {

    private let targetFrames: Int
    private let weft: Weft?
    private let writer: DispatchQueue?

    // State mutated from the display-link thread (draw(in:)) and read from
    // the main thread (runloop pump) — guarded, not "benign".
    private let lock = NSLock()
    private var timestamps: [Double] = []
    private var claims = 0
    private var torn = 0
    private var stopped = false

    private init(targetFrames: Int, weft: Weft?, writer: DispatchQueue?) {
        self.targetFrames = targetFrames
        self.weft = weft
        self.writer = writer
        super.init()
    }

    private var frameCount: Int {
        lock.lock(); defer { lock.unlock() }
        return timestamps.count
    }

    private var isStopped: Bool {
        lock.lock(); defer { lock.unlock() }
        return stopped
    }

    private func requestStop(_ view: MTKView) {
        lock.lock()
        stopped = true
        lock.unlock()
        view.isPaused = true
    }

    /// Run the probe: N frames at the requested cadence, with an optional
    /// live kernel stream (publish/claim under display pacing). Off-screen
    /// — no window, no user interaction, CI-safe.
    ///
    /// - Returns a report even on failure paths (no Metal device, timeout):
    ///   the caller inspects `ok` + `notes`; the probe never throws.
    public static func run(
        frames: Int = 120,
        requestedFPS: Int = 120,
        withKernelStream: Bool = true,
        payloadWords: Int = 64,
        timeout: TimeInterval = 15
    ) -> CadenceProbeReport {
        var notes: [String] = []

        guard let device = MTLCreateSystemDefaultDevice() else {
            return CadenceProbeReport(
                requestedFPS: requestedFPS, framesRendered: 0,
                measuredFPS: 0, medianIntervalMs: 0, p95IntervalMs: 0,
                claimsInDrawPhase: 0, tornAccepted: 0,
                preferredAccepted: false, deviceName: nil,
                environment: MTKViewCadenceProbe.environmentTag(),
                notes: ["no Metal device — probe could not run"])
        }

        // The kernel stream (optional): a Weft + a background writer
        // publishing pat()-filled frames as fast as the display consumes.
        var weft: Weft? = nil
        var writer: DispatchQueue? = nil
        if withKernelStream {
            let w = Weft(payloadMax: payloadWords * 4)
            weft = w
            writer = DispatchQueue(label: "weft.probe.writer", qos: .userInteractive)
        }

        let probe = MTKViewCadenceProbe(
            targetFrames: frames, weft: weft, writer: writer)

        let view = MTKView(frame: CGRect(x: 0, y: 0, width: 64, height: 64),
                           device: device)
        view.enableSetNeedsDisplay = false
        view.isPaused = false
        view.preferredFramesPerSecond = requestedFPS
        view.delegate = probe
        _ = view // retain for the runloop pump below

        // Background writer: fill wBegin with pat(seq, i), publish, repeat.
        // Frames advance independently of drawing — the reader (draw phase)
        // claims whatever is freshest, exactly like a real visualizer.
        if let w = weft, let q = writer {
            q.async { [weak probe] in
                var seq: UInt32 = 0
                while true {
                    guard let p = probe, !p.isStopped else { break }
                    seq += 1
                    let cursor = w.wBegin
                    let bytes = payloadWords * 4
                    for i in 0..<bytes {
                        cursor.storeBytes(of: pat(seq: seq, i: UInt32(i)),
                                          as: UInt8.self,
                                          toByteOffset: i)
                    }
                    _ = w.publish(seq: seq, payloadLen: UInt32(bytes))
                }
            }
        }

        // Pump the runloop until N frames render or the timeout fires.
        let started = Date()
        while probe.frameCount < frames,
              Date().timeIntervalSince(started) < timeout {
            RunLoop.current.run(mode: .default,
                                before: Date().addingTimeInterval(0.005))
        }
        if probe.frameCount < frames {
            notes.append("timeout after \(Int(timeout))s at \(probe.frameCount)/\(frames) frames")
        }
        probe.requestStop(view)

        probe.lock.lock()
        let stamps = probe.timestamps
        let claimCount = probe.claims
        let tornCount = probe.torn
        probe.lock.unlock()

        let elapsed = stamps.count >= 2
            ? (stamps.last! - stamps.first!)
            : 0
        let intervals = zip(stamps.dropFirst(), stamps)
            .map { ($0 - $1) * 1000.0 }
        let sorted = intervals.sorted()
        let median = sorted.isEmpty ? 0 : sorted[sorted.count / 2]
        let p95 = sorted.isEmpty
            ? 0
            : sorted[min(sorted.count - 1, Int((Double(sorted.count) * 0.95).rounded(.up)) - 1)]

        let measuredFPS = elapsed > 0
            ? Double(stamps.count - 1) / elapsed
            : 0

        // Environment honesty: a VM or simulator must never pass for a
        // device measurement.
        #if targetEnvironment(simulator)
        notes.append("iOS simulator — Metal is host-simulated; rate numbers are NOT device numbers")
        #else
        notes.append("physical/virtual GPU: rate capped by the display link driving this process")
        #endif
        if view.preferredFramesPerSecond == requestedFPS {
            notes.append("view accepted preferredFramesPerSecond=\(requestedFPS) (plumbing proven)")
        } else {
            notes.append("view clamped preferredFramesPerSecond to \(view.preferredFramesPerSecond)")
        }

        return CadenceProbeReport(
            requestedFPS: requestedFPS,
            framesRendered: stamps.count,
            measuredFPS: measuredFPS,
            medianIntervalMs: median,
            p95IntervalMs: p95,
            claimsInDrawPhase: claimCount,
            tornAccepted: tornCount,
            preferredAccepted: view.preferredFramesPerSecond == requestedFPS,
            deviceName: device.name,
            environment: MTKViewCadenceProbe.environmentTag(),
            notes: notes)
    }

    static func environmentTag() -> String {
        #if targetEnvironment(simulator)
        return "iOS-simulator"
        #elseif os(macOS)
        return "macOS-runner"
        #else
        return "iOS-device"
        #endif
    }

    // MARK: MTKViewDelegate — the draw phase under measurement

    public func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    public func draw(in view: MTKView) {
        lock.lock()
        if timestamps.count >= targetFrames {
            lock.unlock()
            requestStop(view)
            return
        }
        timestamps.append(CACurrentMediaTime())
        lock.unlock()

        // The FULL draw-phase contract, not a stub: claim, live read,
        // integrity-verify against the writer's pattern.
        if let w = weft {
            let seq = w.claim()
            lock.lock()
            claims += 1
            lock.unlock()
            if seq > 0, let ptr = w.rLivePtr(16) {
                let len = Int(w.rPayloadLen())
                var bad = false
                for i in stride(from: 0, to: len, by: 7) {
                    if ptr.load(fromByteOffset: i, as: UInt8.self)
                        != pat(seq: seq, i: UInt32(i)) {
                        bad = true
                        break
                    }
                }
                if bad {
                    lock.lock()
                    torn += 1
                    lock.unlock()
                }
            }
        }
    }
}

#endif
