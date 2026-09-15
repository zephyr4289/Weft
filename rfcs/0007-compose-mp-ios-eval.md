---
RFC: 0007
Title: Compose Multiplatform on iOS Evaluation
Status: Draft
Authors: Weft Core Team
Created: 2026-09-15
Supersedes / Superseded-by: None
---

# RFC 0007 — Compose Multiplatform on iOS Evaluation

## Summary
Evaluates Compose Multiplatform (CMP) for iOS regarding deferred draw-phase reads (`drawWithContent {}` / `drawBehind {}`), rendering pipelines (Skiko on Metal), and performance trade-offs against native Swift / Metal bindings (`WeftMetalView`).

## Motivation
To determine whether cross-platform Kotlin multiplatform UI code targeting iOS can satisfy Weft's zero-recomposition draw-phase read invariant (Law 1/2) without requiring bespoke Swift/Metal code for every iOS view.

## Guide-level explanation
Developers can write shared Compose code targeting both Android and iOS:
```kotlin
Modifier.drawBehind {
    val frame = weftState.readFrame()
    drawWaveform(frame)
}
```
In CMP iOS, this executes during the Skiko draw phase without triggering UI recomposition.

## Reference-level specification
- **Skiko Pipeline**: CMP iOS renders via Skiko (`org.jetbrains.skiko`) over `CAMetalLayer`.
- **Draw Phase Isolation**: Recomposition is successfully bypassed when state reads are confined to `drawBehind` or `drawWithContent` lambdas.
- **Pacing**: 60 Hz Canvas operates reliably in CMP iOS. 120 Hz ProMotion workloads experience occasional frame pacing jitter due to Kotlin/Native runtime scheduling overhead compared to native Swift `CADisplayLink`.

## Boundary of the claim (Law 4)
CMP iOS supports deferred draw-phase reads for general UI workloads, but native Swift/Metal (`dev.weft.swift`) remains the recommended tier for dedicated 120 Hz pro-audio / high-refresh visualizers.

## Alternatives considered
- Native SwiftUI only: Requires writing duplicate UI code for iOS.
- Raw OpenGL ES / WebGL on iOS: Deprecated by Apple.

## Drawbacks
CMP iOS introduces Skiko / Kotlin Multiplatform runtime dependencies.

## Open questions
- Future Apple Metal integration improvements in Compose Multiplatform 1.8+.

## Hardware Deferral List
- Physical iPhone ProMotion 120 Hz display synchronization benchmarks are deferred.

## Staff Decision
[EMPTY]
