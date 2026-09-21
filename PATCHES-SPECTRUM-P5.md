# PATCHES — Project weft, Pillar 5: Native Silicon Acceleration & Hardware Drivers

**Branch:** `feat/weft-spectrum-native` (base: main @ `7391fee5`)
**Directive:** Mission Briefing — Pillar 5 (weft-spectrum), Swarm Layer 3, Engineer 2
**Result:** 12 independent patches delivering the frozen backend C-ABI +
DMA transport seam, the five-engine SIMD core, the shared engine
machinery, five vendor drivers, the Vulkan 1.3 discovery loader, the
synthetic-hardware mock harness, five native test batteries, the gated
benchmark scoreboard, the fail-closed canonical runner, and the D-52
audit report.
**Zero guardrail regressions:** Engineer 1's governor namespace
(`weft_spectrum_*`) is untouched (the frozen interop surface implemented
is `weft_backend.h` — a deliberate subset contract), no Engineer-3
managed-runtime file was modified, and every law is *measured*, not
asserted.

---

## The one-paragraph version

weft-spectrum routes `.weft` buffer/tensor-stream ops to the optimal
hardware engine present on the running chip — Qualcomm FastRPC/Hexagon,
MediaTek Neuropilot APU, Apple Metal 3/ANE, NVIDIA CUDA/TensorRT +
Vulkan 1.3 timeline queues, and the CPU vector engines (AVX-512/AVX2,
SVE2/NEON, RVV 1.0) — through one pure C-ABI ops table, with zero
vendor-specific developer code. The five kernels are bit-exact across
every engine **by construction** (element-wise IEEE sequences, modular
integer algebra, strict ascending-k FMA chains, a proven
chunk-invariance theorem for the Fletcher recurrence) and the
all-paths-equal oracle battery is the empirical witness. Every vendor
DEVICE path is 100% exercisable headless through the injectable
transport seam: a synthetic device models DMA latency, bus saturation
and register polling, computes through the *aliased* caller pointers
(zero-copy, witnessed), and injects the faults the error ledger
promises to catch. The canonical runner is fail-closed end to end
(`-Wall -Wextra -Werror -pedantic` on every compile; plain, ASan+UBSan
and TSan legs; ~40 s total).

## The laws, enforced (not intended) — measured on the CI runner

| Law | Requirement | Measured | Patch |
| :--- | :--- | :--- | :--- |
| 1 | Zero heap on the hot path; 5,000,000 cycles, 0 byte growth | 5M cycles (plain **and** ASan+UBSan), flat across 3 witnesses (module counter / `mallinfo2` / statm RSS) | 03943d84, 9dad8094 |
| 2 | Zero-copy between CPU rings and engines | pointer identity asserted end-to-end; would-copy counter 0 after 5M device dispatches; results land in caller buffers | 2084e70c, f1eadb76 |
| 3 | Graceful degradation to the CPU vector engine < 1 µs | fallback hop **p50 = 99 ns** (p99 = 111 ns) | 03943d84, cff99034 |
| 4 | Fail-closed everywhere | explicit codes for every refusal class; EBUSY/ETIMEOUT propagate, never silently rerouted | 03943d84 |
| 5 | Bit-exact SIMD across engines | all-paths-equal oracle battery + hand-computed goldens, PASS | 61c68107, 49d549a9, 03943d84 |
| — | ≥ 8× SIMD vs scalar throughput | **geomean 8.48×** (16 KiB cache-resident suite; dot 27.6×, checksum 9.8×, normalize 8.5×) | cff99034 |
| — | < 15 µs GPU/NPU dispatch per burst | **worst p99 = 9.4 µs** (64 KiB bursts, zero >64 µs outliers) | cff99034 |

## The series

| # | Commit | Subject | Payload |
| :-: | :--- | :--- | :--- |
| 1 | `36ad00f5` | frozen backend C-ABI + DMA seam | `weft_backend.h`, `weft_dma.h` |
| 2 | `61c68107` | SIMD core: oracle + dispatch | `weft_simd.h/.c` |
| 3 | `49d549a9` | x86 AVX-512F/BW + AVX2 engines | `weft_simd_x86.c` |
| 4 | `9d71542b` | ARM NEON/SVE2 + RVV 1.0 engines | `weft_simd_arm.c`, `weft_simd_rvv.c` |
| 5 | `f1eadb76` | shared engine machinery + wait ladder | `weft_driver_core.h/.c` |
| 6 | `9dad8094` | registry: probe/admit/order/init + heap lock | `backend_registry.c` |
| 7 | `ef9ea7f7` | five vendor drivers | `driver_*.c` ×5 |
| 8 | `c1bcf638` | Vulkan 1.3 timeline-semaphore loader | `gpu/weft_gpu_loader.*` |
| 9 | `2084e70c` | synthetic DMA mock harness (mandate C) | `tests/spectrum/mock/` |
| 10 | `03943d84` | native batteries incl. 5M torture | `tests/spectrum/native/` |
| 11 | `cff99034` | gated bench + Makefile + canonical runner | `bench_spectrum_native.c`, `Makefile`, `tools/spectrum/`, `ci/` |
| 12 | `bb8285df` | D-52 audit report | `D-report/D-52-SPECTRUM-NATIVE.md` |

## Reproduction

```bash
git checkout feat/weft-spectrum-native
./tools/spectrum/tests/run_spectrum_native_suite.sh     # full fail-closed suite (~40 s)
```

Full analysis: `D-report/D-52-SPECTRUM-NATIVE.md`.
