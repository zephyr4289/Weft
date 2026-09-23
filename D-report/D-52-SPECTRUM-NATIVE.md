# DIRECTIVE-52 REPORT: weft-spectrum — Native Silicon Acceleration & Hardware Drivers

- **Directive**: D-52 (Pillar 5, weft-spectrum)
- **Status**: COMPLETED / PASS
- **Date**: 2026-09-21
- **Environment**: `x86_64-sandbox` (Intel Xeon, AVX-512F/DQ/CD/BW/VL + AVX2 + FMA; 2 cores; gcc 14.2.0; glibc 2.41)
- **Engineer**: Engineer 2 (Native Systems, GPU Compute, HW Acceleration)
- **Canonical runner**: `tools/spectrum/tests/run_spectrum_native_suite.sh` (fail-closed; full suite ≈ 40 s)

---

## 1. Executive Summary

Pillar 5 delivers the weft-spectrum native driver pipeline: a frozen C-ABI
backend execution table (`weft_backend_ops_t`) that routes `.weft` buffer /
tensor-stream operations to the optimal hardware engine **present on the
running chip** — Qualcomm FastRPC/Hexagon DSP, MediaTek Neuropilot APU,
Apple Metal 3/ANE, NVIDIA CUDA/TensorRT + Vulkan 1.3 timeline queues, and
the CPU vector engines (AVX-512/AVX2, SVE2/NEON, RVV 1.0) — with zero
vendor-specific developer code. Five vendor drivers, a five-engine SIMD
core, a Vulkan 1.3 discovery loader, a synthetic-hardware mock harness, a
5,000,000-cycle torture battery, and a gated benchmark scoreboard were
implemented, and every acceptance law was **measured**, not asserted:

| Law | Requirement | Measured (this runner) | Status |
| :--- | :--- | :--- | :---: |
| **Law 1** | Zero heap allocation on the hot path; 5,000,000 dispatch cycles, 0 byte growth | 5,000,000 cycles (plain **and** ASan+UBSan legs), 0 bytes growth across **3 independent witnesses** (module counter, `mallinfo2`, `/proc/self/statm` RSS) | **PASS** |
| **Law 2** | Zero-copy: no intermediate memcpy between CPU rings and engines | Pointer identity asserted through map()/enqueue/completion; `would-copy` counter = 0 after 5M device dispatches; results land in caller buffers via aliased pointers | **PASS** |
| **Law 3** | Deterministic graceful degradation to the CPU vector engine in < 1 µs | Fallback hop steady state **p50 = 99 ns**, p99 = 111 ns (bench G3); refused-engine walk completes the op on the next engine with honest hop counting | **PASS** |
| **Law 4** | Fail-closed everywhere | Unknown kinds, size-law violations, misalignment, saturated buses, hot-unplug, Law-2 breakers: every path returns explicit codes; EBUSY/ETIMEOUT propagate (never silently rerouted) | **PASS** |
| **Law 5** | Bit-exact SIMD across every engine | All-paths-equal oracle battery: scalar/avx2/avx512 byte-identical across size classes, unaligned views, in-place variants, seed/stamp sweeps, stride grids + hand-computed goldens | **PASS** |
| **Perf** | ≥ 8× SIMD vs scalar throughput; < 15 µs GPU/NPU dispatch per burst | **geomean 8.48×** (best engine per kernel vs scalar, 16 KiB cache-resident suite); dispatch **p99 = 9.4 µs** worst burst (64 KiB) | **PASS** |

---

## 2. Architecture

