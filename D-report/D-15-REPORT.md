# D-15 Report: Interactive Showcase Demos (W1–W5 × Modes A–D)

## 1. Executive Summary
Directive 15 delivers the multi-mode interactive showcase suite for Weft under `demos/web/`. It demonstrates high-throughput real-time data streaming across 5 distinct workloads (W1 Audio, W2 Particles, W3 Gyroscope/Spectrogram, W4 EEG, W5 L2 Order Book) and 4 operational modes (Mode A Naive State, Mode B RAF Batching, Mode C Weft SharedArrayBuffer Ring, Mode D Hand-Rolled Triple Buffer).

The implementation strictly honors the **Fairness Pin** design invariant (a single invariant canvas drawing routine for all modes) and cross-origin isolation (COOP/COEP) for zero-copy SAB operation. All 20 matrix permutations and 100-cycle hot-switch transitions were verified under CI (`ebda6af`).

---

## 2. Workload & Mode Architecture

### Workload Suite (`demos/web/src/workloads/generators.ts`)
1. **W1 (Audio Oscilloscope)**: 1,024 float32 samples/frame simulating multi-harmonic sine/saw waveforms.
2. **W2 (Particle System)**: 1,000 particles × 4 floats `(x, y, vx, vy)` simulating gravitation, damping, and collision wrapping.
3. **W3 (Gyroscope / Spectrogram)**: 64 frequency channels × 32 historical steps (2,048 floats) running sliding-window waterfall spectrogram updates.
4. **W4 (Multi-Channel EEG)**: 16 channels × 128 temporal samples (2,048 floats) with simulated alpha/beta/gamma rhythm synthesis.
5. **W5 (L2 Financial Order Book)**: 50 bids + 50 asks (price, depth, count, imbalance = 400 floats) with stochastic Poisson order matching.

### Mode Configurations (`demos/web/src/modes/runner.ts`)
- **Mode A (Naive State)**: State dispatch per producer event; re-renders canvas on every frame tick.
- **Mode B (RAF Batching)**: Accumulates latest frame in a single local buffer; throttles canvas draw to display refresh rate.
- **Mode C (Weft Ring)**: Double-ended lock-free `SharedArrayBuffer` ring buffer powered by `@weft/core` (`WeftWriter` / `WeftReader`). Zero allocation per frame in steady state.
- **Mode D (Hand-Rolled Triple Buffer)**: Traditional 3-slot rotating buffer with atomic swap index.

### Single Shared Draw Routine (Fairness Pin)
All canvas rendering is centralized in `demos/web/src/workloads/draw.ts::drawWorkload`. Modes A, B, C, and D execute identical canvas draw logic with no mode-specific optimizations or bypasses.

---

## 3. Test & Verification Matrix

### CI Validation on Node 22 (`ubuntu-latest`)
| Permutation Matrix | Mode A (Naive) | Mode B (RAF) | Mode C (Weft Ring) | Mode D (Triple Buf) |
| :--- | :---: | :---: | :---: | :---: |
| **W1 Audio** | PASS (1,000 frames) | PASS (1,000 frames) | PASS (1,000 frames) | PASS (1,000 frames) |
| **W2 Particles** | PASS (1,000 frames) | PASS (1,000 frames) | PASS (1,000 frames) | PASS (1,000 frames) |
| **W3 Gyro / Waterfall** | PASS (1,000 frames) | PASS (1,000 frames) | PASS (1,000 frames) | PASS (1,000 frames) |
| **W4 EEG (16-ch)** | PASS (1,000 frames) | PASS (1,000 frames) | PASS (1,000 frames) | PASS (1,000 frames) |
| **W5 L2 Order Book** | PASS (1,000 frames) | PASS (1,000 frames) | PASS (1,000 frames) | PASS (1,000 frames) |

### Hot-Switching Stress Test
- **100 Consecutive Random Mode/Workload Transitions**: 0 dropped pointers, 0 memory leaks, 100% clean teardown and allocation lifecycle.

---

## 4. Evidence Artifacts
- `evidence/D-15/demo_test_output.log`: Complete Vitest and Vite production build logs from CI (`ebda6af`).
- `evidence/D-15/evidence_summary.txt`: Summary of test execution, environment tags, and Fairness Pin audit.
- `demos/web/dist/`: Production Vite assets (`index.html`, `index-*.js`).

---

## 5. Compliance & Invariant Checklist
- [x] Kernel Freeze: `core/c/weft.{c,h}` and `core/rust/src/lib.rs` unmodified (0 diffs).
- [x] Fairness Pin: Single `drawWorkload` routine across all 4 modes.
- [x] Cross-Origin Isolation: COOP/COEP headers configured for SharedArrayBuffer support.
- [x] Matrix Execution: 20-cell matrix + 100 hot switches passed.
- [x] Production Asset Build: `tsc && vite build` exited 0.
