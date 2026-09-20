// weft_tensor.h — Realtime Edge-AI zero-copy tensor fabric (RFC-0021).
//
// WHY EXISTS: the traditional edge-AI staging pipeline copies sensor data
// 4-5 times before inference (kernel DMA buffer -> user heap -> framework
// tensor -> accelerator VRAM -> output marshalling), burning 15-50ms and
// destroying 120 FPS control loops. This module is the Pillar-2 foundation
// that removes every one of those copies for the Weft fabric:
//
//   weft_tensor_view_t   the unified strided tensor descriptor — one
//                        pointer-cast decode of any tensor living in a
//                        ring slot, an arena, an SHM mapping, a dma-buf
//                        region, or GPU-mappable host memory;
//   weft_tensor_arena_t  page-aligned, mlock-able, sub-microsecond bump
//                        arenas (zero malloc/free on the inference path);
//   weft_tensor_ring_t   the lock-free circular DMA tensor ring — MPSC /
//                        SPMC / MPMC claim/commit over one shared mapping
//                        with bounded sequence cursors and a bounded wait
//                        ladder (no spin locks, no unbounded spinning).
//
// The tensor view struct below is the FROZEN ABI surface Engineers 2
// (NPU/GPU kernels) and 3 (sensor streaming / UI hot-plane) compile
// against; its byte layout is pinned by _Static_assert and by the WT-series
// ABI-freeze test. Any change to it is a protocol version bump, not an
// edit.
//
// LAWS (this module is audited against them in ci run_tensor_shard.sh):
//   Law 1  zero heap allocation on the inference path — view/arena/ring
//          create+attach MAY allocate (setup, one mmap each); claim,
//          commit, acquire, release, alloc, slice, reshape allocate
//          NOTHING (stack views + pre-reserved arenas only; ASAN legs
//          prove no hidden allocation).
//   Law 2  bounded latency, no spin locks — steady-state claim/commit is
//          pure shared-memory atomics (zero syscalls); under contention
//          the wait ladder runs FIXED-SIZE rungs (64 pause cycles -> one
//          sched_yield -> one 50us nanosleep) with the caller's deadline
//          checked between every rung. There is no infinite-wait API:
//          every blocking entry point takes an explicit timeout_ns.
//   Law 3  kernel byte-frozen — this module lives under core/c/tensor/
//          and touches no kernel file (weft.{c,h}, fanout.{c,h},
//          frame_cursor.{c,h}, lib.rs, weft.ts); the freeze is proven by
//          the shard's git-diff gate.
//   Law 4  honest alignment bounds — 16/32/64/128B requirements are
//          enforced with explicit error codes (WEFT_TENSOR_EMISALIGN),
//          never silently rounded; ring slot payloads are 64B-aligned by
//          construction (128B when payload_bytes is a 128 multiple).
//
// ADDRESS FORMULA (the whole decode story — there is nothing else):
//
//     addr(i0..i{ndim-1}) = base + byte_offset + sum_k  i_k * strides[k]
//
//   strides are canonically BYTES (signed: negative strides are legal
//   views, e.g. flipped images); base is the payload address in YOUR
//   address space (same-process: physical_or_shm_addr; cross-process: the
//   payload pointer weft_tensor_ring_acquire handed you). Decoding a
//   tensor from any of those media is therefore a pointer add — 0 ns.
//
// Layer discipline: driver layer, additive-only, zero dependencies beyond
// C11 + POSIX (mmap/mlock/sched_yield/nanosleep; no libuv, no malloc in
// the data path). Honesty boundary: the EXECUTABLE-verified surface here
// is x86_64 POSIX (plain/ASAN/TSAN legs, fork torture for the
// cross-process claim); aarch64 compiles clean (yield intrinsic) but has
// no silicon leg in this tree — declared. dma-buf/ANE/TensorRT importer
// seams are ATTACH points (weft_tensor_arena_attach / ring attach over a
// caller mapping), owned by Engineers 2/3 — this module pins the geometry
// and the claim/commit protocol, not vendor drivers.

