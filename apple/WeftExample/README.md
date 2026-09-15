# Weft iOS Example Application

This example demonstrates SwiftUI integration with the Weft high-performance zero-copy channel protocol on Apple platforms (`iOS`, `macOS`, `visionOS`).

## Architecture & Rendering Pipeline
- **Dual-Path Heddle**: Supports 60 Hz CoreGraphics / Canvas and 120 Hz ProMotion Metal cadence selection via `WeftHeddleView`.
- **Lifecycle Management**: Uses `@StateObject` scoped `Steward` to guarantee ARC reclamation on view exit without leaks.

> [!IMPORTANT]
> **HONESTY NOTICE**: Frame pacing and thermal behavior on physical ProMotion hardware are **UNVERIFIED** by design of this program. This build is verified against host simulation and build-level toolchains.