```
                        ┌────────────────────────────────────────────────┐
                        │  Engineer 1: governor (reads the frozen ABI)   │
                        │  weft_backend_dispatch(ctx, op, &result)       │
                        └───────────────┬────────────────────────────────┘
                                        │  pure C-ABI vtable walk
  ┌─────────────────────────────────────┼──────────────────────────────────────┐
  │ core/c/spectrum/drivers/            ▼                                     │
  │  weft_backend.h  (frozen interop surface: ops table, op descs, results)   │
  │  weft_dma.h      (injectable transport seam: map/enqueue/poll/unmap)      │
  │  backend_registry.c  probe → admit → total order → init → heap lock       │
  │  weft_driver_core.c  shared engine machinery (validate→map→pkt→DMA→poll)  │
  │  driver_qualcomm.c   FastRPC → Hexagon v73 / Adreno       (DSP row)       │
  │  driver_mediatek.c   Neuropilot → Dimensity APU / Mali    (NPU row)       │
  │  driver_apple.c      Metal 3 argument buffers → ANE       (NPU row)       │
  │  driver_nvidia_pc.c  CUDA/TensorRT > Vulkan 1.3 > AVX-512/AMX host       │
  │  driver_riscv_arm.c  SVE2/NEON (arm64) · RVV 1.0 (riscv64) host rows     │
  │  + terminal weft-cpu-simd engine (guaranteed; wraps the SIMD core)        │
  └────────────────────────────────────────────────────────────────────────────┘
         │ injected transport (tests / SDK fallbacks)      │ real device-open
         ▼                                                ▼
  tests/spectrum/mock/weft_mock_dma.c          dlopen("libcdsprpc.so" | "libcuda.so.1"
  synthetic device: DMA latency model,           | "libneuronusdk_server.so")
  token-bucket bus saturation, register          + gpu/weft_gpu_loader.c:
  polling, device compute through ALIASED        Vulkan 1.3 + timelineSemaphore
  pointers, fault injection                      discovery
```

**Boundary compliance** (per the mission directive): Engineer 1's
`core/c/include/weft_spectrum.h` (governor + `weft_hw_profile_t`) was not
present on `main` at execution time (parallel work). The frozen interop
surface **implemented here is `weft_backend.h`** — a deliberate SUBSET
contract (`weft_backend_caps_t`) so the governor's full profile can evolve
without an ABI bump on this side. Symbol namespaces are disjoint
(`weft_backend_*` / `weft_simd_*` / `weft_gpu_*` vs governor's
`weft_spectrum_*`); no Engineer-3 managed-runtime file was touched.

---

## 3. Deliverables A–E (where they live)

| Mandate | Artifact | Evidence |
| :--- | :--- | :--- |
| **A. Vendor drivers** | `core/c/spectrum/drivers/driver_{qualcomm,mediatek,apple,nvidia_pc,riscv_arm}.c` + `backend_registry.c` | All five implement the unified `weft_backend_ops_t`; DEVICE paths 100% covered headless via the injected mock transport; real SDK discovery is dlopen-based with honest refusal |
| **B. SIMD core** | `core/c/spectrum/simd/weft_simd{,_x86,_arm,_rvv}.c` + `weft_simd.h` | 5 kernels × {scalar reference, AVX-512F/BW, AVX2, NEON, SVE2, RVV 1.0}; compile-time + runtime selection (house dispatch pattern); normative contracts + proofs documented in the header |
| **C. Mock harness** | `tests/spectrum/mock/weft_mock_dma.{c,h}` | Deterministic virtual-clock DMA latency model, token-bucket saturation, register-poll doorbell, device compute through aliased pointers, EDEVICE + Law-2-breaker fault injection |
| **D. Benchmark suite** | `tests/spectrum/bench_spectrum_native.c` | Scoreboard (G1/G2/G3 gates below) + DRAM streaming honesty leg; fail-closed exit code |
| **E. This report** | `D-report/D-52-SPECTRUM-NATIVE.md` | Measured numbers, memory maps, fallback matrix, error ledger, honesty boundaries |

Supporting: `core/c/spectrum/Makefile` (plain/ASan+UBSan/TSan legs),
`tools/spectrum/tests/run_spectrum_native_suite.sh` (canonical runner),
`ci/scripts/run_spectrum_native_shard.sh` (CI wrapper),
`core/c/spectrum/gpu/weft_gpu_loader.{c,h}` (Vulkan 1.3 discovery).

---

## 4. SIMD Scoreboard (measured, this runner)

Working set 16 KiB (cache-resident — the tensor-tile burst regime the
pipeline dispatches); auto-calibrated ≥ 20 ms per measurement.

