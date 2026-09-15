# Swift/iOS Port — Weft

> **STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION**
>
> Per WHITEPAPER §8.6: this port is structurally validated but not compiled
> or benchmarked in-sandbox. No performance claims. Phase 6+ replaces with
> real-device numbers.

## Why exists

Implements the corrected Triad Protocol (RFC-0001 §4) for Swift/iOS.
The exchange maps to `ManagedAtomic.exchange(.acquiringAndReleasing)`
(single RMW, exact C AcqRel equivalent per WO-P4 decision 3).
Per docs/PORTS.md §2 for the full memory-model mapping table.

## Dependency

- `apple/swift-atomics` — `ManagedAtomic` with `.acquiringAndReleasing` ordering
  (WO-P4 decision 3: do not hand-roll atomics from intrinsics).

## Two-path honesty (WHITEPAPER §8.3)

SwiftUI `Canvas` is Core-Graphics-backed and is the 60 Hz path.
120 Hz on ProMotion requires `MTKView` + `CADisplayLink.preferredFrameRateRange`.
The Steward probes the device at `bind()` time; the dev writes one closure either way.

## Files

| File | Role |
|---|---|
| `Weft.swift` | Kernel: Triad exchange, envelope codec, I6 handshake |
| `Steward.swift` | Lifecycle: @StateObject-scoped, ARC-based release |
| `Heddle.swift` | Draw-phase: SwiftUI Canvas (60 Hz) + MTKView (120 Hz) |

## AXIOM T

Telemetry counters are advisory. No port logic branches on a telemetry counter.
