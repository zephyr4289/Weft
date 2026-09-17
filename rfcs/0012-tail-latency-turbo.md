---
RFC: 0012
Title: Tail-Latency Eradication Layer (TLEL) — placement, prefetch, pinning, kernel-bypass ingestion
Status: Draft
Authors: weft-contributor
Created: 2026-09-18
Supersedes / Superseded-by: None
---

# RFC 0012 — Tail-Latency Eradication Layer (TLEL)

## Summary

An additive C driver-layer module (`core/c/turbo.{h,c}` +
`core/c/uring_rx.{h,c}`) that pushes the fan-out ring's P99/P999 toward its
physical floor **without touching a frozen byte**: 2 MiB-aligned THP-mapped
rings attached through the frozen `weft_fanout_attach_writer()` seam,
reader/writer cache prefetch hints over the documented advisory ctrl reads,
SSE2 non-temporal streaming fill with an explicit ordering fence, capability
detected CPU pinning / NUMA first-touch / `mlock` / `SCHED_FIFO` services
that degrade and *report* every refusal, and a kernel-bypass datagram
ingestion path where the kernel itself is the bracket filler — probed live
as io_uring RING mode on the dev sandbox's 5.10 kernel.

## Motivation

The lead directive for this phase names the target: sub-microsecond P99 and
tail-latency eradication at the architecture's physical limits, under the
Kernel Freeze contract. Three measured facts motivate the design:

1. **The frozen hot path is already fast and wait-free** — B-suite C-leg
   baseline: publish p50 = 45 ns, p99 = 53 ns. What remains is not
   algorithmic cost but *placement* and *environment* jitter: page faults on
   first touch, TLB pressure on multi-MiB rings, scheduler migration, and —
   measured during this work and preserved in
   `litmus/evidence/turbo/aliasing-investigation.log` — an **address-layout
   lottery** where the identical instruction stream over a posix-heap ring
   measured 60 ns in one binary layout and ~450–590 ns in another
   (store→load 4K-aliasing class on the sandbox Xeon). A ring whose p50
   depends on where the allocator happened to place it is a tail-latency
   defect no code change can fix — only placement determinism fixes it.
2. **Ingestion pays a scratch copy and a syscall it does not need.** The
   stock path (`recv` → scratch → memcpy → slot) copies every datagram
   twice. The FI1 bracket already defines exactly what a fill may look
   like; letting the kernel copy directly into the slot under the bracket
   removes the second copy with zero protocol change.
3. **The capability ladder is the honest shape of the problem.** THP,
   `mlock`, `SCHED_FIFO`, io_uring registration/fixed-recv/multishot, and
   NUMA topology are all *host properties*, not code properties. A layer
   that assumed any of them would be wrong on the first container it met;
   a layer that probes, degrades, and reports is deployable everywhere and
   fast where the host allows (Law 4 / AXIOM T posture).

## Guide-level explanation

A user writes the frozen API they already know, wrapped in three optional
calls:

```c
// 1. Deterministic placement (instead of weft_fanout_init):
weft_turbo_ring_opts_t o;   // THP advise + prefault + mlock-best-effort
weft_turbo_ring_opts_default(&o);
o.payload_bytes = PB; o.slot_count = 8;
weft_fanout_t f; weft_turbo_ring_t tr;
weft_turbo_fanout_create(&f, &tr, &o);      // attaches the frozen ring

// 2. Prefetch-wrapped hot path (semantics byte-identical):
uint8_t* c = weft_turbo_begin(&f);          // warms the slot being filled
weft_turbo_fill(&f, src, len);              // NT stream + sfence (optional)
weft_turbo_publish(&f);                     // warms the NEXT slot
weft_turbo_prefetch_next(&r);               // EARLY: before render work
const weft_fanout_claim_t* k = weft_turbo_claim(&r);

// 3. Thread services (every refusal is a return code):
weft_turbo_pin_cpu(1);                      // writer CPU 0, reader CPU 1
int rt = weft_turbo_rt(10);                 // -EPERM in containers: reported
```

For ingestion, one session call replaces the recv loop:

```c
weft_uring_rx_t rx;
weft_uring_attach(&rx, &f, datagram_fd);    // negotiates the ladder rung
while (running) { weft_uring_next(&rx); }   // begin -> kernel fills slot
                                            // -> publish, under the bracket
```

Nothing user-visible changes: claim records, stats, ring bytes, and the
T-series byte-equivalence gates prove the wrappers are semantics-neutral.

## Reference-level specification

### Capability ladder (turbo)

`weft_turbo_caps()` probes once (pthread_once) and caches: page/hugepage
sizes, NUMA node count + per-node cpulists, THP mode
(`/sys/kernel/mm/transparent_hugepage/enabled`), affinity availability, an
`mlock` page probe, and a `SCHED_FIFO` attempt that always restores the old
policy. The report (`weft_turbo_caps_report`) is designed for evidence logs —
the refusal lines are the point.

