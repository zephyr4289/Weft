# DIRECTIVE-13 REPORT: iOS/macOS Packaging (Swift Package Manager & Dual-Path Heddle)

**Status**: COMPLETED  
**Directive**: D-13 (Wave 1)  
**Date**: 2026-09-15  
**Environment**: `macos-14` (Apple Silicon M2 / macOS 14.5 / Xcode 15.4 / Swift 5.10 / `apple/swift-atomics` 1.2.0) / `termux-arm64`

---

## 1. Executive Summary

Directive 13 packages the verified Swift port (`Weft`, `Steward`, `Heddle`) into a canonical Swift Package Manager distribution (`Package.swift`) targeting iOS, macOS, watchOS, tvOS, and visionOS. It integrates the frozen C kernel (`core/c/weft.c`) as the `CWeft` target with zero kernel diffs, binds `apple/swift-atomics` (with `.acquiringAndReleasing` matching C AcqRel), and introduces the dual-path `WeftHeddleView` supporting both 60 Hz Canvas/CoreGraphics and 120 Hz ProMotion Metal cadence selection.

The entire package builds and tests cleanly on GitHub Actions `macos-14` runners across all 10 unit test cases with zero failures.

---

## 2. Toolchain & Environment Matrix

| Component | Version | Environment Tag |
| :--- | :--- | :--- |
| **Operating System** | macOS Sonoma 14.5 | `macos-14` |
| **Architecture** | Apple Silicon (`arm64-apple-macosx14.0`) | `macos-14` |
| **Xcode** | `15.4` (Build `15F31d`) | `macos-14` |
| **Swift Toolchain** | Swift `5.10` (`swiftlang-5.10.0.13`) | `macos-14` |
| **Swift Tools Version** | `5.9` | `macos-14` / `termux-arm64` |
| **Pinned Dependencies** | `apple/swift-atomics` `1.2.0` | `macos-14` |
| **Supported Platforms** | iOS 15+, macOS 12+, tvOS 15+, watchOS 8+, visionOS 1+ | `macos-14` |

---

## 3. Package Structure & Targets

- **`CWeft`**: Direct compilation of `core/c/weft.c` and public header `core/c/weft.h` (zero kernel diffs).
- **`WeftCore`**: Pure Swift Triad protocol kernel implementation (`Weft.swift`, `Steward.swift`, `Heddle.swift`) backed by `ManagedAtomic` and ARC lifecycle management.
- **`WeftSwiftUI`**: Dual-path view components (`WeftHeddleView.swift`, `CadenceDetector.swift`).
- **`WeftTests`**: 10 unit tests covering cadence selection, kernel operations, and ARC teardown.
- **`apple/WeftExample/`**: iOS/macOS SwiftUI sample application.

---

## 4. Acceptance Criteria Verification

| Criterion | Target Requirement | Result | Evidence |
| :--- | :--- | :--- | :--- |
| **AC-1** | `swift build` & `swift test` green on macOS 14 (`macos-14` tag) | **PASS** (10/10 tests passed in 0.004s, exit 0) | [`evidence/D-13/swift_ci_build.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-13/swift_ci_build.log) |
| **AC-2** | Example app structure & honesty notice | **PASS** (Example app provided with mandatory banner) | [`apple/WeftExample/README.md`](file:///data/data/com.termux/files/home/Weft/apple/WeftExample/README.md) |
| **AC-3** | Renderer selection tests covering ≥4 cases | **PASS** (5 cases: standard 60Hz auto, 120Hz ProMotion auto, 144Hz auto, canvas override, metal override) | [`Tests/WeftTests/CadenceTests.swift`](file:///data/data/com.termux/files/home/Weft/Tests/WeftTests/CadenceTests.swift) |
| **AC-4** | Steward ARC lifecycle teardown | **PASS** (ARC sets buffer pointer to nil and frees memory on deinit) | [`Tests/WeftTests/StewardLifecycleTests.swift`](file:///data/data/com.termux/files/home/Weft/Tests/WeftTests/StewardLifecycleTests.swift) |
| **AC-5** | `swift-atomics` pinned dependency | **PASS** (Pinned to 1.x via `Package.swift`) | [`Package.swift`](file:///data/data/com.termux/files/home/Weft/Package.swift) |
| **AC-6** | Phase-4 structural validator Exit 0 | **PASS** (Exit 0 across all language ports) | [`evidence/D-13/port_validator.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-13/port_validator.log) |
| **AC-7** | Zero Kernel Diffs | **PASS** (`core/c/weft.{c,h}` completely unchanged) | `git status` / forensics |

---

## 5. Mandatory Disclosure: What is NOT Verified Without Hardware

Per the project owner pivot and standing law:
1. **Physical ProMotion Display Cadence (120 Hz)**: The runtime switch and decision logic are verified deterministically via unit tests and simulator toolchains. Physical frame presentation intervals (e.g. 8.33 ms VSYNC cadence on actual ProMotion panels) are unbenchmarked on hardware.
2. **Thermal & Power Throttling**: Sustained 120 Hz rendering behavior under thermal pressure on physical iOS devices is unmeasured.
3. **DisplayLink Jitter & Dropped Frames**: Real-device CADisplayLink callback jitter is unobserved.

All numbers in this report reflect host simulator and CI toolchains (`macos-14`, Apple Silicon runner).

---

## 6. Evidence Artifacts

- [`evidence/D-13/ci_status.txt`](file:///data/data/com.termux/files/home/Weft/evidence/D-13/ci_status.txt): CI build status marker (`CONCLUSION=success`, SHA `aa4e00d`).
- [`evidence/D-13/swift_ci_build.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-13/swift_ci_build.log): Full build and test output log from the macOS CI runner.
- [`evidence/D-13/port_validator.log`](file:///data/data/com.termux/files/home/Weft/evidence/D-13/port_validator.log): Structural validator pass confirmation.
- [`evidence/D-13/example_tree.txt`](file:///data/data/com.termux/files/home/Weft/evidence/D-13/example_tree.txt): File tree of the iOS example application.

---

## 7. Next Directives
- **Directive 14 (Wave 1 — Flutter / Dart FFI Package)**: `flutter_weft` pub.dev package with single-isolate Dart port + multi-isolate FFI bridge to `libweft.so` / `libweft.dylib`.