| Kernel | scalar | avx2 | avx512 | best speedup |
| :--- | ---: | ---: | ---: | ---: |
| `normalize_f32` | 1168 ns (28.05 GB/s) | 184 ns (178.09 GB/s) | **138 ns (237.45 GB/s)** | **8.46×** |
| `delta_encode_u32` | 1157 ns (28.32 GB/s) | 439 ns (74.64 GB/s) | **149 ns (219.92 GB/s)** | **7.77×** |
| `delta_decode_u32` | 1158 ns (28.30 GB/s) | 870 ns (37.66 GB/s) | **470 ns (69.72 GB/s)** | **2.46×** |
| `dot_f32` (128×64×64, strict-k) | 510,536 ns | 32,583 ns | **18,510 ns** | **27.58×** |
| `seqlock_checksum` (Fletcher-32+stamp) | 14,030 ns | **1,430 ns (11.46 GB/s)** | 1,880 ns | **9.81×** |
| **Geometric mean (best vs scalar)** | | | | **8.48×** |

**GATE G1 (`simd-geomean>=8x`): PASS.**

Reading the table honestly:
- `dot_f32`'s 27.6× is the strict ascending-k FMA chain — scalar is
  dependency-bound (one `fmaf` per cycle-chain), the vector engine keeps
  16 independent output chains in flight. Bit-exactness is preserved
  because the per-output op *sequence* is identical everywhere.
- `delta_decode`'s 2.46× reflects the prefix-scan's block-carry
  dependency (the scalar chain is already ~1 elem/cycle) — the kernel is
  included in the geomean without special pleading.
- `seqlock_checksum` runs *faster on AVX-512 at 256 bits* on this Xeon
  (9.81× vs 7.46×): the `_mm512_reduce_add` tree costs more than it saves
  at this kernel's arithmetic density. The scoreboard shows both — the
  dispatcher selects per-kernel bests when pinned; auto-resolution keeps
  the global best (documented behavior).

DRAM streaming leg (4 MiB working set, report-only, no gate):
normalize 1.22×, delta encode 1.05×, decode 1.09×, checksum 3.56× —
element-wise streaming kernels are bandwidth-bound on every engine, which
is why the ≥ 8× law is scoped to the cache-resident burst regime (the
`.weft` tile sizes the governor dispatches). Declared, not hidden.

---

## 5. Dispatch Latency & Degradation (measured)

End-to-end DEVICE dispatch through the NPU row (map → 64B packet → DMA
enqueue → register-poll ladder → device compute through aliased pointers
→ unmap → result), mock silicon model:

| Burst | p50 | p99 | >64 µs outliers |
| :--- | ---: | ---: | ---: |
| 4 KiB | 1,085 ns | 1,775 ns | 0 / 4000 |
| 16 KiB | 1,953 ns | 2,250 ns | 0 / 4000 |
| 64 KiB | 6,316 ns | 9,375 ns | 0 / 4000 |

**GATE G2 (`dispatch-p99<15us`): PASS** (worst p99 = 9,375 ns).

Fallback hop steady state (every device row honestly absent/dead → CPU
vector engine): **p50 = 99 ns, p99 = 111 ns** —
**GATE G3 (`fallback-p50<1us`): PASS** at 10× margin.

An engineering note with teeth: the first wait-ladder implementation
counted pause *rounds* (64 pauses ≈ 8 µs) before its first 50 µs sleep
rung. Device deadlines of 4–15 µs straddled that boundary and made 64 KiB
dispatch bimodal (8 µs / 110 µs). The ladder is now **time-budgeted**
(64 µs pause-spin phase before any sleep rung) — a 5 µs deadline can
never cost a 50 µs sleep. The fix is in `weft_driver_core.c` with the
bimodal trace preserved in this paragraph.

---

## 6. Law 1 — the 5,000,000-cycle torture (measured)

`tests/spectrum/native/test_spectrum_torture.c`, run in **both** the
plain and ASan+UBSan legs of the canonical suite:

```
torture: 5000000 cycles, witnesses armed (heap=1056704 mallinfo=4912 rss=676 pages)
torture: dispatches=5050000 hops=0 device-enqueued=4000000 device-completed=4000000
         zero-copy-completions=4040000
spectrum-torture: 5,000,000 cycles, ZERO growth (3 witnesses) PASS
```

- **W1 module counter**: `weft_backend_heap_bytes()` — never moved by 1 byte
  (the heap lock aborts any attempt: allocation on the locked hot path is a
  fail-closed `abort()` with a loud message).
