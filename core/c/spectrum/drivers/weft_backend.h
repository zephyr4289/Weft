// weft_backend.h — WEFT SPECTRUM native driver execution table (Pillar 5, D-52).
//
// WHY EXISTS: the Weft fabric must extract the physical limits of every
// processor class on Earth (Hexagon DSP, Dimensity APU, Apple ANE, CUDA/
// TensorRT, Vulkan timeline queues, AVX-512/AMX, SVE2/NEON, RVV 1.0)
// without asking application developers to write a single line of
// vendor-specific code. When an application hands Weft a buffer or tensor
// stream, the spectrum driver pipeline must route the operation directly
// to the optimal hardware engine present on THIS chip, with zero heap
// traffic on the acceleration path and deterministic graceful degradation
// when an engine refuses.
//
// THIS FILE IS THE FROZEN INTEROP SURFACE between the three Pillar-5
// engineers (boundary contract, per the mission directive):
//
//   Engineer 1 (core governor) owns core/c/include/weft_spectrum.h — the
//     frozen weft_hw_profile_t descriptor and the micro-architecture
//     probing engine. The governor READS this header, derives its engine
//     mask from its hw profile, calls weft_backend_ctx_create(), and
//     invokes weft_backend_dispatch(). It never mutates probing state
//     owned here; this code never mutates governor state. The caps view
//     (weft_backend_caps_t) is the read-only projection the governor
//     schedules against — it is deliberately a SUBSET contract so the
//     full weft_hw_profile_t can evolve on Engineer 1's side without an
//     ABI bump here.
//
//   Engineer 3 (managed runtimes & SDKs) owns the TypeScript/Swift/Dart/
//     Python bindings and developer-facing fallbacks. Bindings consume
//     ONLY this header through clean, pure C-ABI dynamic dispatch
//     function pointers (weft_backend_ops_t below is exactly that: a
//     vtable of C function pointers, no C++ mangling, no language
//     runtime requirement). SDK fallbacks may be injected at runtime via
//     weft_backend_register() from the same table.
//
//   Engineer 2 (this file's owner) implements the backend drivers in
//     core/c/spectrum/drivers/, the SIMD vector engines in
//     core/c/spectrum/simd/ and the Vulkan loader in
//     core/c/spectrum/gpu/.
//
// LAWS (audited by tests/spectrum/native/ + tools/spectrum/tests/
// run_spectrum_native_suite.sh):
//   Law 1  ZERO heap allocation on the hot acceleration path. Kernel
//          launches, DMA enqueue and SIMD transforms allocate ZERO
//          bytes: command rings, descriptor pools, DMA handle tables,
//          log rings and scratch arenas are carved ONCE from the
//          context arena during weft_backend_ctx_create(). The module
//          heap lock (weft_backend_heap_lock) turns any post-init
//          allocation into a loud abort — the 5,000,000-cycle torture
//          leg proves 0-byte growth under ASan with three independent
//          witnesses (module counter, mallinfo2, /proc/self/statm).
//   Law 2  ZERO-COPY. No intermediate memcpy between CPU rings and
//          GPU/NPU engines. When unified memory is present (Apple
//          Silicon, Dimensity APU, Adreno, Tegra) the device address
//          ALIASES the host pointer — the mock transport asserts
//          pointer identity end-to-end and counts would-be copies
//          (must stay 0). Discrete engines (dGPU) receive dma-buf
//          imports, never staging copies.
//   Law 3  DETERMINISTIC GRACEFUL DEGRADATION. A refused engine (absent
//          driver, sandbox CI, hot-unplug) falls back to the SIMD
//          vector engine in < 1 us steady-state: the fallback hop is a
//          table walk over pre-probed, immutably ordered entries with
//          throttled (never blocking) honest logging — no crash, no
//          dropped op, no silent success lies.
//   Law 4  FAIL-CLOSED. Unknown op kinds, misaligned descriptors,
//          exhausted rings and saturated buses return explicit error
//          codes; nothing is silently rounded, retried or coerced.
//          Transient errors (EBUSY/ETIMEOUT) propagate to the caller;
//          only REFUSAL codes (ENOTSUP/EDEVICE/EREFUSED) fall back.
//   Law 5  BIT-EXACTNESS. Every SIMD implementation of a kernel family
//          produces byte-identical results to the scalar reference —
//          by construction (element-wise ops, lane-per-output
//          sequential-K accumulation with IEEE-754 single-rounding
//          fused multiply-add, modular chunked Fletcher arithmetic),
//          proven by the all-paths-equal oracle sweep, not by
//          assertion.
//
// HONESTY BOUNDARY (per the repo's per-port honesty culture): the
// executable-verified surface of this module is x86_64 Linux (plain,
// ASan, UBSan, TSan legs on the CI sandbox: AVX-512 + AVX2 + FMA
// silicon). aarch64 NEON/SVE2 and riscv64 RVV 1.0 legs are
// compile-guarded for their native CI runners (apple-packages /
// linux-arm64 / riscv-port shards) and are NOT executable-tested in the
// x86_64 sandbox — declared. Bit-exactness across those ISAs holds by
// the construction arguments above (verified per-ISA by the same
// oracle battery on their runners).
//
// PROVENANCE: house pattern (sha256_hw.c / fanout_simd.c) for dispatch —
// per-function target attributes, one runtime CPU probe resolved into
// relaxed-atomic function pointers, force pins for A/B benches and the
// bit-identity gate, honest impl-name strings. House error style
// (weft_tensor.h §2): zero success, negative errno-adjacent codes, no
// errno laundering.

