# DIRECTIVE-13 — iOS/macOS packaging: Swift Package Manager + dual-path Heddle (60/120 Hz)

- Wave: 1 · Depends: D-10 · Effort: ~8 h · Status: ISSUED · Constraint: macOS runner

## 1. Context

The Swift port (`core/swift/{Steward,Heddle,Weft}.swift`) is verified source-only (WO-P4 T4): `swift-atomics` with `.acquiringAndReleasing` == C AcqRel (no divergence, per PORTS.md §2). Production distribution = `Weft` on the Swift Package Index. Per the owner pivot: **simulator/build verification only; the ProMotion 120 Hz path ships design-verified, never device-benchmarked.**

## 2. Tasks

- **T13.1 Package.swift.** swift-tools 5.9; targets: `CWeft` (compiles FROZEN `core/c/weft.c` directly — no kernel diffs), `WeftCore` (Swift sources from `core/swift`), `WeftSwiftUI` (views). Dependency: `apple/swift-atomics` pinned to 1.x (SPM lockfile committed).
- **T13.2 Dual-path Heddle view.** `WeftHeddleView(renderer:)`:
  - `.canvas` path: SwiftUI `Canvas`/CoreGraphics, frame capped 60 Hz (CADisplayLink-gated draw invalidation);
  - `.metal` path: `MTKView` + `CADisplayLink`, `preferredFramesPerSecond = maximum` for ProMotion 120 Hz;
  - automatic runtime switch by observed display cadence + explicit user override; selection logic unit-tested (no device needed to test the *decision*, only the frame pacing itself).
- **T13.3 Steward integration.** `@StateObject`/observation-scoped Steward; teardown ordering mirrors the Compose disposal discipline (single dispose, no leak); ARC reclaim maps to buffer-reference `nil` (PORTS.md I6 row) — assert in unit tests.
- **T13.4 Example app.** iOS example (W2-class animated workload) building for **iOS Simulator**; scheme `WeftExample-iOS`; README documents the 60/120 switch and carries the honesty label: frame pacing on hardware is UNVERIFIED by design of this program.
- **T13.5 CI.** GitHub Actions `macos-14` runner: `swift build` + `swift test` (macOS) + `xcodebuild -destination 'platform=iOS Simulator,name=iPhone 15'` build-only for the example. If runner quota blocks, the directive gates but does not block the series — declare, don't absorb.

## 3. Non-goals

No kernel diffs. No real-device runs, no XCTest on hardware, no App Store/TestFlight. No CocoaPods (SPM only). No Metal shader authoring beyond the passthrough needed for the MTKView path (shader binding generator is RFC Q1 material, D-17).

## 4. Acceptance criteria (mechanical)

1. `swift build` + `swift test` green on macOS (`macos-14` tag), atomics dependency resolved from lockfile.
2. Example app builds for iOS Simulator (xcodebuild exit 0, log retained).
3. Renderer selection: unit tests cover cadence detection + override (≥ 4 cases).
4. Steward disposal: unit test proves exactly-once teardown and ARC reclaims (buffer reference nil) on drop.
5. `swift package dump-package` shows pinned `swift-atomics` 1.x.

## 5. Evidence to return

`evidence/D-13/`: swift build/test logs, xcodebuild log, Package.swift + Package.resolved, unit test report, example app tree listing.

## 6. Report

`reports/D-13-REPORT.md` per index §4. Include a "what is NOT verified without hardware" section (frame pacing, thermal, ProMotion persistence) — this section is mandatory, not optional.