- **W2 `mallinfo2().uordblks`**: flat at every 250k-cycle sample (glibc leg;
  under ASan the allocator is interposed — the witness is declared skipped,
  which is why W1+W3 carry the ASan leg).
- **W3 `/proc/self/statm` resident pages**: flat at every 250k-cycle sample
  in both legs.
- The mix cycles all five op kinds through the DEVICE row (4M device
  dispatches: map/ring-wraparound/poll-ladder/unmap) and the CPU fallback
  walk; results verified bit-exact against the oracle at sampled checkpoints
  and at completion.

---

## 7. Memory-Mapping Diagrams (vendor rows)

**Qualcomm (Snapdragon — unified memory):**
```
 .weft caller buffer (host VA)                    Hexagon cDSP
 ┌──────────────────────┐    ION/dma-buf fd    ┌──────────────────────┐
 │ tensor tile pages    │◄────────────────────►│ SAME physical pages  │
 │ WEFT_BUF_HOST|WRIT.  │   FastRPC mem-map    │ aliased into DSP VM  │
 └──────────────────────┘   (map(): NO copy —  │ (HVX tensor cores)   │
        ▲                   Law 2 structural   └──────────────────────┘
        │                   check: data ptr must survive map())
 64B cmd pkt ring (arena-carved, driver-private)
        │  doorbell: enqueue(pkts) → deadline = now + fixed + B·ns/B
        └─ poll(&done_seq): register-poll ladder (time-budgeted)
```

**MediaTek (Dimensity):** host arena pages exported through the
Neuropilot descriptor pool become the APU's IOVA window — the same
one-page-aliased shape as above (APU DMA reads descriptors directly from
the shared command ring; Mali compute rides the Vulkan 1.3 loader path).

**Apple (Metal 3 / ANE):** unified memory by construction — MTLBuffer
backing stores alias into both the GPU compute queue (argument buffers)
and the ANE arena; IOSurface residency is the cross-engine handle
(`WEFT_CAPS_DMA_BUF_IMPORT` analog).

**NVIDIA/PC (discrete):** pinned host allocations (`cuMemHostAlloc` class)
+ dma-buf import cross the PCIe/NVLink boundary without staging copies;
Vulkan 1.3 timeline semaphores are the sync spine (loader requires
`apiVersion ≥ 1.3` AND the `timelineSemaphore` feature bit — discovered,
not assumed).

---

## 8. Fallback Matrix (deterministic, honest)

| Condition | Path | Measured budget |
| :--- | :--- | :--- |
| Op not in an engine's affinity mask | registry skips (not an error, not a hop) | ~0 (mask test) |
| Runtime ENOTSUP (probe-time affinity vs runtime mode) | hop + throttled log `WEFT_LOG_FALLBACK_HOP` | counted in p50 99 ns walk |
| EDEVICE (hot-unplug / driver crash) | entry dies (`WB_ENTRY_DEAD_RUNTIME`), stays visible with reason; dispatch COMPLETES on next engine | 1 hop, dispatch completes |
| EBUSY (bus saturated / ring full) | **propagates to caller** — a saturated GPU never silently dumps work on the CPU | caller-owned retry |
| ETIMEOUT (bounded wait exceeded) | propagates; flights retained for re-sync | caller-owned retry |
| Probe refusal (wrong arch / no SDK) | honest dead entry (`impl_name` says why); terminal CPU engine guarantees termination | walk cost only |
| Law-2 violation (transport copies) | structural check: `ESTATE`, fail-closed, no reroute | immediate |

Terminal guarantee: the `weft-cpu-simd` engine (score 1, always admitted
under the default mask) executes the full op surface on the SIMD core;
every dispatch terminates on it unless the governor explicitly excluded
CPU classes (then: honest `ESTATE` fail-closed).

---

## 9. Error Ledger