#ifndef WEFT_BACKEND_H
#define WEFT_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===========================================================================
// §0 ABI versioning (frozen; a change to anything _Static_assert-ed below
//    is a protocol version bump, not an edit)
// ===========================================================================

#define WEFT_BACKEND_ABI_VERSION 1u

// ===========================================================================
// §1 Status registry (every API returns one of these; no errno laundering)
// ===========================================================================

typedef enum {
    WEFT_BACKEND_OK           =  0,   ///< success

    WEFT_BACKEND_EINVAL       = -1,   ///< NULL/zero argument, bad flag, bad dims
    WEFT_BACKEND_ENOTSUP      = -2,   ///< op kind not in this backend's affinity
    WEFT_BACKEND_EBUSY        = -3,   ///< bus saturated / ring full / token bucket empty
    WEFT_BACKEND_EDEVICE      = -4,   ///< device gone (hot-unplug, driver crash)
    WEFT_BACKEND_EREFUSED     = -5,   ///< engine refused (no driver, sandbox CI, no perm)
    WEFT_BACKEND_ETIMEOUT     = -6,   ///< bounded wait exceeded caller deadline
    WEFT_BACKEND_ENOMEM       = -7,   ///< arena exhausted (INIT-TIME ONLY; Law 1 forbids
                                      ///< this on the hot path)
    WEFT_BACKEND_ESTATE       = -8,   ///< wrong lifecycle state / registry misuse
    WEFT_BACKEND_ECHECKSUM    = -9,   ///< seqlock checksum mismatch (torn read detected)
    WEFT_BACKEND_EMISALIGN    = -10,  ///< alignment law violated (Law 4: never rounded)
    WEFT_BACKEND_ERANGE       = -11,  ///< index/size out of bounds
    WEFT_BACKEND_EABI         = -12,  ///< backend ABI version mismatch

    // REFUSAL set — the ONLY codes that trigger a fallback hop (Law 3).
    // EBUSY/ETIMEOUT are transient: propagated to the caller for retry
    // policy, never silently re-routed (a saturated GPU must not quietly
    // dump work onto the CPU without the caller seeing the decision).
} weft_backend_status_t;

// ===========================================================================
// §2 Vendor / engine identity (frozen wire numbers; gaps reserved)
// ===========================================================================

typedef enum {
    WEFT_VENDOR_QUALCOMM  = 1,   ///< FastRPC -> Hexagon Tensor DSP / Adreno
    WEFT_VENDOR_MEDIATEK  = 2,   ///< Neuropilot -> Dimensity APU / Mali Immortalis
    WEFT_VENDOR_APPLE     = 3,   ///< Metal 3 unified memory -> ANE arena
    WEFT_VENDOR_NVIDIA_PC = 4,   ///< CUDA / TensorRT / Vulkan 1.3 timeline / AVX-512/AMX
    WEFT_VENDOR_RISCV_ARM = 5,   ///< CPU vector engine: SVE2/NEON (ARM64), RVV 1.0 (riscv64)
    WEFT_VENDOR_CPU_SIMD  = 6,   ///< terminal guaranteed engine (scalar + dispatched SIMD)
    WEFT_VENDOR_MOCK      = 0x4D4F4B43u,  ///< 'MOCK' — synthetic hardware harness only
} weft_vendor_id_t;