#ifndef WEFT_TENSOR_H
#define WEFT_TENSOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===========================================================================
// §1 Element types
// ===========================================================================

/// Element dtypes for the sensor->NPU->UI pipeline. Values are FROZEN wire
/// numbers (they appear in ring slots and cross-process views); gaps are
/// reserved for future widening (do not renumber).
typedef enum {
    WEFT_DTYPE_U8   = 1,   ///< unsigned 8-bit (camera planes, logits-u8)
    WEFT_DTYPE_I8   = 2,   ///< signed 8-bit (quantized NPU weights/activations)
    WEFT_DTYPE_I16  = 3,   ///< signed 16-bit (audio PCM before f32 lift)
    WEFT_DTYPE_F16  = 4,   ///< IEEE 754 half (GPU/Vulkan native)
    WEFT_DTYPE_BF16 = 5,   ///< bfloat16 (TPU/ANE-class accelerators)
    WEFT_DTYPE_F32  = 6,   ///< IEEE 754 single (canonical host interchange)
    WEFT_DTYPE_F64  = 7,   ///< IEEE 754 double (physics/solver tensors)
} weft_dtype_t;

/// Maximum rank this fabric addresses (NCHW + batch + time is 5; 8 leaves
/// headroom for e.g. KV-cache blocks). FROZEN: baked into the view struct.
#define WEFT_TENSOR_MAX_DIMS 8

/// Byte size of one element; 0 for unknown dtype values.
uint32_t weft_dtype_size(weft_dtype_t dt);

/// Natural alignment of one element (== size for every dtype in §1; F16/
/// BF16 are 2-byte aligned on every target we ship). 0 for unknown.
uint32_t weft_dtype_align(weft_dtype_t dt);

/// Short lowercase name ("u8".."f64"), "unknown" for out-of-range values.
const char* weft_dtype_name(weft_dtype_t dt);

// ===========================================================================
// §2 Status registry (every API returns one of these; no errno laundering)
// ===========================================================================

typedef enum {
    WEFT_TENSOR_OK        =  0,   ///< success
    WEFT_TENSOR_EINVAL    = -1,   ///< NULL/zero argument, non-power-of-2, bad flag
    WEFT_TENSOR_ENOMEM    = -2,   ///< arena exhausted / mapping failed
    WEFT_TENSOR_EAGAIN    = -3,   ///< try_claim/try_acquire would have to wait
    WEFT_TENSOR_ETIMEOUT  = -4,   ///< bounded wait exceeded the caller deadline
    WEFT_TENSOR_ERANGE    = -5,   ///< index/shape out of bounds, stride escapes
    WEFT_TENSOR_EOVERFLOW = -6,   ///< offset/length math exceeded 2^64-1
    WEFT_TENSOR_EMISALIGN = -7,   ///< alignment law violated (Law 4)
    WEFT_TENSOR_ERESHAPE  = -8,   ///< zero-copy reshape impossible (§ view)
    WEFT_TENSOR_EDIM      = -9,   ///< ndim/axis/rank out of 0..MAX_DIMS
    WEFT_TENSOR_EDTYPE    = -10,  ///< unknown weft_dtype_t value
    WEFT_TENSOR_EMAGIC    = -11,  ///< attach target is not a Weft tensor ring
    WEFT_TENSOR_EGEOMETRY = -12,  ///< header geometry self-inconsistent
} weft_tensor_status_t;

/// Stable name ("WEFT_TENSOR_EMISALIGN"); "WEFT_TENSOR_E<code>" otherwise.
const char* weft_tensor_status_name(int st);