| service | wants | this sandbox (measured) |
|---|---|---|
| THP ring | THP = madvise or always | `[always]`, kernel 5.10 |
| prefault | MAP_POPULATE-equivalent explicit touch | available (T3: 0 min-fault delta) |
| core pinning | `sched_setaffinity` | available (2 CPUs) |
| NUMA first-touch | > 1 node | 1 node — ladder verified, cross-node deferred |
| mlock | RLIMIT_MEMLOCK ≥ ring | 64 KiB cap — graduated: small rings lock, MiB rings refuse with errno |
| SCHED_FIFO | CAP_SYS_NICE | EPERM — refused, reported (T7) |

### Tail-latency ring allocator

`weft_turbo_fanout_create`:
1. optional NUMA bind scope **before** the mapping exists;
2. `mmap(PRIVATE|ANON)` of `ring_bytes + align`, trimmed at both edges to
   the hugepage boundary (the classic guaranteed-aligned mapping);
3. `madvise(MADV_HUGEPAGE)` before any touch (best-effort, errno recorded);
4. the ctrl-zeroing `memset` doubles as first-touch + prefault (this is
   also the attach contract: ctrl reads as a fresh `weft_fanout_init` ring);
5. optional `mlock` after touching (graduated refusal, recorded);
6. `weft_fanout_attach_writer()` — the frozen path validates geometry and
   adopts the mapping. `weft_turbo_fanout_destroy` munmaps.

Byte-equivalence gate: T2 publishes 10,000 deterministic frames through a
plain ring and a turbo ring and requires **byte-identical ring contents** —
placement and policy change nothing observable.

### Prefetch wrappers

- `weft_turbo_begin/publish`: writer-thread-only (reads of `w_seq`, `ring`,
  `payload_bytes`, `slot_count` are the 02 §1 writer-private discipline);
  prefetch the slot being begun and the next slot respectively.
- `weft_turbo_prefetch_next(r)` / `weft_turbo_claim(r)`: advisory Relaxed
  load of ctrl[0] (`latestSeq` — the AXIOM T surface fanout.h documents),
  prefetch of the likely target slot's leading region, then the frozen
  claim. A stale guess wastes a hint, never a result.
- Prefetch is capped to a leading region (`WEFT_TURBO_PREFETCH_MAX_BYTES`,
  default 4096): the claim's copy touches the head first and streams the
  tail under the hardware prefetcher; full-width prefetch at 64 KiB was
  measured as pure instruction overhead (evidence: `tl-writer.log` and the
  cap rationale in `turbo.h`).

### Non-temporal streaming fill

`weft_turbo_fill`: SSE2 `_mm_stream_si128` body (baseline ISA — no -mavx
flags, no runtime dispatch) with relaxed-atomic word prologue/epilogue for
ragged edges, and a **load-bearing `_mm_sfence()` before return**: NT stores
are weakly ordered, and the frozen publish's Release stamp must never
retire ahead of a payload word. The begin invalidate (SeqCst store + fence)
precedes the fill in program order; the bracket is preserved end-to-end
(P1/P2 hold). T5 proves byte-identical output vs `weft_fanout_fill` on both
aligned (M=8) and ragged (M=3) geometries — the ragged geometry exists
because the first implementation had an 8-byte slot overrun at exactly that
geometry, caught by this gate and fixed (the fix comment in `turbo.c` cites
it).

### Kernel-bypass ingestion (uring_rx)

Ladder: `NONE → SYSCALL → RING → FIXED` (multishot + SQPOLL documented as
the ≥ 5.19 deployment rung, labeled PREDICTED). The probe hand-rolls the
five io_uring syscalls (no liburing dependency — rationale in `uring_rx.c`):
setup, ring mmaps, a NOP submit+enter proving the plumbing, and a buffer
registration attempt. **Depth-1 protocol** per `weft_uring_next`:

```
FIONREAD gate        (no bytes -> return 0, no seq burned)
cursor = begin()     (FI1: slotSeq[k] <- 0, SeqCst store + fence)
kernel fills slot    (RING: RECV SQE + one enter; SYSCALL: recv)
  short  -> tail zeroed, padded++
  long   -> head kept (MSG_TRUNC accounting), truncated++
  fail   -> aborted++ (burned seq counted; telescoping stays exact:
            the next claim accounts it as a drop — I7 identity intact)
publish()            (Release stamp + latestSeq)
```

Depth-1 is a *soundness* requirement, not a simplification: the frozen
`weft_fanout_publish()` stamps slot `w_slot` with `w_seq` — only the most
recent begin's pair — so out-of-order completion of multiple in-flight
recvs cannot be expressed without breaking the frozen API. Datagram fds
only (SO_TYPE enforced): FIONREAD counts datagrams, not bytes.

**Invariants touched** — none modified; all preserved by construction and
pinned by the T/U-series gates: I1 (this layer never writes stamps), I2
(prefetch is architecturally a no-op; NT fill adds only the sfence *inside*
the bracket), I3 (writer-side helpers are writer-thread-only; uring_rx is
single-consumer by contract), I4 (telescoping checked exact in T8/U3, where
the burned-seq accounting is exercised), I5 (hot paths allocation-free), I6
(untouched — kernel path only), I7 (drop accounting exact under torture),
I8 (hints and alignment only add locality).