#define WEFT_VENDOR_COUNT 6u   ///< count of REAL vendor slots (mock excluded)

typedef enum {
    WEFT_ENGINE_NPU         = 0,   ///< Apple ANE, Dimensity APU, Hexagon tensor cores
    WEFT_ENGINE_GPU         = 1,   ///< Adreno, Mali, Metal, CUDA, Vulkan compute
    WEFT_ENGINE_DSP         = 2,   ///< Hexagon generic
    WEFT_ENGINE_CPU_VECTOR  = 3,   ///< AVX-512/AVX2/AMX/SVE2/NEON/RVV
    WEFT_ENGINE_CPU_SCALAR  = 4,   ///< guaranteed terminal fallback
} weft_engine_class_t;

/// Engine-class admission mask bits for weft_backend_cfg_t::engine_mask.
#define WEFT_ENGINE_MASK_NPU         (1u << 0)
#define WEFT_ENGINE_MASK_GPU         (1u << 1)
#define WEFT_ENGINE_MASK_DSP         (1u << 2)
#define WEFT_ENGINE_MASK_CPU_VECTOR  (1u << 3)
#define WEFT_ENGINE_MASK_CPU_SCALAR  (1u << 4)
#define WEFT_ENGINE_MASK_ALL         0x1Fu

// ===========================================================================
// §3 Operation kinds (the unified op surface every backend speaks)
// ===========================================================================

typedef enum {
    WEFT_OP_NORMALIZE_F32      = 1,  ///< out[i] = (x[i] - f0) * f1  (element-wise)
    WEFT_OP_DELTA_ENCODE_U32   = 2,  ///< out[0]=in[0]-u0; out[i]=in[i]-in[i-1]  (modular)
    WEFT_OP_DELTA_DECODE_U32   = 3,  ///< out[i] = u0 + sum_{j<=i} in[j]         (modular)
    WEFT_OP_DOT_F32            = 4,  ///< C[m,n] = A[m,k] . B[k,n]  (strict seq-K, fused)
    WEFT_OP_SEQLOCK_CHECKSUM   = 5,  ///< Fletcher-32 + seqlock-stamp mix -> result_u64
} weft_op_kind_t;

/// Affinity mask bit for a kind k: (1ULL << k). Backends declare which kinds
/// they ACCELERATE; dispatch walks entries in priority order and skips any
/// whose affinity lacks the op's bit (a hop, not an error).
#define WEFT_OP_AFFINITY(k) (1ULL << (k))

#define WEFT_OP_FLAG_INPLACE (1u << 0)   ///< bufs[0] doubles as destination

// ===========================================================================
// §4 Buffer descriptor — the zero-copy currency (Law 2)
// ===========================================================================

/// Buffer dtype values this ABI knows natively. Frozen. (Interop note for
/// Engineer 3: tensor dtype WEFT_DTYPE_F32 == 6 maps to WEFT_BACKEND_DTYPE_F32;
/// U32 has no tensor-layer equivalent — the delta codecs own it.)
typedef enum {
    WEFT_BACKEND_DTYPE_UNKNOWN = 0,
    WEFT_BACKEND_DTYPE_F32     = 1,
    WEFT_BACKEND_DTYPE_U32     = 2,
} weft_backend_dtype_t;

#define WEFT_BUF_HOST      (1u << 0)   ///< host-visible memory
#define WEFT_BUF_DEVICE   (1u << 1)   ///< device-resident (mapped by an engine)
#define WEFT_BUF_UNIFIED  (1u << 2)   ///< host ptr aliases device ptr (UMA)
#define WEFT_BUF_PINNED   (1u << 3)   ///< page-locked for DMA (cudaHostAlloc-class)
#define WEFT_BUF_IMPORTED (1u << 4)   ///< buffer came in via dma-buf/IOSurface import
#define WEFT_BUF_FD_VALID (1u << 5)   ///< fd carries a dma-buf/IOSurface handle
#define WEFT_BUF_WRITABLE (1u << 6)