/// Explicit alignment classes enforceable at validate/commit time (Law 4).
/// Values 0/16/32/64/128 only — validate() rejects anything else.
#define WEFT_TENSOR_ALIGN_NONE 0u
#define WEFT_TENSOR_ALIGN_16   16u
#define WEFT_TENSOR_ALIGN_32   32u
#define WEFT_TENSOR_ALIGN_64   64u
#define WEFT_TENSOR_ALIGN_128  128u

// ===========================================================================
// §3 The unified strided tensor descriptor (FROZEN ABI — do not edit)
// ===========================================================================

/// One tensor's geometry + backing memory, decodable with one pointer add.
///
///   tensor_id             producer-chosen identity (frame id, stream id);
///                         the fabric never interprets it.
///   dtype / ndim          element type and rank (ndim in 1..8).
///   shape[8]              element counts per axis (0 beyond ndim — the
///                         zero-fill is canonical and validated).
///   strides[8]            BYTE deltas per axis step (signed; negative
///                         strides describe flipped windows). Strides
///                         beyond ndim are canonical zero.
///   byte_offset           byte displacement from the payload base to
///                         element (0,...,0) — where the view window starts.
///   byte_length           the caller-declared byte span the view may
///                         touch: must cover max walking extent + one
///                         element (validate() proves it — this is the
///                         out-of-bounds-stride wall).
///   physical_or_shm_addr  payload base address. Same-process or
///                         unified-memory (Apple UMA / dma-buf mmap)
///                         consumers may use it directly; cross-process
///                         consumers MUST pair the view with the payload
///                         pointer their own mapping produced (ring
///                         acquire returns it) — the field is advisory
///                         there (the writer's address space).
///
/// sizeof == 176, natural 8-byte alignment, little-endian fields on the
/// wire (WTR1 rings commit this struct verbatim into slot headers).
typedef struct {
    uint64_t     tensor_id;
    weft_dtype_t dtype;
    uint8_t      ndim;
    uint8_t      _reserved[5];
    uint64_t     shape[WEFT_TENSOR_MAX_DIMS];
    int64_t      strides[WEFT_TENSOR_MAX_DIMS];
    uint64_t     byte_offset;
    uint64_t     byte_length;
    uintptr_t    physical_or_shm_addr;
} weft_tensor_view_t;

// ABI freeze pins (compile-time, every TU including this header).
_Static_assert(sizeof(weft_dtype_t) == 4, "weft_dtype_t must be a 4-byte enum");
_Static_assert(sizeof(weft_tensor_view_t) == 176,
               "weft_tensor_view_t ABI drift: expected 176 bytes (RFC-0021 §3)");
_Static_assert(offsetof(weft_tensor_view_t, tensor_id) == 0, "ABI: tensor_id");
_Static_assert(offsetof(weft_tensor_view_t, dtype) == 8, "ABI: dtype");
_Static_assert(offsetof(weft_tensor_view_t, ndim) == 12, "ABI: ndim");
_Static_assert(offsetof(weft_tensor_view_t, shape) == 24, "ABI: shape");
_Static_assert(offsetof(weft_tensor_view_t, strides) == 88, "ABI: strides");
_Static_assert(offsetof(weft_tensor_view_t, byte_offset) == 152, "ABI: byte_offset");
_Static_assert(offsetof(weft_tensor_view_t, byte_length) == 160, "ABI: byte_length");
_Static_assert(offsetof(weft_tensor_view_t, physical_or_shm_addr) == 168,
               "ABI: physical_or_shm_addr");

// ---------------------------------------------------------------------------
// §3.1 View construction
// ---------------------------------------------------------------------------

/// Initialize a CONTIGUOUS row-major view over `shape` (strides computed
/// as products of trailing shape * dtype size — the NCHW/NHWC default).
/// byte_length = nelements * dtype size, overflow-checked. `base` is
/// stored into physical_or_shm_addr; byte_offset is the caller's window
/// displacement (typically 0). Fills shape/strides beyond ndim with zero.
/// Returns EDTYPE / EDIM / ERANGE / EOVERFLOW — a returned view is
/// untouched on failure.
int weft_tensor_view_init(weft_tensor_view_t* v, uint64_t tensor_id,
                          weft_dtype_t dtype, uint8_t ndim,
                          const uint64_t* shape, uintptr_t base,
                          uint64_t byte_offset);

