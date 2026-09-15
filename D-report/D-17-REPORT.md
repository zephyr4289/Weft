# D-17 Report: Protocol RFC Spikes Q1–Q5

## 1. Executive Summary
Directive 17 delivers technical evaluations, prototypes, benchmarks, and formal RFC memos for the five open architectural questions (Q1–Q5) identified in `ARCHITECTURE.md`. In strict adherence to the project invariants, the core kernel remains 100% **FROZEN** (0 diffs to `core/c/` and `core/rust/`).

All RFC memos have been drafted per `rfcs/TEMPLATE.md` with problem statements, reference specifications, hardware-deferral lists, and empty staff-decision fields.

---

## 2. RFC Findings & Benchmark Summary

| RFC Number & Title | Focus Area | Headline Finding & Tag | Recommendation |
| :--- | :--- | :--- | :--- |
| **RFC 0003: Triad-2 GPU-Resident** | WebGPU / Metal / Vulkan VRAM exchange | **0.47 µs handoff (180x speedup)** over CPU staging memory copy (`linux-sandbox+dawn`). | **Accept for Wave 3** (GPU-heavy workloads). |
| **RFC 0004: Multi-Consumer Fan-Out** | 1-Writer, N-Reader seqlock ring | **1.27M publishes/sec** across 4 concurrent readers with 0 allocations (`node/linux-sandbox`). | **Accept as Driver Pattern**. |
| **RFC 0005: VerifiedWeft Authenticated** | HMAC-SHA256 frame integrity | **2.10 µs encode / 2.09 µs decode** at 64B payload; missed < 1.0 µs target in pure software (`x86_64-sandbox`). | **Defer / Opt-In Only**. |
| **RFC 0006: Android ReattachPolicy** | Process death & state recovery | Rebirth state machine cleanly separates clean re-allocation from shared memory re-hydration. | **Accept for Android**. |
| **RFC 0007: Compose MP on iOS** | Skiko draw-phase deferred reads | CMP iOS successfully bypasses recomposition during draw phase; 120 Hz ProMotion favors native Metal. | **Accept (Dual-tier policy)**. |

---

## 3. Evidence Artifacts
- `evidence/D-17/gpu_pingpong_bench.log`: GPU vs CPU memory handoff benchmarks.
- `evidence/D-17/fanout_prototype.log`: Multi-reader seqlock fan-out throughput and 0-allocation logs.
- `evidence/D-17/verified_weft_bench.log`: HMAC-SHA256 encode/decode latency measurements.
- `rfcs/`:
  - `0003-triad2-gpu-resident.md`
  - `0004-fanout-heddles.md`
  - `0005-verifiedweft.md`
  - `0006-reattach-policy.md`
  - `0007-compose-mp-ios-eval.md`

---

## 4. Compliance & Invariant Checklist
- [x] Kernel Freeze: `core/c/weft.{c,h}` and `core/rust/src/lib.rs` unmodified (0 diffs).
- [x] All 5 RFCs written using standard template with `[EMPTY]` staff decisions.
- [x] Spike code isolated in `spikes/` with clean lint/compilation.
- [x] All benchmark numbers environment-tagged.
- [x] Hardware deferrals explicitly declared.
- [x] Structural validator: `python3 tools/port_validator.py --target all` PASS.
