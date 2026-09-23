---
RFC: 0021
Title: weft-tensor — Realtime Edge-AI Zero-Copy Tensor Fabric (strided views, paged arenas, lock-free DMA tensor rings)
Status: Draft
Authors: systems-engineer-1 (Core DMA, Hardware Memory & Tensor Geometry Lead)
Created: 2026-09-20
Supersedes / Supersedes-by: none
---

# RFC 0021 — weft-tensor: Realtime Edge-AI Zero-Copy Tensor Fabric

## Summary

A new driver-layer module, `core/c/tensor/`, that makes tensors first-class
Weft citizens with zero serialization and zero copies from sensor DMA to
consumer decode. Three engines:

1. **`weft_tensor_view_t`** — a frozen 176-byte strided tensor descriptor
   (dtype, rank-8 shape, signed byte strides, byte window, backing
   address) whose decode is one pointer add:
   `addr(i) = base + byte_offset + Σ iₖ·strides[k]`. Slicing, cropping,
   transposing, and reshaping are zero-allocation view algebra.
2. **`weft_tensor_arena_t`** — page-aligned, mlock-able, optionally
   THP-hinted regions with an O(1) bump sub-allocator (measured 2.3-2.8
   ns/alloc on the 2-CPU sandbox) and an `attach` seam for unified-memory
   media (dma-buf mmap, Vulkan HOST_VISIBLE, Apple UMA).
3. **`weft_tensor_ring_t`** — a lock-free circular DMA tensor ring
   (MAP_SHARED, cacheline-aligned slots) with MPSC/SPMC/MMPC
   claim/commit/acquire/release semantics over bounded sequence cursors.
   Every commit publishes a full `weft_tensor_view_t` inside the slot:
   the consumer's decode is a pointer cast, and the release/acquire
   regime makes torn observation structurally impossible (50,000-message
   MPSC stress with 25.6M per-ticket word checks: zero tears; fork()
   torture with cross-process producers: zero tears).

The module is additive (no kernel file touched — Law 3), compiles with
plain C11 + POSIX (`-std=c11 -pthread`, zero dependencies), and is gated
by the WT-series battery (view WT1-16, arena WT17-24, ring WT25-40) in a
new `tensor-fabric` CI shard running plain + ASAN + TSAN legs.

## Motivation

The realtime edge-AI staging pipeline copies sensor data four to five
times before inference: kernel DMA buffer → user-space heap array →
framework tensor struct → accelerator VRAM → output marshalling back to
the UI model. On a 120 FPS control loop the copies alone burn 15-50ms —
the entire latency budget — and every copy is also an allocation
(Law 1 violation) with unbounded tail latency (Law 2 violation) on a
path that should be a pointer handoff.

Weft already owns the primitives that make the copies unnecessary: shared
memory across processes (RFC-0011 sessions), zero-copy fan-out rings
(RFC-0004), GPU-resident mappings (RFC-0003/0013), and frozen byte
layouts with ABI fingerprints (RFC-0017 weftc). What is missing is the
TENSOR geometry layer that lets those primitives carry
NPU/LLM-grade payloads: a descriptor that GPU uniform buffers, NPU
command streams, and UI hot-planes all decode identically; an allocator
that never mallocs on the inference path; and a ring whose slots carry
whole tensors, not opaque frames.

This RFC pins exactly that layer. Every claim below is executable: the
WT-series gates in `ci/scripts/run_tensor_shard.sh`.

## Guide-level explanation