/// Initialize an EXPLICITLY strided view (strides in BYTES, signed).
/// byte_length is caller-declared (not derived) — the span the view may
/// legitimately touch; validate() proves it covers the walking extent.
/// This is the constructor for sub-windows, padded rows, flipped axes,
/// and foreign-layout reinterpretations.
int weft_tensor_view_init_strided(weft_tensor_view_t* v, uint64_t tensor_id,
                                  weft_dtype_t dtype, uint8_t ndim,
                                  const uint64_t* shape, const int64_t* strides,
                                  uintptr_t base, uint64_t byte_offset,
                                  uint64_t byte_length);

/// Deep validation — the static-analysis wall (returns the FIRST violation):
///   EDTYPE   dtype not in §1
///   EDIM     ndim not in 1..8, or shape/stride tail not canonical-zero
///   ERANGE   shape[k] == 0 for k < ndim, or byte_length does not cover
///            max walking extent + one element (THE out-of-bounds-stride
///            wall: |stride[k]| * (shape[k]-1) is bounded for every axis)
///   EOVERFLOW walking extent arithmetic exceeds 2^64-1
///   EMISALIGN (base + byte_offset) violates dtype-natural alignment, or
///            the requested align_req (16/32/64/128) — Law 4, explicit
///   EINVAL   align_req not in {0,16,32,64,128}
/// validate() never dereferences physical_or_shm_addr.
int weft_tensor_view_validate(const weft_tensor_view_t* v, uint32_t align_req);

// ---------------------------------------------------------------------------
// §3.2 Address math (the 0 ns decode)
// ---------------------------------------------------------------------------

/// Offset (bytes, from the payload base, byte_offset INCLUDED) of element
/// `idx`. Checks idx[k] < shape[k] (ERANGE) and accumulator overflow
/// (EOVERFLOW); also EDTYPE/EDIM/ERANGE from a shallow geometry check.
int weft_tensor_view_element_offset(const weft_tensor_view_t* v,
                                    const uint64_t* idx, uint64_t* out_offset);

/// Typed element address using the view's OWN base (same address space /
/// unified memory). NULL on any validation failure — pair with
/// element_offset when a code is needed.
void* weft_tensor_view_element_addr(const weft_tensor_view_t* v,
                                    const uint64_t* idx);

/// Typed element address against an EXPLICIT payload base — the
/// cross-process discipline (base = the payload pointer YOUR mapping of
/// the ring/arena/dma-buf produced). NULL on validation failure.
void* weft_tensor_view_element_addr_at(const weft_tensor_view_t* v,
                                       const uint64_t* idx,
                                       const void* payload_base);

// ---------------------------------------------------------------------------
// §3.3 Zero-allocation view algebra (dst is a caller stack view)
// ---------------------------------------------------------------------------

/// Narrow axis `axis` to [start, start+count): new byte_offset =
/// byte_offset + start*strides[axis], shape[axis] = count, strides and
/// byte_length recomputed for the narrowed window. ERANGE on window
/// escape, EDIM on axis.
int weft_tensor_view_slice(weft_tensor_view_t* dst,
                           const weft_tensor_view_t* src,
                           uint8_t axis, uint64_t start, uint64_t count);

/// Per-axis sub-window (crop/ROI): offset/count arrays indexed by axis,
/// applied simultaneously. byte_offset += sum_k offset[k]*strides[k].
/// ERANGE on any axis escape.
int weft_tensor_view_subwindow(weft_tensor_view_t* dst,
                               const weft_tensor_view_t* src,
                               const uint64_t* offset, const uint64_t* count);

