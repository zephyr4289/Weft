import XCTest
@testable import WeftSwiftUI

final class CadenceTests: XCTestCase {

    func testAutoSelectionStandardDisplay() {
        let detector = CadenceDetector(maxRefreshRate: 60.0)
        let resolved = detector.resolve(mode: .auto)
        XCTAssertEqual(resolved, .canvas)
        XCTAssertEqual(detector.preferredFPS(for: resolved), 60)
    }

    func testAutoSelectionProMotionDisplay() {
        let detector = CadenceDetector(maxRefreshRate: 120.0)
        let resolved = detector.resolve(mode: .auto)
        XCTAssertEqual(resolved, .metal)
        XCTAssertEqual(detector.preferredFPS(for: resolved), 120)
    }

    func testCanvasOverrideOnProMotionDisplay() {
        let detector = CadenceDetector(maxRefreshRate: 120.0)
        let resolved = detector.resolve(mode: .canvas60)
        XCTAssertEqual(resolved, .canvas)
        XCTAssertEqual(detector.preferredFPS(for: resolved), 60)
    }

    func testMetalOverrideOnStandardDisplay() {
        let detector = CadenceDetector(maxRefreshRate: 60.0)
        let resolved = detector.resolve(mode: .metal120)
        XCTAssertEqual(resolved, .metal)
        XCTAssertEqual(detector.preferredFPS(for: resolved), 60)
    }

    func testHighRefreshRateProMotion144Hz() {
        let detector = CadenceDetector(maxRefreshRate: 144.0)
        let resolved = detector.resolve(mode: .auto)
        XCTAssertEqual(resolved, .metal)
        XCTAssertEqual(detector.preferredFPS(for: resolved), 120)
    }
}
