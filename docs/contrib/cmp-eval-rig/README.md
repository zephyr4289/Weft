# contrib/cmp-eval-rig — RFC-0007 Compose Multiplatform on iOS Evaluation Rig

## What this is

RFC 0007 (Draft, Staff Decision [EMPTY]) asks whether Compose Multiplatform
can carry Weft's draw-phase read contract on iOS, against the native
Swift/Metal path. Evaluation without an instrument is an opinion. This rig
turns the evaluation into two columns of numbers produced by the SAME
report shape on both sides:

| column | Swift (native) | CMP (this rig) |
|---|---|---|
| frames rendered | `MTKViewCadenceProbe` | `CmpDrawProbe` |
| measured FPS | same | same |
| median / p95 frame interval | same | same |
| draw-phase claims == frames | `MetalProbeTests` gate | `drawPhaseClaims` gate |
| torn accepted | must be 0 | must be 0 |
| **recompositions** | n/a (no recomposer) | **must be 0 — THE gate** |
| environment honesty | report.notes | report.notes |

The report types are frozen now so the first CMP run compares
like-for-like against the CI-proven Swift probe
(`Sources/WeftSwiftUI/MTKViewCadenceProbe.swift` + `MetalProbeTests.swift`).

## The decisive gate: recompositions == 0

The native path has no recomposer; CMP does. The whole RFC-0007 question is
whether a Weft read inside `drawBehind`/`Canvas` stays OUT of composition.
The rig counts recompositions of the host composable during the run. A
nonzero count fails the evaluation regardless of FPS — that is the Law 1/Law 2
invariant, measured, not argued.

## The cadence gate: p95, not median

RFC-0007 flags "occasional frame pacing jitter due to Kotlin/Native runtime
scheduling" on 120 Hz ProMotion workloads. Median hides jitter; p95 interval
is where it shows. The rig publishes both; the evaluation criterion is:

- `medianIntervalMs` within one frame period of the display (8.33 ms at 120 Hz)
- `p95IntervalMs` — the CMP-vs-native comparison column. Native Swift
  numbers come from `MetalProbeTests` runs on the same runner class.

## Status (honesty banners, per repo law)

- Swift side of the comparison: **CI-PROVEN** (macOS runner + iOS-sim legs).
- This rig: **SOURCE-ONLY, PENDING CMP TOOLCHAIN** — no Compose Multiplatform
  target exists in the tree yet; the rig compiles the day one lands.
- ProMotion 120 Hz device rate: **deferred** on BOTH sides
  (RFC-0007 Hardware Deferral List). Measured-vs-requested is always
  published separately; VM/sim numbers never pass for device numbers.

## How to run (when CMP lands)

1. Add the CMP target to the android/Compose module tree (or a standalone
   gradle module consuming `dev.weft.cmp`).
2. Instantiate `CmpDrawProbe` with the host's Weft read injection
   (`readFrame` = kernel claim + live view into `payloadOut`;
   `verifyPayload` = the canonical pat() stride check).
3. Render `requestedFrames = 600` (10 s at 60 Hz) on the target device.
4. Publish the `CmpCadenceReport` JSON next to the Swift probe's report in
   `litmus/evidence/cmp-eval/` and update RFC-0007's evidence section with
   both columns.