/// Axis permutation (NCHW <-> NHWC, HWC->CHW...): perm must be a
/// bijection of 0..ndim-1 (EINVAL otherwise); shape/strides permute,
/// byte_offset/byte_length pass through UNCHANGED — the element set is
/// identical, only the index order changes.
int weft_tensor_view_permute(weft_tensor_view_t* dst,
                             const weft_tensor_view_t* src,
                             const uint8_t* perm);

/// Zero-copy reshape: requires src to be C-contiguous (row-major byte
/// strides; size-1 axes unconstrained) AND element-count-preserving;
/// otherwise ERESHAPE — a materializing copy is Engineer 3's UI-plane
/// business, never silently done here. No -1 inference wildcard: shapes
/// are explicit in the fabric (Law 4 honesty).
int weft_tensor_view_reshape(weft_tensor_view_t* dst,
                             const weft_tensor_view_t* src,
                             uint8_t new_ndim, const uint64_t* new_shape);

/// 1 iff the view is C-contiguous (stride[k] == trailing shape product *
/// dtype size, size-1 axes unconstrained, byte_offset unconstrained);
/// 0 otherwise (invalid views are non-contiguous).
int weft_tensor_view_is_contiguous(const weft_tensor_view_t* v);

/// Element count = product of shape (UINT64_MAX on overflow — documented
/// saturation; callers gate with validate() when it matters).
uint64_t weft_tensor_view_nelements(const weft_tensor_view_t* v);

// ===========================================================================
// §4 Paged tensor arenas (Law 1: the only allocation surface)
// ===========================================================================

#define WEFT_TENSOR_ARENA_F_MLOCK    0x1u  ///< mlock the mapping (refusal is REPORTED, not fatal)
#define WEFT_TENSOR_ARENA_F_HUGEPAGE 0x2u  ///< MADV_HUGEPAGE hint (Linux; advisory, reported)
#define WEFT_TENSOR_ARENA_F_PREFAULT 0x4u  ///< touch every page at create (kill minor faults)
#define WEFT_TENSOR_ARENA_F_SHARED   0x8u  ///< MAP_SHARED (fork-inheritable / cross-process)

/// A pre-reserved, page-aligned region with an O(1) bump cursor. The bump
/// is SINGLE-THREAD by design (the sensor/inference thread owns its
/// arena — same discipline as the kernel's writer-private fields);
/// cross-thread sharing requires caller-owned handoff. mlock/hugepage
/// outcomes are reported honestly in the struct (applied vs refused).
typedef struct {
    uint8_t* base;        ///< page-aligned region start
    uint64_t capacity;    ///< usable bytes (page-rounded)
    uint64_t cursor;      ///< bump offset (next allocation starts here)
    uint64_t highwater;   ///< max cursor ever reached (diagnostics)
    uint64_t alloc_count; ///< sub-allocations served since create
    uint32_t flags;       ///< creator flags (report only)
    int      locked;      ///< 1 = mlock applied, 0 = refused/absent (honest)
    int      hugepage_hint; ///< 1 = MADV_HUGEPAGE accepted, 0 = refused/absent
    int      creator;     ///< 1 = we own the mapping (destroy unmaps)
} weft_tensor_arena_t;

/// Rewind point (O(1) frame recycling).
typedef struct {
    uint64_t cursor;
    uint64_t alloc_count;
} weft_tensor_arena_mark_t;

/// Reserve `capacity` bytes (rounded UP to whole pages), page-aligned,
/// private-anon by default (MAP_SHARED with _F_SHARED). F_PREFAULT writes
/// every page once. Returns EINVAL/ENOMEM; the arena is zeroed on failure.
int weft_tensor_arena_create(weft_tensor_arena_t* a, uint64_t capacity,
                             uint32_t flags);