**Litmus impact** — no L-series change (the kernel surface is untouched).
New conformance batteries: T1–T9 (turbo_test.c) and U1–U5 (uring_test.c),
run under plain/ASAN/TSAN and the all-seq_cst A/B regime (ordering-neutral
by requirement). Evidence: `litmus/evidence/turbo/`.

**Envelope impact** — none. The ring layout is the RFC-0004 byte contract,
unchanged; T2 proves it byte-for-byte.

## Boundary of the claim (Law 4)

MEASURED on the dev sandbox (2-vCPU shared Xeon VM, kernel 5.10; logs under
`litmus/evidence/turbo/`):

- Reader claim p50 at display cadence (500 µs, 1 MiB render-thrash between
  frames, background writer): **plain 567 ns → early-hint 413 ns (−27%)**;
  with pinning **p99 3803 ns → 2098 ns (−45%)**.
- Placement determinism: identical wrapper code measured **60–66 ns on the
  turbo mmap ring vs 60–590 ns on the posix-heap ring** across binary
  layouts — the mmap ring removes the address-layout lottery by
  construction. Root cause of the posix lottery is *not* fully isolated
  in-sandbox (no perf-counter access); the trail is committed, the claim is
  stated as "consistent with store→load 4K aliasing", not as fact.
- Prefault: zero minor-fault delta across a 1000-frame hot loop (T3).
- Ingestion: RING mode is **live and correct** (120k datagrams, zero torn
  frames, exact telescoping) but **~33% slower than SYSCALL at depth-1 on
  this kernel** — `enter()` is heavier than `recv()` here. The RING rung is
  a structural proof, not a throughput claim.
- NT streaming fill: byte-identical (T5) but **slower in the single-writer
  microbench** (sfence drain dominates with no concurrent reader pressure).
  Opt-in, measured, not recommended by default.

PREDICTED (hardware-gated, not claimed as measured): THP/TLB p99 deltas on
real hardware with deeper page-table walks (this VM's EPT makes TLB
behavior unrepresentative — TL-ring in-sandbox shows ≤ 5% p50); cross-NUMA
first-touch placement (single-node sandbox); FIXED/multishot/SQPOLL
syscall-free steady state (kernel ≥ 5.19); SCHED_FIFO jitter elimination
(EPERM in-sandbox, ladder verified).

## Alternatives considered

- **Modify the frozen kernels in place** (prefetch inside `fanout.c`,
  NT fill inside `weft.c`): rejected — the Kernel Freeze contract is the
  assignment's guardrail; every technique here rides a seam the frozen
  layers already expose (`attach_writer`, advisory ctrl reads,
  writer-private field discipline). Byte-diff on frozen files is zero.
- **Link liburing**: rejected — the repo builds with plain `gcc` and zero
  new link-time dependencies across its history (sha256_mb, shm_ring,
  gpu_ring all hand-roll their syscall surfaces); the ladder must degrade
  on partial kernels, and a library would launder the refusals our Law 4
  posture requires verbatim.
- **Depth-N io_uring (multiple in-flight recvs)**: rejected for soundness —
  out-of-order CQEs cannot be stamped through the frozen publish API (it
  stamps only the most recent begin's pair). Documented as the multishot
  design instead: kernel-chosen staging buffers + one copy, the throughput
  rung for ≥ 5.19 hosts.
- **`mlockall` unconditionally at attach**: rejected — 64 KiB
  RLIMIT_MEMLOCK in containers makes it a guaranteed refusal for real ring
  sizes; the graduated per-ring attempt + errno report is the honest shape.
- **Do nothing**: rejected — the directive is explicit, and the
  measured address-layout lottery is a defect class the frozen code cannot
  self-heal.

## Drawbacks

- Every wrapper adds ~5–8 ns on a benign layout (phase-probe measured); a
  hot loop that would never fault, migrate, or alias pays it forever.
- `weft_turbo_fill`'s sfence makes large-payload single-writer publishes
  *slower* in-sandbox — it exists for cache-pollution-dominated shapes and
  carries a measured-regression label rather than a recommendation.
- The NT body is outside strict C11 (sanctioned raw-cursor production path
  per fanout.h; TSAN-invisible like the documented plain-store path).
- One more module to port if Rust/TS surfaces want the same services
  (deferred — the C layer serves JNI/FFI native surfaces first).
- The prefetch cap (4096 B) is a heuristic; the optimal leading region is
  payload- and host-dependent.

## Open questions

- Root cause of the posix-ring aliasing lottery (needs perf counters /
  bare metal; the sandbox's virtualized MMU cannot answer it).
- Does the early-hint p50 win hold at 60 Hz (16.7 ms) cadence with real
  render workloads, or only at the 500 µs cadence measured?
- Depth-N/multishot ingestion on ≥ 5.19 kernels: does the one-extra-copy
  staging design beat depth-1 zero-copy at realistic feed rates?
- Should the capability probe gain a one-shot calibration (measure the
  aliasing penalty, auto-enable the mmap ring) instead of relying on the
  documented default?
