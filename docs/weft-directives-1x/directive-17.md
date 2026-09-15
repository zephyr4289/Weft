# DIRECTIVE-17 — Protocol RFC spikes Q1–Q5 (timeboxed; kernel FROZEN)

- Wave: background (never blocks) · Depends: D-10 · Budget: ≤ 8 working days TOTAL · Status: ISSUED

## 1. Context

`ARCHITECTURE.md` §Open Questions lists Q1–Q5. The owner has elevated them to the program. Staff ruling: these are **spikes and decision memos, not features**. The kernel stays FROZEN for the entire series — nothing here touches `core/c/weft.{c,h}` or the Rust mirror. Every RFC gets a decision (accept / defer / reject) with evidence; `rfcs/TEMPLATE.md` governs; RFC numbers continue from 0002 (`0003-triad2-gpu-resident`, `0004-fanout-heddles`, `0005-verifiedweft`, `0006-reattach-policy`, `0007-compose-mp-ios-eval`).

## 2. Tasks (each: timebox → spike → memo)

- **T17.1 (Q1) GPU-Resident Mode / Triad-2 — 3 d.** Spike = WebGPU (headless wgpu/Dawn on linux) compute-writer → render-reader `GPUBuffer` ping-pong implementing exchange semantics: measure CPU-path vs GPU-path handoff latency (tag `linux-sandbox+dawn`). `AHardwareBuffer` / `MTLBuffer` integration = **design sections only** (no devices). Sketch the shader binding generator contract for AGSL/MSL/WGSL (docs). Zero-alloc steady-state analysis of the GPU path.
- **T17.2 (Q2) Multi-consumer fan-out Heddles — 2 d.** Design doc + TS userland prototype: single-writer, N-reader seqlock-style fan-out (primary canvas + minimap + diagnostic recorder + network visualizer as the four canonical readers). Prove Law 2 (zero steady-state allocations) per reader in the prototype; analyze I6 reclaim under N readers (reclaim-action per GC platform); kernel untouched — this is a driver-layer pattern until accepted.
- **T17.3 (Q4) VerifiedWeft / Authenticated Frames — 1 d.** Spec: optional per-frame HMAC header (HMAC-SHA256 primary, BLAKE3 noted) for network-sourced or untrusted IPC streams. Bench on sandbox: ns/frame at 64 B payload, OpenSSL vs monotonic-count differences (tag `x86_64-sandbox`). Sub-microsecond target: measure, don't promise; report the number with its tag.
- **T17.4 (Q5) Android Process-Death ReattachPolicy — 1 d.** Design doc + Kotlin stubs in `weft-compose`'s ReattachPolicy seam (from D-12 T12.3): memory re-hydration vs clean re-allocation decision matrix; no device runs — the memo states which experiments require hardware and defers them explicitly.
- **T17.5 (Q3) Compose Multiplatform on iOS evaluation — 1 d.** Decision memo from JetBrains CMP sources + local experiment where possible: does CMP/iOS support `graphicsLayer {}`-style deferred draw-phase reads identically to Android, or must it bind `MTKView` (D-13's `.metal` path)? Recommendation with citations; no commitments on CMP adoption.

## 3. Non-goals

Kernel diffs (absolute). Production implementations of any Q. Claims requiring hardware (deferred, listed). RFC acceptance is a staff decision after memo review — spikes don't self-accept.

## 4. Acceptance criteria (per RFC)

1. Memo exists at `rfcs/00XX-*.md` with: problem, proposal, evidence links, hardware-deferral list, staff-decision field left EMPTY.
2. Spike code confined to `spikes/` (own build; no imports from `core/`); tree lints clean.
3. Every number environment-tagged; every "would need hardware" claim explicitly listed.
4. Timebox: memo declares actual vs budgeted time (deviations field).

## 5. Evidence to return

`evidence/D-17/`: spike logs, bench outputs with tags, prototype test outputs, citation list per memo.

## 6. Report

`reports/D-17-REPORT.md` — one section per RFC: status (spike done / timebox overrun declared), headline finding, recommendation.
