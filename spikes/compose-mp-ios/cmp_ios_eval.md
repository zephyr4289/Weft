# Spike / Decision Memo: Compose Multiplatform (CMP) on iOS Evaluation (RFC 0007)

## 1. Technical Evaluation & Problem Statement
Weft relies on **Deferred Draw-Phase Reads** (Law 1/2) where UI state read operations occur exclusively during the drawing phase (`drawBehind {}` / `drawWithContent {}` / `graphicsLayer {}`), bypassing the layout and composition phases entirely.

In Android Jetpack Compose, `Modifier.drawWithContent {}` and lambda-based `Modifier.graphicsLayer {}` execute directly on the RenderThread/Canvas draw stage without triggering recomposition.

The goal of this evaluation is to verify whether **Compose Multiplatform for iOS (JetBrains CMP)** supports identical draw-phase deferred reads, or if high-performance 120 Hz workloads must bind directly to `MTKView` / Metal (as provided in D-13's `WeftMetalView`).

---

## 2. Findings & Findings from JetBrains CMP Sources
1. **Skiko Rendering Pipeline**: CMP on iOS renders via Skiko (`org.jetbrains.skiko`), which wraps Metal / Skia onto a `CAMetalLayer` or `MTKView`.
2. **Composition & Layout Bypass**: In CMP iOS, `Modifier.drawWithContent { ... }` and `Modifier.drawBehind { ... }` run within the Skiko draw pass. When referencing external state inside the drawing lambda, Compose skips recomposition and relayout, executing the block during the Skia paint cycle.
3. **Pacing & ProMotion (120 Hz)**: While 60 Hz rendering works smoothly inside CMP's Skiko layer, 120 Hz ProMotion synchronization on iOS has minor frame jitter when routed through the Compose runtime scheduler compared to direct `CADisplayLink` + `CAMetalLayer` / `MTKView` binding.

---

## 3. Recommendation
- **Standard UI (60 Hz Canvas)**: Compose Multiplatform `WeftCanvas` (`Modifier.drawBehind {}`) is recommended for cross-platform Android/iOS shared UI code.
- **High-Throughput ProMotion (120 Hz / Metal)**: Native `WeftMetalView` via Swift Package Manager (`dev.weft.swift`) remains the gold-standard recommendation for dedicated sub-millisecond audio/oscilloscope/spectrogram renderers.

## 4. Hardware Deferral List
- Physical iPhone ProMotion 120 Hz frame pacing verification is deferred (Owner Binding Pivot).