/// One buffer, 64 bytes = one cache line (Pillar-1 alignment mandate).
/// data is the HOST address; on unified-memory engines the device address
/// aliases it exactly (Law 2 — the transport's map() is the only place a
/// device handle may be attached, and it never copies).
typedef struct {
    void*    data;      ///< host virtual address (aliased on UMA engines)
    uint64_t bytes;     ///< payload size in bytes
    uint64_t dma_tag;   ///< opaque device handle filled by transport map()
    uint32_t dtype;     ///< weft_backend_dtype_t
    uint32_t flags;     ///< WEFT_BUF_* bits
    int32_t  fd;        ///< dma-buf / IOSurface handle, or -1
    uint32_t reserved0;
    uint64_t reserved1[3];
} weft_buffer_desc_t;

// ===========================================================================
// §5 Operation descriptor — what the governor dispatches (frozen layout)
// ===========================================================================

/// Buffer slot conventions per kind:
///   NORMALIZE_F32    bufs[0]=src, bufs[2]=dst (INPLACE: bufs[0] only)
///   DELTA_*_U32      bufs[0]=src, bufs[2]=dst (INPLACE: bufs[0] only)
///   DOT_F32          bufs[0]=A, bufs[1]=B, bufs[2]=C
///   SEQLOCK_CHECKSUM bufs[0]=src (result returned via result_u64)
/// Element counts: m for the streaming kinds (normalize/delta/checksum:
/// m = element count); DOT_F32 uses m,k,n + lda,ldb,ldc row-major strides.
/// NORMALIZE params: f0 = min, f1 = rs (caller-precomputed reciprocal span
/// times scale — the single-multiply contract; see simd/weft_simd.h).
/// DELTA params: u0 = seed. CHECKSUM params: u0 = observed seqlock stamp.
typedef struct {
    uint32_t kind;        ///< weft_op_kind_t
    uint32_t flags;       ///< WEFT_OP_FLAG_*
    uint32_t m, k, n;     ///< dims (see above)
    uint32_t lda, ldb, ldc;
    float    f0, f1;      ///< normalize: min, rs
    uint32_t u0, u1;      ///< delta: seed / checksum: seqlock stamp
    uint32_t reserved0, reserved1;
    weft_buffer_desc_t bufs[3];
} weft_op_desc_t;

/// Dispatch result — written into the CALLER'S storage (zero-alloc, Law 1).
typedef struct {
    uint64_t result_u64;     ///< SEQLOCK_CHECKSUM digest
    uint64_t completed_seq;  ///< engine completion sequence
    uint64_t enqueue_ns;     ///< monotonic ns at enqueue
    uint64_t complete_ns;    ///< monotonic ns at completion
    uint64_t device_ns;      ///< time attributed to the engine (emulated DMA incl.)
    uint32_t backend_index;  ///< executing entry in the context table
    uint32_t engine_class;   ///< weft_engine_class_t of the executor
    uint32_t fallback_hops;  ///< engines refused before the executor (Law 3)
    int32_t  status;         ///< final status (== return value)
    uint64_t reserved[4];
} weft_dispatch_result_t;

// ===========================================================================
// §6 Capability view — the read-only projection the governor schedules on
// ===========================================================================

#define WEFT_CAPS_UNIFIED_MEM    (1u << 0)  ///< host ptr aliases device ptr
#define WEFT_CAPS_DMA_BUF_IMPORT (1u << 1)  ///< cross-engine fd import supported
#define WEFT_CAPS_TIMELINE_SEM   (1u << 2)  ///< Vulkan 1.3 / CUDA timeline sync
#define WEFT_CAPS_PINNED_ALLOC   (1u << 3)  ///< page-locked host allocator
#define WEFT_CAPS_ZERO_COPY      (1u << 4)  ///< no staging copies, ever