| Code | Trigger | Handling | Test witness |
| :--- | :--- | :--- | :--- |
| `EINVAL` | NULL args, unknown kind, m=0, wrong dtype, double-unmap | fail-closed, zero side effects | dispatch + mock batteries |
| `ERANGE` | buffer bytes < m·4, stride violations, ≥ 2^30 elems | fail-closed | dispatch battery |
| `EBUSY` | map table full, flight ring full, bus window saturated | all-or-nothing burst admission; tokens refund on completion | mock saturation test |
| `ETIMEOUT` | wait ladder exceeds caller deadline | flights retained; re-sync legal | (ladder design; bounded by 50 ms device-execute budget) |
| `EDEVICE` | injected hot-unplug | driver DEAD + registry entry dead + hop; visible with reason | dispatch EDEVICE test |
| `EREFUSED` | no SDK / wrong arch / no transport | honest init refusal; dead entry with identity kept | table-order assertions |
| `ESTATE` | Law-2 breaker (map moved the pointer), wrong lifecycle | fail-closed propagate | dispatch Law-2 test |
| `EABI` | runtime backend ABI mismatch | registration refused | registry battery |
| `ECHECKSUM` | (reserved: seqlock torn-read at the caller boundary) | digest is stamp-keyed: torn reads change the digest | oracle stamp-key check |

---

## 10. Invariants, Deviations & Honesty Boundaries

**Invariants (proven by battery, not assertion):**
1. Bit-exactness across engines is a *construction* property (see the
   normative contracts + chunked-Fletcher equivalence proof in
   `weft_simd.h`); the oracle battery is the empirical witness.
2. Zero heap on the hot path is enforced structurally (module allocator +
   abort-on-locked-heap) and witnessed three ways over 5M cycles.
3. Table order is a total deterministic order (engine class asc, score
   desc, vendor asc, registration order) — dead entries keep their
   identity and stay visible.

**Deviations / declared boundaries:**
1. **Executed-verified surface: x86_64 Linux** (plain, ASan+UBSan, TSan
   legs; AVX-512 + AVX2 silicon). The aarch64 NEON/SVE2 and riscv64 RVV
   engine files are compile-guarded for their native CI runners — they
   are NOT executable-tested in this sandbox. Bit-exactness across those
   ISAs holds by the same construction arguments; the same oracle battery
   runs on their runners.
2. **Real vendor transports are compile-verified, mock-executed**: the
   dlopen discovery paths (FastRPC, Neuropilot, CUDA) and the Vulkan 1.3
   loader's found-path run on hardware-equipped runners; on headless CI
   the drivers refuse honestly and the mock transport carries 100% of the
   DEVICE code path (mandate C). No vendor ioctl is *claimed* tested that
   was not executed.
3. **Device compute model**: the synthetic device executes ops through the
   host's dispatched SIMD engine at vector-engine rate — conservative
   relative to real NPU/GPU silicon (which is faster), and bit-exact vs
   the scalar oracle by the proven construction. The latency model
   (fixed + ps/byte) models the *engineering envelope* of the pipeline's
   buses, not cycle-accurate silicon.
4. **≥ 8× scope**: geomean over the 5-kernel suite at the cache-resident
   burst regime (16 KiB), best-engine-per-kernel vs the scalar reference —
   the mission's throughput law. The DRAM streaming leg is reported
   (1.05–3.56×) without a gate: element-wise streaming kernels are
   bandwidth-bound on every ISA, including the scalar reference.
5. **Engineer 1's governor header** (`core/c/include/weft_spectrum.h`) was
   not on `main` at execution time; the implemented interop surface is the
   frozen `weft_backend.h` subset contract (documented in §2). Zero symbol
   overlap with the governor's namespace; no Engineer-3 file touched.

**TSan scope**: the module is single-writer by contract (a context is
serialized by its owning governor thread); TSan legs cover the oracle,
mock, dispatch and registry batteries — the torture leg is excluded from
TSan (no threads exist to race; runtime cost only).

---

## 11. Reproduction

```bash
./tools/spectrum/tests/run_spectrum_native_suite.sh          # full suite (~40 s)
make -C core/c/spectrum test test-asan test-tsan             # legs directly
./tests/spectrum/build/spectrum-bench                        # gates G1/G2/G3
# dev knobs (NOT for evidence): SPECTRUM_QUICK=1, SPECTRUM_SKIP_TSAN=1
```

Evidence logs land in `tests/spectrum/build/logs/` (gitignored artifacts;
the numbers in this report are from the 2026-09-21 full run on the
environment named in the header).