/// Adopt a CALLER-PROVIDED region (dma-buf mmap, Vulkan HOST_VISIBLE,
/// Apple UMA buffer, an existing SHM mapping — the unified-memory seam).
/// The region is only registered, never freed; `flags` report the
/// caller's knowledge (mlock state is probed, not imposed). Honesty
/// wall: `memory` must be 16B-aligned minimum and capacity nonzero
/// (< 2^63); weaker geometry is rejected EINVAL, never silently rounded
/// (Law 4).
int weft_tensor_arena_attach(weft_tensor_arena_t* a, void* memory,
                             uint64_t capacity, uint32_t flags);

/// O(1) sub-allocation: aligns the cursor up to `align` (power of two,
/// 1..4096), serves `size` bytes, returns the pointer (and the OFFSET
/// from base when offset_out != NULL). NULL + ENOMEM/EMISALIGN/EINVAL
/// via status_out on refusal — exhaustion is reported with the highwater
/// updated, never rounded down. Zero syscalls, zero heap.
void* weft_tensor_arena_alloc(weft_tensor_arena_t* a, uint64_t size,
                              uint32_t align, uint64_t* offset_out,
                              int* status_out);

/// Capture a rewind point.
int weft_tensor_arena_mark(const weft_tensor_arena_t* a,
                           weft_tensor_arena_mark_t* m);

/// Rewind to a mark (O(1); lifetime discipline: the caller guarantees no
/// view into memory above the mark is still being read — same contract
/// as ring slot release). EINVAL on a foreign/regressive mark.
int weft_tensor_arena_rewind(weft_tensor_arena_t* a,
                             const weft_tensor_arena_mark_t* m);

/// Reset to empty (mark at origin). O(1).
void weft_tensor_arena_reset(weft_tensor_arena_t* a);

/// Unmap (creator) / forget (attacher). Idempotent.
void weft_tensor_arena_destroy(weft_tensor_arena_t* a);

/// Snapshot: used bytes, highwater, allocation count.
void weft_tensor_arena_stats(const weft_tensor_arena_t* a,
                             uint64_t* used, uint64_t* highwater,
                             uint64_t* allocs);

// ===========================================================================
// §5 Lock-free circular DMA tensor ring
// ===========================================================================

#define WEFT_TENSOR_RING_MAGIC       0x31525457u  ///< "WTR1" little-endian
#define WEFT_TENSOR_RING_VERSION     1u
#define WEFT_TENSOR_RING_MODE_MPSC   1u           ///< multi-producer, single-consumer
#define WEFT_TENSOR_RING_MODE_SPMC   2u           ///< single-producer, multi-consumer
#define WEFT_TENSOR_RING_MODE_MPMC   3u           ///< both sides multi

#define WEFT_TENSOR_RING_F_MLOCK     0x1u   ///< mlock the mapping (reported)
#define WEFT_TENSOR_RING_F_HUGEPAGE  0x2u   ///< MADV_HUGEPAGE hint (reported)
#define WEFT_TENSOR_RING_F_PREFAULT  0x4u   ///< touch every page at create

#define WEFT_TENSOR_RING_CTRL_BYTES   128u  ///< control header (two cachelines)
#define WEFT_TENSOR_SLOT_HEADER_BYTES 256u  ///< per-slot header (4 cachelines)
#define WEFT_TENSOR_RING_MAX_SLOTS    (1u << 20)  ///< depth ceiling (power of 2)

/// Wait-ladder rung sizes (Law 2: FIXED, deadline-checked between rungs).
#define WEFT_TENSOR_WAIT_PAUSE_SPINS 64u
#define WEFT_TENSOR_WAIT_SLEEP_NS    50000ull  ///< 50 us per sleep rung

/// One ring slot: the sequence word gates the claim/commit/acquire/release
/// cycle; the view + payload_used are published by commit. The header is
/// exactly 256 bytes so payloads always start on a cacheline boundary
/// (128B-aligned too whenever payload_bytes is a 128 multiple).
typedef struct {
    _Atomic uint64_t   seq;          ///< Vyukov sequence: see §5 of RFC-0021
    weft_tensor_view_t view;         ///< geometry published at commit
    uint32_t           payload_used; ///< bytes the producer committed
    uint32_t           flags;        ///< reserved (zero on wire)
    uint8_t            _pad[64];     ///< header padding to 256
} weft_tensor_ring_slot_t;

