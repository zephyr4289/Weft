# D-28 REPORT: Pillar 2 — weft-tensor Zero-Copy Tensor Fabric

- **Directive**: D-28 / Mission Pillar 2 (Realtime Edge AI & Zero-Copy Tensor Fabric)
- **Status**: COMPLETED / PASS (3 DCO-signed commits on `feat/weft-tensor-core`, off `origin/main` @ db24254)
- **Date**: 2026-09-20
- **Environment**: `x86_64-sandbox` (Xeon, 2 CPU, gcc 14.2.0, Debian glibc; ASAN + TSAN legs local)
- **Reference**: RFC-0021 (`rfcs/0021-weft-tensor-fabric.md`, filed Draft), mission laws 1-4

---

## 1. Executive Summary

Pillar 2's foundation layer is delivered as `core/c/tensor/` — a
zero-dependency C11 driver-layer module that removes the sensor→NPU→UI
copy storm at the geometry level: tensors decode with ONE pointer add
from ring slots, arenas, SHM mappings, dma-buf regions, or GPU-mappable
host memory. Every claim is gated: the WT-series battery (932 checks
across three binaries) runs plain + ASAN + TSAN in the new
`tensor-fabric` CI shard, plus a Python-oracle golden-fixture gate that
proves the address math cross-implementation, a fork() torture that
proves the claim/commit protocol across real process boundaries, and
3-rep measured benches. Kernel bytes untouched (Law 3: diff-verified, 0
frozen files).

## 2. Acceptance Criteria Scoreboard (mission deliverables)

| Deliverable | Acceptance | Measured / Proven | Status |
| :--- | :--- | :--- | :---: |
| 1. Lock-free DMA Circular Tensor Ring | MPSC + SPMC claim/commit, page-locked, 64B/128B alignment, bounded sequence cursors | Vyukov sequence-word protocol (check-then-CAS unique tickets, advance-before-read exactly-once tail); MAP_SHARED mappings, mlock reported honestly; slot payloads 64B-aligned by construction (128B for 128-multiple payloads); WT25-40: 512 checks. **MPSC 50,000 msgs / 25.6M per-ticket word checks — ZERO tears, ZERO FIFO violations; SPMC 4 racing consumers — exactly-once proven; MPMC 2×2 — zero tears; fork() torture: 10,000 cross-process tensors, zero tears; deadline honesty: 60ms budget honored at 60.0-60.1ms** | **PASS** |
| 2. Unified Strided Tensor Geometry | directive-verbatim `weft_tensor_view_t`, `Address(i)=Base+byte_offset+Σiₖ·strideₖ`, zero-alloc slice/sub-window/reshape | 176-byte frozen ABI (static + runtime pins, WT2); WT1-16: 329 checks incl. **23 golden offsets bit-exact vs an independent Python oracle** (1D audio, 2D spectrograms incl. padded/flipped, 4D NCHW + NHWC, 5D KV-cache blocks, cropped sensor windows); OOB-stride wall, 2^64 overflow walls, Law-4 EMISALIGN classes 16/32/64/128; slice/subwindow/permute/reshape (ERESHAPE on non-contiguous — never a silent copy) | **PASS** |
| 3. Memory Arena & Paged Allocation | page-aligned, unified-memory-ready, sub-µs sub-allocation, zero malloc/free | mmap page-aligned + mlock/THP-hint/prefault (refusals REPORTED); attach seam for dma-buf/Vulkan/UMA regions (16B-minimum wall, EMISALIGN); WT17-24: 91 checks; **measured 2.3-2.8 ns/alloc over 1M-alloc legs (3 reps, ASAN leg 4.1ns)** — ~400× under the 1µs budget; exhaustion ENOMEM reported, never rounded | **PASS** |
| 4. Static Analysis & Formal Verification | OOB stride detection; zero tearing across concurrent claim/commit; bit-exact offsets across 1D/2D/4D NCHW+NHWC | OOB strides: ERANGE wall (WT6, WT29 through commit); tearing: per-ticket splitmix word patterns verified on every message — MPSC/SPMC/MMPC/fork legs all zero; bit-exactness: the Python-oracle golden fixture (WT4) + independent in-test arithmetic (WT38 flagship: slice→subwindow→permute chain over a live ring tensor, every address exact) | **PASS** |

**Laws**: L1 zero-heap data path — ASAN legs green, no allocation between
create/attach and destroy; L2 bounded latency — no unbounded-wait API
exists, fixed-rung wait ladder (64 pauses → yield → 50µs sleep),
deadlines measured honored; L3 kernel byte-frozen — 0 frozen files in
the branch diff (core/c/Makefile gains pass-through targets only); L4
honest alignment — closed class registry, EMISALIGN explicit at
validate AND commit, payload alignment by construction.