typedef struct {
    uint32_t abi_version;          ///< must equal WEFT_BACKEND_ABI_VERSION
    uint32_t vendor_id;            ///< weft_vendor_id_t
    uint32_t engine_class;         ///< weft_engine_class_t
    uint32_t flags;                ///< WEFT_CAPS_*
    uint64_t op_affinity;          ///< WEFT_OP_AFFINITY(kind) bits
    uint64_t device_memory_bytes;  ///< 0 for CPU engines
    int32_t  score;                ///< 0..1000 capability score (ordering hint)
    uint32_t reserved[4];
    char     impl_name[32];        ///< honest identity, e.g. "fastrpc+hexagon-v73"
} weft_backend_caps_t;

// ===========================================================================
// §7 THE DRIVER EXECUTION TABLE — pure C-ABI dynamic dispatch (frozen)
// ===========================================================================

/// Instance lifecycle cfg passed to ops->init(). owner is the opaque
/// weft_backend_ctx_t; arena_alloc carves driver state from the context
/// arena (init-time only — Law 1); log emits a throttled record through the
/// context ring (never blocks, never formats on the hot path).
typedef struct {
    void*    owner;                 ///< opaque weft_backend_ctx_t*
    const struct weft_dma_transport_s* transport;  ///< injected seam (mock/real)
    weft_backend_status_t (*arena_alloc)(void* owner, uint64_t bytes,
                                         uint64_t align, void** out);
    void (*log)(void* owner, uint32_t code, uint32_t backend_idx, uint32_t aux);
    uint32_t flags;                 ///< ctx flags the driver must respect
    uint32_t engine_mask;           ///< admission mask the driver was admitted under
    uint32_t cmd_ring_slots;        ///< ctx-derived command ring depth for this driver
    uint32_t dma_map_slots;         ///< ctx-derived DMA handle table depth for this driver
} weft_backend_init_cfg_t;

/// The unified backend vtable. Every hardware engine on Earth speaks THIS
/// shape: probe -> init -> execute/submit -> sync -> shutdown. All function
/// pointers are pure C ABI (Engineer 3's managed runtimes bind them
/// directly). self is the driver's state block (ops->state_bytes carved
/// from the context arena at init — driver files hold ZERO global mutable
/// state, so any number of contexts coexist TSan-clean).
typedef struct __attribute__((aligned(64))) {
    const char* name;                 ///< stable identity string
    uint32_t   abi_version;           ///< WEFT_BACKEND_ABI_VERSION
    uint32_t   vendor_id;             ///< weft_vendor_id_t
    uint32_t   engine_class;          ///< weft_engine_class_t
    uint32_t   state_bytes;           ///< driver state block size (arena-carved)
    uint32_t   reserved0;

    /// Cold: can this backend run HERE? Refusal is honest (EREFUSED with
    /// impl_name explaining why). Never called after table build.
    weft_backend_status_t (*probe)(weft_backend_caps_t* caps_out);

    /// Cold: initialize driver state (self, cfg->state_bytes). May allocate
    /// ONLY through cfg->arena_alloc. A failing init kills the entry
    /// (fail-closed) — the entry is logged dead with its reason.
    weft_backend_status_t (*init)(void* self, const weft_backend_init_cfg_t* cfg);

    /// HOT: synchronous single-op execution. Zero heap (Law 1), zero copy
    /// (Law 2). Refusal codes hop; transient codes propagate (Law 3/4).
    weft_backend_status_t (*execute)(void* self, const weft_op_desc_t* op,
                                     weft_dispatch_result_t* out);

    /// HOT: enqueue a homogeneous op batch into the engine command ring
    /// (pre-formed weft_cmd_pkt_t, see weft_dma.h). Zero heap.
    weft_backend_status_t (*submit)(void* self, const weft_op_desc_t* ops,
                                    uint32_t count);

    /// Bounded wait for completion_seq (Law: no unbounded spinning; the
    /// wait ladder mirrors the tensor module's — pause / yield / sleep).
    weft_backend_status_t (*sync)(void* self, uint64_t completion_seq,
                                  uint64_t timeout_ns);

    /// Cold: release driver state (arena reclaimed wholesale at ctx destroy).
    void (*shutdown)(void* self);
} weft_backend_ops_t;

// ===========================================================================
// §8 Context configuration + lifecycle
// ===========================================================================

