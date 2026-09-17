// MetalProbeTests.swift — CI gates for the real MTKView cadence probe.
//
// The probe (Sources/WeftSwiftUI/MTKViewCadenceProbe.swift) renders real
// MTKView frames with a live kernel stream (publish on a writer thread,
// claim inside draw(in:)). These tests pin the contract that survives any
// environment:
//
//   P1  The probe RUNS end-to-end on a Metal-capable host: frames actually
//       rendered, every draw(in:) performed a kernel claim (the draw-phase
//       read proof), zero torn frames accepted under display pacing.
//   P2  The 120 Hz path is REQUESTED and the view's plumbing accepts it
//       (preferredFramesPerSecond sticks) — the ProMotion setup gate.
//   P3  The report carries honest environment tagging — a VM/simulator
//       measurement can never pass for a device number.
//
// WHAT IS DELIBERATELY NOT ASSERTED: a measured 120 fps. VMs and simulators
// cap the display link; the measured rate is recorded, published, and
// compared across environments (the RFC-0007 CMP rig consumes the same
// numbers), but the device-rate banner flips only on real ProMotion
// hardware. Honesty over optics — the repo's Law 4 posture.

import XCTest
import WeftCore
import WeftSwiftUI

#if canImport(MetalKit) && !os(watchOS)
import MetalKit // MTKView; also re-exports Metal symbols on Apple SDKs
#endif
#if canImport(Metal) && !os(watchOS)
import Metal // MTLCreateSystemDefaultDevice (defining module, not transitive)
#endif

#if canImport(MetalKit) && !os(watchOS)
final class MetalProbeTests: XCTestCase {

    /// P1: end-to-end probe with a live kernel stream.
    func testProbeRendersFramesWithDrawPhaseClaimsAndZeroTorn() throws {
        guard MTLCreateSystemDefaultDevice() != nil else {
            throw XCTSkip("no Metal device on this host — declared skip, "
                          + "not a pass (the runner's GPU story is the story)")
        }
        let report = MTKViewCadenceProbe.run(
            frames: 120, requestedFPS: 120,
            withKernelStream: true, payloadWords: 64, timeout: 20)

        XCTAssertTrue(report.ok,
                      "probe must render frames, claim in every draw phase, "
                      + "and accept zero torn frames — got \(report)")
        XCTAssertEqual(report.claimsInDrawPhase, report.framesRendered,
                       "every draw(in:) must perform the draw-phase claim")
        XCTAssertEqual(report.tornAccepted, 0,
                       "no torn/corrupt frame may be accepted under pacing")
        XCTAssertGreaterThan(report.measuredFPS, 0,
                             "a measured rate must exist when frames rendered")
        XCTAssertEqual(report.deviceName != nil, true)
    }

    /// P2: the ProMotion setup path — requested cadence sticks.
    func testProbeRequestsAndAccepts120HzPath() throws {
        guard MTLCreateSystemDefaultDevice() != nil else {
            throw XCTSkip("no Metal device on this host — declared skip")
        }
        let report = MTKViewCadenceProbe.run(
            frames: 60, requestedFPS: 120, withKernelStream: false, timeout: 20)
        XCTAssertTrue(report.preferredAccepted,
                      "view must accept preferredFramesPerSecond=120 "
                      + "(the ProMotion plumbing gate); notes: \(report.notes)")
        XCTAssertEqual(report.requestedFPS, 120)
    }

    /// P3: environment honesty — the tag must name the environment class,
    /// and the notes must disclose the rate-cap reality of non-device hosts.
    func testProbeReportCarriesHonestEnvironmentTags() throws {
        guard MTLCreateSystemDefaultDevice() != nil else {
            throw XCTSkip("no Metal device on this host — declared skip")
        }
        let report = MTKViewCadenceProbe.run(
            frames: 30, requestedFPS: 120, withKernelStream: true, timeout: 20)
        #if targetEnvironment(simulator)
        XCTAssertEqual(report.environment, "iOS-simulator")
        XCTAssertTrue(report.notes.contains { $0.contains("simulator") },
                      "simulator runs must disclose the simulated-GPU caveat")
        #else
        XCTAssertEqual(report.environment, "macOS-runner")
        XCTAssertTrue(report.notes.contains { $0.contains("display link") },
                      "non-device runs must disclose the rate cap source")
        #endif
    }

    /// The measured-vs-requested distinction is the honesty load-bearing
    /// wall: on a host that cannot reach 120, the report must still carry
    /// BOTH numbers so reviewers see the gap instead of a green lie.
    func testReportCarriesMeasuredAndRequestedRates() throws {
        guard MTLCreateSystemDefaultDevice() != nil else {
            throw XCTSkip("no Metal device on this host — declared skip")
        }
        let report = MTKViewCadenceProbe.run(
            frames: 60, requestedFPS: 120, withKernelStream: true, timeout: 20)
        XCTAssertEqual(report.requestedFPS, 120)
        // measuredFPS is whatever the host's display link drove — recorded,
        // published, never fudged toward the requested number.
        XCTAssertGreaterThanOrEqual(report.measuredFPS, 0)
        if report.measuredFPS > 0 && report.measuredFPS < 100 {
            XCTAssertTrue(report.notes.contains { $0.contains("display link") }
                          || report.notes.contains { $0.contains("simulator") },
                          "a sub-120 measurement on a non-device host must "
                          + "carry the environment caveat")
        }
    }
}
#endif