A camera driver (or Engineer 3's ingestion layer) creates a ring sized
for its frame geometry, then claims a slot per frame, writes sensor
bytes directly into the slot payload, and commits a view describing the
geometry:

```c
weft_tensor_ring_t ring;
weft_tensor_ring_create(&ring, /*slots*/ 64, /*payload*/ 150528,
                        WEFT_TENSOR_RING_MODE_MPSC,
                        WEFT_TENSOR_RING_F_MLOCK | WEFT_TENSOR_RING_F_PREFAULT);

uint64_t ticket; uint8_t* payload; uint32_t cap;
weft_tensor_ring_claim(&ring, /*deadline*/ 2000000ull /*2ms*/,
                       &ticket, &payload, &cap);
// DMA/audio/callback writes the frame bytes at `payload` (64B-aligned)...

uint64_t nchw[4] = {1, 3, 224, 224};
weft_tensor_view_t view;
weft_tensor_view_init(&view, frame_id, WEFT_DTYPE_U8, 4, nchw,
                      (uintptr_t)payload, 0);
weft_tensor_ring_commit(&ring, ticket, &view, 150528);
```

The consumer (Engineer 2's NPU preproc, or the UI hot-plane) acquires and
decodes with a pointer cast — no parse, no copy, no allocation:

```c
const weft_tensor_view_t* v; const uint8_t* bytes; uint32_t used; uint64_t t;
weft_tensor_ring_acquire(&ring, 2000000ull, &t, &v, &bytes, &used);
// Zero-copy view algebra over the SAME memory:
weft_tensor_view_t roi, nhwc;
weft_tensor_view_subwindow(&roi, v, (uint64_t[4]){0,0,32,64},
                                   (uint64_t[4]){1,3,64,64});
weft_tensor_view_permute(&nhwc, &roi, (uint8_t[4]){0,2,3,1});  // NCHW->NHWC
float* px = weft_tensor_view_element_addr_at(&nhwc, idx, bytes);
weft_tensor_ring_release(&ring, t);
```

Arenas serve the compute side: activations, tiles, and logits live in
one pre-reserved region; `weft_tensor_arena_alloc` is a cursor bump
(measured 2.3-2.8 ns/alloc, 1M-alloc legs), `mark/rewind` recycles frames
in O(1), and nothing between create and destroy ever calls malloc (the
ASAN legs prove the zero-allocation data path).

Errors are explicit, named, and never silently downgraded: an unaligned
view is `WEFT_TENSOR_EMISALIGN` (Law 4), an out-of-bounds stride is
`WEFT_TENSOR_ERANGE`, a full ring under a deadline is
`WEFT_TENSOR_ETIMEOUT` — there is no unbounded-wait API anywhere.

## Reference-level specification

### §1 Element dtype registry (frozen wire numbers)

| dtype | value | size/align | role |
|---|---|---|---|
| `WEFT_DTYPE_U8` | 1 | 1 | camera planes, u8 logits |
| `WEFT_DTYPE_I8` | 2 | 1 | quantized NPU activations |
| `WEFT_DTYPE_I16` | 3 | 2 | audio PCM |
| `WEFT_DTYPE_F16` | 4 | 2 | GPU/Vulkan native |
| `WEFT_DTYPE_BF16` | 5 | 2 | TPU/ANE-class accelerators |
| `WEFT_DTYPE_F32` | 6 | 4 | canonical host interchange |
| `WEFT_DTYPE_F64` | 7 | 8 | physics/solver tensors |

Values appear in committed slot views; gaps are reserved — renumbering
is a protocol break. Natural alignment == element size for every dtype.

### §2 `weft_tensor_view_t` — the frozen ABI (176 bytes, 8-aligned)

| offset | field | type | notes |
|---|---|---|---|
| 0 | `tensor_id` | u64 | producer identity; never interpreted by the fabric |
| 8 | `dtype` | enum(4) | §1 registry |
| 12 | `ndim` | u8 | 1..8 |
| 13 | `_reserved[5]` | u8[5] | zero on the wire; dirty tails are rejected (EDIM) |
| 24 | `shape[8]` | u64[8] | element counts; canonical zero beyond ndim |
| 88 | `strides[8]` | i64[8] | BYTE deltas (signed — negative = flipped windows) |
| 152 | `byte_offset` | u64 | displacement of element (0,…,0) from payload base |
| 160 | `byte_length` | u64 | declared touchable span (see §3) |
| 168 | `physical_or_shm_addr` | uintptr | payload base — see §3 cross-process note |

`sizeof == 176` and every offset above are pinned by `_Static_assert` in
`weft_tensor.h` AND by runtime mirrors (WT2). Changing any of this is a
WTR1 version bump, not an edit.

### §3 The walking-extent wall (validate)

For a view with axes k:

```
U    = Σ_{strides[k]>0}  strides[k]·(shape[k]−1)
L    = −Σ_{strides[k]<0} |strides[k]|·(shape[k]−1)
span = |L| + U + dtype_size
```

`weft_tensor_view_validate(v, align_req)` proves, in order:

1. dtype ∈ §1 (EDTYPE); ndim ∈ 1..8 and shape/stride tails canonical
   zero (EDIM); shape[k] ≥ 1 (ERANGE); align_req ∈ {0,16,32,64,128}
   (EINVAL — the class registry is closed);
2. `byte_offset ≥ |L|` — a negative-stride window may never dip below
   the payload base (ERANGE). The canonical flipped-plane view
   (byte_offset = (H−1)·row_stride, first-row stride negative) lands
   exactly at zero and is accepted;
3. `byte_length ≥ span` — THE out-of-bounds-stride wall (ERANGE);
4. `byte_offset + U + dtype_size` must not wrap 2^64 (EOVERFLOW); every
   product/sum inside U, L, span is overflow-checked;
5. Law 4 alignment: `(base + byte_offset)` must satisfy dtype-natural
   alignment AND the requested class — violations are EMISALIGN, never
   rounded.

`physical_or_shm_addr` is the payload base in the WRITER's address
space: same-process or unified-memory consumers may use it directly;
cross-process consumers MUST pair the view with the payload pointer
their own mapping produced (`weft_tensor_ring_acquire` returns it — the
ring is fully relocatable). `validate()` never dereferences it.

`weft_tensor_view_element_offset` recomputes `byte_offset + Σ iₖ·sₖ` with
unsigned arithmetic and per-axis index bounds checks (ERANGE), refusing
any below-base underflow; multiplication overflow is EOVERFLOW.

### §4 View algebra (zero allocation: dst is caller stack storage)

| op | semantics | invariants |
|---|---|---|
| `init` | row-major contiguous strides; `byte_length = nelem·size` | INT64_MAX stride wall (EOVERFLOW beyond) |
| `init_strided` | explicit byte strides + declared span | §3 applies |
| `slice(axis, start, count)` | `byte_offset += start·s[axis]`; `shape[axis] = count` | tight span recomputed; window ⊂ parent's (walls hold by construction) |
| `subwindow(off[], cnt[])` | per-axis simultaneous slice | same |
| `permute(perm[])` | shape/strides permuted; bijection enforced (EINVAL) | byte_offset/byte_length/element set UNCHANGED |
| `reshape(new_shape)` | requires C-contiguity (ERESHAPE otherwise) and element-count preservation (ERESHAPE) | byte_offset passes through; byte_length preserved (monotone) |
| `is_contiguous` | trailing-product check; size-1 axes unconstrained | 0/1 |

There is deliberately NO −1 reshape wildcard and NO silent materializing
copy: a copy is Engineer 3's UI-plane decision, made explicitly.

### §5 Paged arenas

`weft_tensor_arena_create(capacity, flags)` mmaps page-aligned anonymous
memory (MAP_SHARED with `_F_SHARED` for fork sharing), rounds capacity
up to whole pages, and reports — never claims — placement outcomes:
`locked` (mlock applied/refused), `hugepage_hint` (MADV_HUGEPAGE
accepted/refused). `_F_PREFAULT` touches every page once. Law 2 note:
create/attach may allocate (one mmap); the data path never does.

`weft_tensor_arena_alloc(size, align, &off)` is a cursor bump: align up
(power-of-two class 1..4096, EINVAL otherwise), bounds-check, advance.
Exhaustion returns NULL + ENOMEM with the cursor unmoved and highwater
recorded — never a silent shrink. `mark/rewind/reset` are O(1). The bump
is SINGLE-THREAD by design (the sensor/inference thread owns its arena —
the kernel's writer-private discipline); no locks exist to contend.
`attach(memory, capacity)` adopts caller regions (dma-buf mmap, Vulkan
HOST_VISIBLE, SHM) with a 16B-minimum base and never frees them.

### §6 Ring wire format (WTR1, little-endian)

```
offset 0    control header (128 bytes — two cachelines)
offset 128  slot[0] .. slot[N-1],  slot_stride = 256 + payload_bytes
```

Control header (validated EXACTLY on attach; unknown bits rejected):

| off | field | notes |
|---|---|---|
| 0 | magic `WTR1` (u32) | 0x31525457 |
| 4 | version (u16) | 1 |
| 6 | mode (u16) | 1 MPSC / 2 SPMC / 3 MPMC |
| 8 | slot_count (u32) | power of two, ≤ 2^20 |
| 12 | payload_bytes (u32) | multiple of 64 (≤ 1 GiB) |
| 16 | slot_stride (u32) | must equal 256 + payload_bytes |
| 20 | flags (u32) | creator flags (advisory) |
| 24 | ring_bytes (u64) | must equal 128 + N·slot_stride |
| 32 | creator_pid / reserved0 | diagnostics / zero |
| 40 | created_unix_ns (u64) | diagnostics |
| 48 | `head` (atomic u64) | next ticket to CLAIM |
| 56 | `tail` (atomic u64) | next ticket to ACQUIRE |
| 64..88 | stat_committed/acquired/released/full_hits | relaxed counters |
| 96 | reserved[4] | zero; non-zero = EGEOMETRY |

Slot header (256 bytes = four cachelines, `_Static_assert`-pinned):

| off | field | notes |
|---|---|---|
| 0 | `seq` (atomic u64) | the protocol word (§7) |
| 8 | `weft_tensor_view_t` (176B) | published by commit |
| 184 | `payload_used` (u32) | bytes committed |
| 188 | `flags` (u32) | zero on wire |
| 192 | pad (64B) | keeps payloads cacheline-aligned |

`slot_stride = 256 + payload_bytes` with payload_bytes a 64 multiple
makes every slot payload 64B-aligned by construction (128B whenever
payload_bytes is a 128 multiple — Law 4). `weft_tensor_ring_create` maps
MAP_SHARED|MAP_ANONYMOUS (fork children and any process handed the
mapping share the ring — the DMA posture), zero-fills, writes identity
fields, and initializes `slot[i].seq = i` (the fresh-ring invariant).

### §7 Claim/commit protocol (Vyukov-style bounded sequence cursors)

Invariant — for slot index i ≡ t (mod N), over generations:

```
seq == t      slot is free for producer ticket t
seq == t+1    ticket t is COMMITTED (awaiting acquire)
seq == t+N    ticket t was acquired AND released (free for ticket t+N)
```

- **claim** (`try_claim`): load `head` (acquire); gate on
  `slot[head].seq == head` (acquire) — the authoritative fullness and
  generation check; CAS `head → head+1` (acq_rel) for a unique ticket.
  Losing the race is EAGAIN; a busy slot is EAGAIN (`stat_full_hits`).
  The winner owns the slot's payload exclusively until commit.
- **commit**: write payload (caller), copy + normalize the view
  (`physical_or_shm_addr := slot payload`, `byte_length := payload_used`),
  run the §3 wall against the slot's real payload address (view escapes
  and dtype-natural misalignment are rejected with the §9 codes), then
  ONE release-store `seq := t+1`. Every payload byte written before the
  release is visible to the consumer that acquire-loads `seq == t+1`:
  a committed tensor is never observed torn.
- **acquire** (`try_acquire`): load `tail` (acquire); gate on
  `slot[tail].seq == tail+1`; then advance `tail` FIRST — store for MPSC
  (single consumer owns the cursor), CAS for SPMC/MMPC (the loser saw
  the same ticket, lost the advance, returns EAGAIN without reading).
  Advance-before-read makes every ticket acquired by EXACTLY one
  consumer, structurally.
- **release**: ONE release-store `seq := t+N`. The consumer's reads
  happen-before the next producer's writes for that slot (the producer's
  claim acquire-loads `seq == its ticket`). Bidirectional hygiene with
  no fences beyond the release/acquire pairs the protocol already needs
  — x86 pays nothing extra, ARMv8/RISC-V get the ordering for free.

The cursors are 64-bit monotonic ("bounded"): the modulo arithmetic
never reuses a sequence number within the lifetime of a mapping
(2^64 messages at 1M msg/s ≈ 585,000 years). Misuse detection is
best-effort and documented, not defended: commit of an unowned ticket or
double release returns EAGAIN when the seq word disagrees; a release
without acquire is indistinguishable at the slot level (it simply drops
the message and stalls the tail — the next acquire times out; see
Boundary §).

Blocking forms (`claim`/`acquire` with an explicit `timeout_ns`) retry
the try-forms through the wait ladder. `timeout_ns == 0` is a pure try.
There is no infinite-wait entry point.

### §8 The wait ladder (Law 2)

```
rung 1: 64 × cpu_relax (pause/yield)      — fixed CPU cycles
rung 2: one sched_yield()                 — one syscall
rung 3: one 50 µs nanosleep               — one syscall
(the caller's deadline is checked between EVERY rung)
```

Steady-state claim/commit/acquire/release is pure shared-memory atomics —
zero syscalls (the S-series strace evidence class already established
this for the RFC-0004 ring the protocol mirrors). Under contention the
ladder runs bounded rungs; every blocking call carries the caller's
deadline. Measured (WT36): a full-ring claim with a 60 ms budget returns
ETIMEOUT after 60.0-60.1 ms.

### §9 Error registry

`WEFT_TENSOR_OK 0`, `EINVAL -1`, `ENOMEM -2`, `EAGAIN -3`, `ETIMEOUT -4`,
`ERANGE -5`, `EOVERFLOW -6`, `EMISALIGN -7`, `ERESHAPE -8`, `EDIM -9`,
`EDTYPE -10`, `EMAGIC -11`, `EGEOMETRY -12` — named by
`weft_tensor_status_name`, stable, and the ONLY values the API returns.

### Invariants touched

None of the kernel invariants I1-I6 change: this is a new driver-layer
subsystem riding the same memory-model posture (fenced acq/rel over
shared pages) the RFC-0004/0011 layers established. New module-local
invariants: the §3 walking-extent wall, the §6 wire tables, the §7
sequence-word invariant, and the exactly-once tail advance.

### Litmus impact

Additive: the WT-series (view WT1-16, arena WT17-24, ring WT25-40) plus
the Python-oracle golden fixture and its freshness gate. No existing
L/F/V/T/U/S test changes. The `tensor-fabric` CI shard (plain + ASAN +
TSAN; fork torture declared-skip under TSAN) is registered in the
extreme matrix under `any_code` (core/** already routes there) and in
the ci/README ownership table.

### Envelope impact

None. No kernel bytes, no fan-out wire format, no .weftrec, no schema.
`weft_tensor_ring_slot_t` embeds the RFC-0017-era tensor view layout —
Engineers 2/3 compile against this header unchanged.

## Boundary of the claim (Law 4)

What this enables, with its limits:

- **Zero-copy decode**, stated as: claim→commit→acquire→release moves
  zero payload bytes; decode is one pointer add. It does NOT remove
  copies a vendor driver itself insists on (TensorRT/ANE input heaps) —
  those ride the attach seams and remain Engineer 2's integration work.
- **Zero tearing**, stated as: the release/acquire regime makes every
  committed message observation atomic-by-construction; proven by 50K
  MPSC messages (25.6M per-ticket word checks), 50K SPMC, 20K MPMC, and
  10K fork-torture cross-process tensors — all zero tears, TSAN clean.
  It does NOT claim a torn-word-free hardware DMA engine writing the
  payload concurrently with the producer — the sensor side must use the
  claim/commit bracket (that is the contract; a DMA engine writing into
  an unclaimed slot is out of contract and undetectable here).
- **Bounded latency**, stated as: no unbounded waits exist; ladder rungs
  are fixed-size; deadlines honored (measured). The claim is NOT
  hard-real-time WCET certification — wcet-audit territory, not this
  module's.
- **Sub-microsecond arena sub-allocation**: measured 2.3-2.8 ns/alloc
  (1M-alloc legs, 3 reps, 2-CPU sandbox). Sandbox-class hardware, not
  silicon marketing.
- **Throughput**: 440K-560K verified msgs/s and 7.2-8.2 GB/s payload
  movement on the 2-CPU sandbox (16 KiB payloads, every word verified) —
  an honesty floor, not a peak: the verification pass IS the workload.
- **MPMC/MPSC strict FIFO**: tickets are consumed in ticket order; a
  slow producer stalls later tickets (head-of-line blocking) — Vyukov
  semantics, deliberate, documented. Latency-critical deployments should
  size N ≥ producers × in-flight depth.
- **Release without acquire** is not detectable at the slot level (the
  states coincide); it drops the message and stalls the tail — the next
  acquire hits its deadline and reports ETIMEOUT. Detection, not
  prevention.
- **Arena single-thread discipline**: the bump cursor has no locks by
  design; cross-thread use requires caller-owned handoff.
- **`physical_or_shm_addr`** is advisory cross-process (the writer's
  address space); consumers must use their own mapping's payload
  pointer. The API hands them exactly that.
- dma-buf/memfd fd import, ANE/TensorRT registration, WGSL-side decode
  (uniform layout), and sensor capture drivers are OTHER pillars' work —
  this RFC freezes the geometry + protocol they build on.

## Alternatives considered

1. **Do nothing** (ship views atop the RFC-0004 frame ring): loses typed
   geometry, dtype/alignment law enforcement, and per-slot descriptors;
   every consumer re-implements stride math badly. Rejected — the copy
   storm is the exact problem this project exists to kill.
2. **Disruptor-style commit watermark** (producers publish, a gating
   sequence advances over contiguous commits): decouples slow producers
   from FIFO stalls, but requires a second shared word and an advancement
   owner; on 2-CPU edge parts the extra cacheline ping outweighs the
   benefit at our depths. Rejected for v1; revisit if head-of-line
   blocking shows up in the field (Open Questions).
3. **Futex-based bounded waits**: fewer wakeups under heavy contention,
   but a syscall per wait on the uncontended path's tail and nontrivial
   cross-process futex semantics on non-Linux targets. The pause→yield→
   sleep ladder is portable and measurably honors deadlines (WT36);
   futex stays a documented follow-up, not v1.
4. **Per-slot variable sizes (span/offset allocation)**: would let one
   ring carry mixed geometries, at the cost of a second sub-allocator
   and fragmentation on the DMA path. Rejected — one ring per geometry
   class is the honest edge-AI pattern (camera ring, audio ring, logits
   ring); span rings are an Open Question.
5. **Element-unit strides (NumPy's older convention)**: rejected — byte
   strides are the zero-copy lingua franca (every DMA descriptor, WGSL
   `array<T>` layout, and Vulkan row pitch speaks bytes); element
   strides force a multiply into every decode.
6. **malloc-backed pools** for the arena: rejected — Law 1; also loses
   page alignment, mlock, and THP hints, and reintroduces allocator
   jitter the governor layer already fights.

## Drawbacks

- Every slot pays a 256-byte header (6.25% at 4 KiB payloads, 1.5% at
  16 KiB). Cacheline isolation was judged worth it; tiny-payload rings
  should use the frame ring (RFC-0004) instead.
- Strict FIFO (head-of-line blocking) — see Boundary.
- Single-thread arena bump limits naive sharing patterns.
- Fixed payload capacity per ring wastes memory for bursty mixed loads
  (alternative 4).
- The TSAN leg cannot run the fork torture (declared skip, not a silent
  pass) — fork+TSAN is unsupported in this tree; the plain and ASAN
  legs carry it.
- 128-byte control header reserves fields we do not use yet — the
  version gate rejects unknown bits rather than guessing, so growth
  costs a bump.

## Open questions

1. Should a future WTR2 add a producer-side "commit watermark" to
   decouple FIFO stalls (alternative 2), and at what depth does
   head-of-line blocking actually bite on silicon?
2. Span-based rings (alternative 4) for mixed-geometry ingestion —
   Engineer 3's audio+video mux may force the question.
3. Futex (Linux) / `os_sync_wait_on_address` (Darwin) wakeups for
   power-constrained waits — keep the ladder as fallback?
4. dma-buf/memfd fd-passing attach helpers: own them here or in the
   E2 GPU layer? (The attach seam exists; the fd plumbing is unowned.)
5. WGSL-side decode: does the view struct land in uniform buffers
   verbatim (E2's Pillar-2 codegen consumes the frozen table §2)?
6. Does the ANE path need a `payload_used`-independent alignment class
   (128B enforced at commit rather than by construction)?

## Implementation plan

Engineer 1 (this author), Pillar 2, on `feat/weft-tensor-core`:

- `core/c/tensor/` — `weft_tensor.h` (frozen contract), view/arena/ring
  engines, module Makefile (plain/ASAN/TSAN legs);
- `core/c/tensor/tests/` — WT1-40, the Python-oracle golden fixture +
  freshness gate, `run.sh`;
- `ci/scripts/run_tensor_shard.sh` + extreme-matrix registration +
  ci/README routing row; evidence under `litmus/evidence/tensor/`.

Acceptance criterion flipping Status to Implemented: the
`tensor-fabric` shard green on merged main (plain + ASAN + TSAN legs,
all WT checks), which this branch's local transcript already shows.

---

*Process notes: lazy consensus, 7 days — silence is consent; a substantiated
objection cites a Law, an invariant, or a litmus test. Kernel RFCs
additionally require both kernel-maintainer approvals. See
[CONTRIBUTING.md](../CONTRIBUTING.md) §3 and [GOVERNANCE.md](../GOVERNANCE.md).*