## 3. Measured evidence (2-CPU sandbox, 3 reps, house JSONL)

| Metric | Value |
| :--- | :--- |
| Arena sub-allocation | 2.3 / 2.5 / 2.8 ns per alloc (1M allocs per rep) |
| Ring throughput (16 KiB payloads, 4 producers, every word verified) | 440,942 / 481,802 / 503,185 msgs/s |
| Verified payload movement | 7.22 / 7.89 / 8.24 GB/s |
| Integrity coverage per bench rep | 50,000 msgs × 2,048 words = 102.4M word checks |
| Ring smoke (4 KiB, plain) | 713,130 msgs/s, 2.92 GB/s |
| Wait-ladder deadline | 60.0-60.1 ms elapsed for a 60 ms budget (WT36) |

Throughput figures are honesty floors, not peaks: the verification pass
IS the workload. No tight perf assert ships (house no-flaky rule); the
generous bound (10 µs/alloc, completion under timeout) is the only hard
gate.

## 4. Bugs caught by my own gates before commit

- The Python oracle caught MY hand-arithmetic in its own fixture spec
  (flipped-plane byte_offset 1,224,960 vs the correct 1,226,240 =
  479×2560) — the oracle exists precisely to catch this class.
- Contiguous-stride INT64_MAX wall added after finding that
  shape {4, 2^62} could cast a >2^63 stride negative (EOVERFLOW now).
- Test-side bugs my gates pinned (library math was correct each time,
  proven by the golden fixture): WT7 alignment-class expectations
  (0x10000+64 is 64B- but not 128B-aligned), WT10 NHWC permute stride
  table, WT8 batch-stride expectation, WT15 wrap case that didn't
  actually wrap, WT21 mark arithmetic, WT23 arena capacity sizing,
  WT27 slot-recycle sequence, verifier stride table for [1,4,16,64].
- A use-after-free in MY WT33/39 harness (ticket arrays freed before
  the exactly-once merge) — the class of bug the ASAN leg exists to
  catch; fixed by reordering.
- WT39 barrier deadlock (count included consumers that never wait on
  it) — found by the 300s timeout gate.
- `stdbuf` (LD_PRELOAD) breaks ASAN binaries ("runtime not first in
  library list") — shards run binaries directly.

## 5. Evidence index

Repo (`litmus/evidence/tensor/`): `wt-suite-plain.log` (329+91+512
checks, 0 failures), `wt-suite-asan.log` (same, zero findings),
`wt-suite-tsan.log` (503 checks — fork torture declared-skip),
`bench-throughput.jsonl` (3 reps × {arena 1M, ring 50K×16KiB}),
`shard-local-transcript.txt` (full shard: ALL PASS 7/7), `README.md`.
Clean-worktree verification: detached worktree @ 3abb1a0, full shard
ALL PASS.

## 6. Boundary of the claim (honesty)

Zero-copy decode means claim→commit→acquire→release moves zero payload
bytes; vendor drivers that insist on their own heaps are E2's attach
seams. Zero tearing is by-construction under the claim/commit bracket —
a DMA engine writing an UNCLAIMED slot is out of contract and
undetectable here. MPMC/MPSC consume in strict ticket order: a slow
producer stalls later tickets (deliberate Vyukov semantics — size N ≥
producers × depth). Release-without-acquire is slot-level
indistinguishable; it stalls the tail and surfaces as consumer
ETIMEOUT. The arena bump is single-thread by design. Throughput numbers
are 2-CPU sandbox class, honestly labeled. TSAN cannot run fork torture
(declared skip; plain + ASAN carry it). Push to origin was refused (no
write credentials in this sandbox) — patches + zip in this package are
the apply path.

## 7. What Engineer 2 / Engineer 3 inherit

- The frozen `weft_tensor.h` contract (view ABI table in RFC-0021 §2) —
  compile against it; any drift is a WTR1 version bump, not an edit.
- Ring attach over any mapping they own (dma-buf mmap, memfd, Vulkan
  HOST_VISIBLE): `weft_tensor_ring_attach(mapping, bytes, mode)`.
- Arena attach for NPU/UMA staging regions: `weft_tensor_arena_attach`.
- `weft_tensor_view_element_addr_at(view, idx, payload)` as THE
  cross-process decode primitive (never `physical_or_shm_addr`).
- Open seams awaiting owners (RFC-0021 Open Questions): dma-buf fd
  plumbing, futex wakeups, span rings for mixed geometries, WGSL-side
  uniform decode.