#define WEFT_BACKEND_DEFAULT_CMD_RING   256u
#define WEFT_BACKEND_DEFAULT_DMA_MAPS   256u
#define WEFT_BACKEND_MAX_BACKENDS       32u
#define WEFT_BACKEND_ARENA_DEFAULT      (1u << 20)  ///< 1 MiB scratch arena

#define WEFT_CTX_FLAG_STRICT_ZERO_COPY  (1u << 0)  ///< assert aliasing end-to-end
#define WEFT_CTX_FLAG_ALLOW_MOCK        (1u << 1)  ///< admit WEFT_VENDOR_MOCK entries
#define WEFT_CTX_LOG_OFF                0u
#define WEFT_CTX_LOG_ERROR              1u
#define WEFT_CTX_LOG_INFO               2u   ///< default: fallbacks + refusals
#define WEFT_CTX_LOG_DEBUG              3u

typedef struct {
    uint32_t engine_mask;         ///< WEFT_ENGINE_MASK_* (default ALL)
    uint32_t log_level;           ///< WEFT_CTX_LOG_* (default INFO)
    uint32_t flags;               ///< WEFT_CTX_FLAG_*
    uint32_t cmd_ring_slots;      ///< 0 -> default 256
    uint32_t dma_map_slots;       ///< 0 -> default 256
    uint32_t reserved0;
    uint64_t arena_bytes;         ///< 0 -> default 1 MiB
    /// Mock/transport seam: per-vendor transport override. Non-NULL entries
    /// are injected into that vendor driver at init — the seam that makes
    /// every vendor code path 100% testable on headless CI (mandate C).
    const struct weft_dma_transport_s* transport_overrides[WEFT_VENDOR_COUNT];
} weft_backend_cfg_t;

/// Opaque context. Created with the heap OPEN (drivers may carve state),
/// then the module heap lock engages — every dispatch thereafter is
/// provably allocation-free (Law 1).
typedef struct weft_backend_ctx_s weft_backend_ctx_t;

/// Build the execution table: probe every registered backend, admit by
/// engine_mask, order deterministically (engine class asc, score desc,
/// vendor_id asc, registration order), init each admitted entry, then LOCK
/// the heap. Returns NULL + logs honestly on fatal misconfiguration.
/// The ctx owns one aligned allocation (context + arena).
weft_backend_ctx_t* weft_backend_ctx_create(const weft_backend_cfg_t* cfg);

/// Flush the log ring and release the whole context (arena reclaimed
/// wholesale; driver shutdown hooks run first). Heap lock released.
void weft_backend_ctx_destroy(weft_backend_ctx_t* ctx);

/// HOT PATH — the dispatch the governor invokes. Walks the immutable
/// table: first entry whose affinity covers op->kind executes it; refusal
/// codes hop with throttled honest logging; transient codes propagate.
/// Guaranteed to terminate on the terminal CPU engine. Zero heap, zero
/// copy, fallback steady-state < 1 us (Law 3).
weft_backend_status_t weft_backend_dispatch(weft_backend_ctx_t* ctx,
                                             const weft_op_desc_t* op,
                                             weft_dispatch_result_t* out);

/// Batch enqueue (homogeneous kinds; EINVAL otherwise) + bounded sync.
weft_backend_status_t weft_backend_submit(weft_backend_ctx_t* ctx,
                                          const weft_op_desc_t* ops,
                                          uint32_t count);
weft_backend_status_t weft_backend_sync(weft_backend_ctx_t* ctx,
                                        uint64_t completion_seq,
                                        uint64_t timeout_ns);

// ===========================================================================
// §9 Registry (builtins + Engineer 3's runtime SDK fallbacks)
// ===========================================================================

/// The builtin ops table (drivers + terminal CPU engine). Frozen order;
/// pure C-ABI function pointers (Engineer 3 binds these directly).
const weft_backend_ops_t* const* weft_backend_registry_ops(uint32_t* count_out);

/// Runtime registration (init-time, single-threaded bootstrap contract —
/// the governor owns serialization during startup). Post-ctx-create
/// registration returns ESTATE (tables are immutable once built: TSan-clean
/// lock-free reads afterwards). ABI mismatch is refused fail-closed.
weft_backend_status_t weft_backend_register(const weft_backend_ops_t* ops);
weft_backend_status_t weft_backend_unregister(const weft_backend_ops_t* ops);