_Static_assert(sizeof(weft_tensor_ring_slot_t) == WEFT_TENSOR_SLOT_HEADER_BYTES,
               "slot header ABI drift: expected 256 bytes (RFC-0021 §5)");
_Static_assert(offsetof(weft_tensor_ring_slot_t, seq) == 0, "ABI: slot seq");
_Static_assert(offsetof(weft_tensor_ring_slot_t, view) == 8, "ABI: slot view");
_Static_assert(offsetof(weft_tensor_ring_slot_t, payload_used) == 184, "ABI: payload_used");

/// Local handle onto a mapped ring. `base` is the mapping start in THIS
/// process (every pointer the API returns derives from it — the ring is
/// fully relocatable across address spaces).
typedef struct {
    uint8_t* base;          ///< mapping start (control header lives here)
    uint32_t slot_count;    ///< depth (power of two, <= 2^20)
    uint32_t payload_bytes; ///< per-slot payload capacity (64B multiple)
    uint32_t slot_stride;   ///< 256 + payload_bytes
    uint32_t mode;          ///< MPSC / SPMC / MPMC
    uint32_t flags;         ///< creator flags (advisory report)
    uint64_t ring_bytes;    ///< CTRL_BYTES + slot_count * slot_stride
    int      creator;       ///< 1 = we own the mapping (destroy unmaps)
    int      locked;        ///< mlock outcome (honest report)
    int      hugepage_hint; ///< MADV_HUGEPAGE outcome (honest report)
} weft_tensor_ring_t;

/// Mapping size needed for a ring (page granularity is the mmap caller's
/// business; this is the EXACT byte contract attach verifies against).
/// status_out receives EINVAL/ERANGE on bad geometry (or pass NULL).
uint64_t weft_tensor_ring_required_bytes(uint32_t slot_count,
                                         uint32_t payload_bytes,
                                         int* status_out);

/// Create a ring: MAP_SHARED|MAP_ANONYMOUS, page-aligned, zeroed, every
/// slot's seq initialized to its index (the fresh-ring invariant), head =
/// tail = 0. MAP_SHARED means fork() children and shm-handed mappings
/// share the SAME ring — the DMA posture (PROT_READ|PROT_WRITE across
/// processes). F_PREFAULT touches every page. Returns EINVAL/ENOMEM.
int weft_tensor_ring_create(weft_tensor_ring_t* r, uint32_t slot_count,
                            uint32_t payload_bytes, uint16_t mode,
                            uint32_t flags);

/// Adopt an EXISTING mapping (another process's ring, an shm object, a
/// dma-buf mmap): validates magic, version, mode, geometry
/// self-consistency, reserved-zero, and the EXACT byte count — a
/// mismatched object is REFUSED (EMAGIC/EGEOMETRY/EINVAL), never guessed
/// at. The mode argument must match the header's mode (a mismatched
/// discipline is EINVAL — the cursor ownership contract differs).
int weft_tensor_ring_attach(weft_tensor_ring_t* r, void* mapping,
                            uint64_t mapping_bytes, uint16_t mode);

/// Unmap (creator) / forget (attacher). Idempotent.
void weft_tensor_ring_destroy(weft_tensor_ring_t* r);

// ---------------------------------------------------------------------------
// §5.1 Producer side: claim -> write payload -> commit
// ---------------------------------------------------------------------------

/// Non-blocking claim of the next producer ticket. EAGAIN when the ring
/// is full or the ticket race was lost (stat_full_hits counts refusals).
/// On success the CALLING THREAD owns the slot exclusively until commit:
/// write the payload at *payload_out (capacity *cap_out), then commit.
int weft_tensor_ring_try_claim(weft_tensor_ring_t* r, uint64_t* ticket_out,
                               uint8_t** payload_out, uint32_t* cap_out);

