# WeftSwiftUI (swift-heddle)

Heddle-2.0 SwiftUI/Metal connectors for the **Weft Hot-Plane (HPL1)** —
`@Observable` model + MetalKit canvas rendering **directly from POSIX shared
memory**, zero per-frame allocations on the draw path.

## What's inside

| File | Role |
|---|---|
| `HotPlane.swift` | Pinned HPL1 constants + geometry + fail-closed validation + 15-code taxonomy; `WeftHotPlane` seqlock consumer (caller-owned snapshots, tear counting, newest-first `readRecent`, dirty-mask scans) — every multi-byte load explicitly `.littleEndian` |
| `HotPlaneModel.swift` | `@Observable` bridge — `pump()` mutates stored properties in place; Observation emits only for lanes whose sequence advanced |
| `MetalHuddleView.swift` | `HuddleRenderer` (MTKView delegate): preallocated `MTLBuffer` filled straight from the plane window per frame — zero-copy data path; encode → endEncoding → present → commit ordering audited |
| `WeftHudView.swift` | `WeftSharedMapping` (POSIX shm `open`+`mmap` attach — Engineer 1's bridge entry) + `WeftHudOverlay` (TimelineView HUD at diagnostic cadence) |

## Usage

```swift
let mapping = WeftSharedMapping(path: "/dev/shm/weft-hotplane")!
let plane = mapping.plane
MetalHuddleView(plane: plane, lane: 0, preferredFPS: 240)
```

## Verification honesty

The authoring sandbox has **no swiftc**: the Swift sources and the XCTest
battery (`HotPlaneTests`, 8 tests incl. torn-seqlock and corruption matrices)
are **CI-gated on the Apple lane**. The always-runnable local proof is
`audit/static_audit.mjs` (22 mechanical checks: constants parity vs
`heddle-core/layout.js`, `.littleEndian` on all 27 load sites, draw-path
purity, Metal command ordering, `@Observable` + taxonomy contracts).