// ===========================================================================
// §10 Introspection, heap discipline, logging — the honest ledger
// ===========================================================================

typedef struct {
    uint32_t vendor_id;
    uint32_t engine_class;
    int32_t  score;
    int32_t  state;          ///< 1 = live, 0 = dead, -1 = dead-during-init
    uint32_t flags;          ///< WEFT_CAPS_* of the live entry
    uint64_t op_affinity;
    uint64_t device_memory_bytes;
    char     impl_name[32];
    int32_t  death_reason;   ///< status code that killed it (0 if live)
} weft_backend_info_t;

/// Snapshot the context table (bounded by cap; returns the entry count).
uint32_t weft_backend_table_info(const weft_backend_ctx_t* ctx,
                                 weft_backend_info_t* out, uint32_t cap);

/// Total bytes currently allocated by this module ( Law 1 witness #1 ).
uint64_t weft_backend_heap_bytes(void);
/// Non-zero once the hot-path heap lock is engaged.
int      weft_backend_heap_locked(void);
/// Engage/release the module heap lock (engaged automatically at the end of
/// ctx_create; release exists for the torture batteries' bracketing only).
void     weft_backend_heap_lock(void);
void     weft_backend_heap_unlock(void);

/// Log codes (records are 32B fixed, pre-allocated ring; suppression is
/// first-occurrence + every 65536th per code — honest, never blocking).
typedef enum {
    WEFT_LOG_PROBE_REFUSED   = 1,  ///< aux = vendor_id, backend_idx identifies entry
    WEFT_LOG_INIT_FAILED     = 2,
    WEFT_LOG_FALLBACK_HOP    = 3,  ///< aux = refusal code
    WEFT_LOG_BUS_SATURATED   = 4,
    WEFT_LOG_DEVICE_GONE     = 5,
    WEFT_LOG_TORN_READ       = 6,
    WEFT_LOG_HEAP_VIOLATION  = 7,  ///< fatal: abort follows
    WEFT_LOG_MOCK_EVENT      = 8,
    WEFT_LOG_DISPATCH_DEBUG  = 9,
} weft_log_code_t;

/// Flush the log ring to stderr (cold: shutdown, test checkpoints).
void weft_backend_flush_log(weft_backend_ctx_t* ctx);

/// Counters snapshot — the evidence the audit report reads.
typedef struct {
    uint64_t dispatches;
    uint64_t fallback_hops;
    uint64_t refusals;
    uint64_t busy_propagated;
    uint64_t device_gone;
    uint64_t bytes_submitted;
    uint64_t torn_detected;
    uint64_t log_records_dropped;
} weft_backend_stats_t;

void weft_backend_stats(const weft_backend_ctx_t* ctx,
                        weft_backend_stats_t* out);

/// Monotonic nanoseconds (vDSO CLOCK_MONOTONIC; the module's clock for
/// enqueue/complete/device timestamps and the bench suite).
uint64_t weft_backend_now_ns(void);

// ===========================================================================
// §11 ABI freeze (Pillar-1 cache-line mandates — pinned, not conventional)
// ===========================================================================

_Static_assert(sizeof(weft_buffer_desc_t) == 64,
               "weft_buffer_desc_t must be exactly one 64B cache line");
_Static_assert(sizeof(weft_op_desc_t) <= 256,
               "weft_op_desc_t must stay within 4 cache lines");
_Static_assert(sizeof(weft_backend_caps_t) <= 96,
               "weft_backend_caps_t must stay within 2 cache lines");
_Static_assert(sizeof(weft_dispatch_result_t) <= 128,
               "weft_dispatch_result_t must stay within 2 cache lines");
_Static_assert(_Alignof(weft_backend_ops_t) == 64,
               "weft_backend_ops_t must be 64B-aligned (cache-line mandate)");
_Static_assert(sizeof(void*) == 8,
               "this frozen ABI assumes LP64 pointer width");

#ifdef __cplusplus
}
#endif

#endif // WEFT_BACKEND_H