/// Bounded claim: retries try_claim through the wait ladder until
/// `timeout_ns` elapses (0 = single attempt). ETIMEOUT on deadline. No
/// unbounded variant exists (Law 2).
int weft_tensor_ring_claim(weft_tensor_ring_t* r, uint64_t timeout_ns,
                           uint64_t* ticket_out, uint8_t** payload_out,
                           uint32_t* cap_out);

/// Publish slot `ticket`: shallow-validates + Law-4-aligns the view,
/// proves payload_used covers the view's walking extent, copies the view
/// into the slot header (physical_or_shm_addr := the slot's payload
/// address, so same-process consumers read it directly), then
/// release-stores the sequence word. The producer's payload writes
/// happen-before the release; the acquiring consumer's reads happen-after
/// the matching acquire — a committed tensor is never observed torn.
/// Returns the view-validation code on bad views (EAGAIN on a ticket not
/// owned/already committed — protocol misuse detection).
int weft_tensor_ring_commit(weft_tensor_ring_t* r, uint64_t ticket,
                            const weft_tensor_view_t* view,
                            uint32_t payload_used);

// ---------------------------------------------------------------------------
// §5.2 Consumer side: acquire -> read (view + payload) -> release
// ---------------------------------------------------------------------------

/// Non-blocking acquire of the next committed tensor: view_out points
/// INTO the slot header (stable until release), payload_out is derived
/// from THIS process's mapping (the cross-process discipline), used_out
/// is the producer's payload_used. EAGAIN when nothing new is committed.
int weft_tensor_ring_try_acquire(weft_tensor_ring_t* r, uint64_t* ticket_out,
                                 const weft_tensor_view_t** view_out,
                                 const uint8_t** payload_out,
                                 uint32_t* used_out);

/// Bounded acquire (see claim). MPSC: the single consumer owns `tail`
/// (misuse — two acquirers on an MPSC ring — is detected as ticket
/// wait/timeout, documented, not defended); SPMC/MMPC: consumers race a
/// CAS on tail — every ticket is acquired by EXACTLY one consumer.
int weft_tensor_ring_acquire(weft_tensor_ring_t* r, uint64_t timeout_ns,
                             uint64_t* ticket_out,
                             const weft_tensor_view_t** view_out,
                             const uint8_t** payload_out, uint32_t* used_out);

/// Retire ticket: release-stores the slot sequence (the consumer's reads
/// happen-before it; the next producer for that slot synchronizes on it).
/// EAGAIN if the ticket is not currently acquired (misuse detection).
int weft_tensor_ring_release(weft_tensor_ring_t* r, uint64_t ticket);

// ---------------------------------------------------------------------------
// §5.3 Introspection
// ---------------------------------------------------------------------------

/// Counters (cross-process shared, monotonic): committed, acquired,
/// released, try-claim refusals. Relaxed atomics — diagnostics, never
/// correctness authority (the slot sequence words are the only authority).
void weft_tensor_ring_stats(const weft_tensor_ring_t* r,
                            uint64_t* committed, uint64_t* acquired,
                            uint64_t* released, uint64_t* full_hits);

/// In-flight window (committed - acquired); saturates at UINT64_MAX.
uint64_t weft_tensor_ring_in_flight(const weft_tensor_ring_t* r);

/// Geometry accessors (from the local handle).
uint32_t weft_tensor_ring_slot_count(const weft_tensor_ring_t* r);
uint32_t weft_tensor_ring_payload_bytes(const weft_tensor_ring_t* r);
uint32_t weft_tensor_ring_slot_stride(const weft_tensor_ring_t* r);
uint16_t weft_tensor_ring_mode(const weft_tensor_ring_t* r);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // WEFT_TENSOR_H
