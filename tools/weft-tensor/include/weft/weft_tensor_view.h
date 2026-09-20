// weft_tensor_view.h — RFC-0017 §2: the strided tensor view ABI seam.
//
// WHY EXISTS: the accelerator pillar (this tree) connects Weft's DMA tensor
// ring — Engineer 1's Core Lock-Free DMA Ring + Strided Geometry Engine —
// to hardware execution units (Metal/ANE, Vulkan dma-buf compute, ONNX
// Runtime, llama.cpp/ggml). Every adapter needs the SAME five questions
// answered about a tensor before it can bind memory: which dtype, which
// geometry, where do the bytes live, how are they strided, and is the
// layout acceptable for THIS accelerator's memory model. Today every
// runtime answers those questions with its own private descriptor, so
// adapters hand-copy and re-frame — the exact staging-copy tax this
// pillar exists to delete. This header is the ONE contract all four
// backends code against:
//
//   weft_tensor_view_t (128 B, cache-line pair, ABI-versioned)
//        │
//        ├── backends/vulkan  (wrap → VkBuffer/SSBO, std430 math)
//        ├── backends/metal   (wrap → IOSurface/CVPixelBuffer/MTLBuffer)
//        ├── backends/onnx    (wrap → OrtValue via CreateTensorWithData)
//        └── backends/ggml    (wrap → ggml tensor data pointer placement)
//
// ABI DISCIPLINE: Engineer 1's ring engine is under parallel development
// on its own branch. This header freezes the INTEROP surface (v1) the
// adapters consume; when the core's weft_tensor_view lands, both sides
// carry the same WEFT_TENSOR_VIEW_ABI_VERSION and the static layout
// asserts below make any drift a COMPILE ERROR, never a silent mismatch
// (the version discipline every Weft wire format follows).
//
// DTYPE DIALECT: the numeric dtype codes are the WTS1 dialect
// (core/c/weft_tensor.h, RFC-0016 §5) — u8=0 .. f64=10, frozen public
// constants. When weft_tensor.h is visible (any post-Series-10 tree) the
// equivalence is a static assert, not a convention. F16 stays the
// weft_f16_codec dialect tree-wide.
//
// STRIDES: strides[] are in ELEMENTS (the GPU/ONNX convention — WGSL/GLSL
// index arrays by element, ONNX TensorShape Strided semantics match, and
// ggml is element-counted by construction; byte strides are derived once,
// in one place, by weft_tensor_view_byte_stride()). A PACKED view (flag
// bit set) has canonical row-major contiguous strides — adapters may use
// flat math on it. Non-packed views describe genuine strided geometry
// (a plane inside a camera frame, a channel slice inside an audio bank).
//
// LAW 1: every function here allocates NOTHING (the view is a value type;
// adapters treat it as read-only geometry).
// LAW 2: 16-byte float-vector alignment is a first-class predicate —
//        weft_tensor_view_gpu_ready() is the gate every GPU/NPU adapter
//        calls before binding; a view that fails it routes to the
//        SIMD-accelerated [FALLBACK-COPY] road (weft_accel_common.h),
//        never a silent misaligned bind.
// LAW 3: tools layer; core/c is untouched; adapters under backends/.
// LAW 4: weft_tensor_view_validate() is a refusal ladder — every anomaly
//        is a distinct negative code, nothing is guessed, and the
//        byte-capacity check makes over-run impossible by construction.

#ifndef WEFT_TENSOR_VIEW_H
#define WEFT_TENSOR_VIEW_H

#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>

#include "weft_tensor_dialect.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// ABI identity
// ---------------------------------------------------------------------------

/// ABI version of the interop contract itself. Bumped ONLY on a breaking
/// change (field reorder/resize, stride-unit change); the core's
/// implementation of the view must carry the same value.
#define WEFT_TENSOR_VIEW_ABI_VERSION 1u

/// Magic "WTV1" (little-endian u32: 'W' 'T' 'V' '1').
#define WEFT_TENSOR_VIEW_MAGIC 0x31565457u

/// Maximum rank the v1 ABI carries (matches WTS1's rank bound).
#define WEFT_TENSOR_VIEW_MAX_RANK 4u

// ---------------------------------------------------------------------------
// The view (128 B — two cache lines, 64-byte aligned)
// ---------------------------------------------------------------------------

/// View flags.
enum {
    /// Payload words are big-endian (host-to-accelerator conversion is a
    /// [FALLBACK-COPY] road; a zero-copy bind of mismatched endianness is
    /// refused, never guessed — Law 2/Law 4).
    WEFT_TENSOR_VIEW_F_BIG_ENDIAN = 0x01u,
    /// Backing pages are device-pinned / dma-buf backed (advisory — the
    /// import roads probe for themselves; AXIOM T: never a correctness
    /// input).
    WEFT_TENSOR_VIEW_F_PINNED = 0x02u,
    /// Strides are canonical row-major contiguous (flat math allowed).
    WEFT_TENSOR_VIEW_F_PACKED = 0x04u,
};

/// A strided, typed window onto DMA memory. Built by the ring engine
/// (Engineer 1's core) or by weft_tensor_view_init() for foreign spans;
/// consumed read-only by every accelerator adapter.
typedef struct weft_tensor_view {
    alignas(64) uint32_t magic; ///< WEFT_TENSOR_VIEW_MAGIC (member-level
                                ///< _Alignas raises the struct's alignment
                                ///< to the cache line — standard C11)
    uint8_t  abi_version;  ///< WEFT_TENSOR_VIEW_ABI_VERSION
    uint8_t  dtype;        ///< weft_tensor_dtype_t (WTS1 dialect)
    uint8_t  rank;         ///< 1..WEFT_TENSOR_VIEW_MAX_RANK
    uint8_t  flags;        ///< WEFT_TENSOR_VIEW_F_* (unknown bits refuse)
    uint32_t elem_count;   ///< prod(dims[0..rank))
    uint32_t reserved0;    ///< 0 (discipline: reserved fields must be zero)
    uint64_t schema_id;    ///< weftc layout hash (Pillar 1) — 0 = ungoverned
    uint32_t dims[4];      ///< dims[k]=0 for k >= rank; every dim >= 1
    uint32_t strides[4];   ///< ELEMENTS; canonical row-major when PACKED
    uint64_t byte_len;     ///< data[0] .. last addressed byte (see below)
    uint64_t epoch;        ///< producer frame seq (fencing; 0 = untracked)
    void*    data;         ///< the bytes (alignment checked by predicates)
    uint8_t  reserved[48]; ///< 0 — pads the struct to 128 B
} weft_tensor_view_t;

_Static_assert(sizeof(weft_tensor_view_t) == 128,
               "weft_tensor_view: ABI v1 size (two cache lines)");
_Static_assert(alignof(weft_tensor_view_t) == 64,
               "weft_tensor_view: ABI v1 alignment (cache line)");
_Static_assert(offsetof(weft_tensor_view_t, data) == 72,
               "weft_tensor_view: ABI v1 field order frozen (data @ 72)");

// ---------------------------------------------------------------------------
// Validation (the refusal ladder — Law 4)
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_TV_OK = 0,
    WEFT_TV_ERR_NULL = -1,           ///< NULL view / NULL out-param
    WEFT_TV_ERR_MAGIC = -2,          ///< wrong magic (not a view, or garbage)
    WEFT_TV_ERR_ABI = -3,            ///< abi_version unknown (future struct)
    WEFT_TV_ERR_DTYPE = -4,          ///< dtype outside the WTS1 dialect
    WEFT_TV_ERR_RANK = -5,           ///< rank 0 or > MAX_RANK
    WEFT_TV_ERR_DIMS = -6,           ///< a dim < 1, or tail dims nonzero
    WEFT_TV_ERR_COUNT = -7,          ///< elem_count != prod(dims)
    WEFT_TV_ERR_STRIDES = -8,        ///< non-canonical stride while PACKED
    WEFT_TV_ERR_SPAN = -9,           ///< geometry addresses past byte_len
    WEFT_TV_ERR_CAPACITY = -10,      ///< geometry exceeds the caller's
                                     ///< backing capacity (byte_cap)
    WEFT_TV_ERR_PTR = -11,           ///< NULL data pointer
    WEFT_TV_ERR_FLAGS = -12,         ///< unknown flag bits set
    WEFT_TV_ERR_ENDIAN = -13,        ///< BIG_ENDIAN payload (conversion is
                                     ///< the fallback road, not a guess)
} weft_tv_err_t;

/// Full ladder. `byte_cap` is the backing extent the caller KNOWS the view
/// may touch (the ring slot's payload bytes, the dma-buf mapping length);
/// 0 skips the capacity leg (trust-the-producer mode for hot paths — the
/// geometry legs still run). Returns WEFT_TV_OK or the first refusal.
weft_tv_err_t weft_tensor_view_validate(const weft_tensor_view_t* v,
                                        uint64_t byte_cap);

/// The GPU/NPU acceptability predicate (Law 2): little-endian, F32/F16 (or
/// u8 raw-byte tensors), data pointer 16-byte aligned, and byte_len within
/// the capacity. Returns WEFT_TV_OK when a zero-copy GPU bind is legal;
/// WEFT_TV_ERR_* names the exact reason otherwise (adapters route to
/// [FALLBACK-COPY] on ANY nonzero return — never a silent misaligned bind).
weft_tv_err_t weft_tensor_view_gpu_ready(const weft_tensor_view_t* v);

/// One-line diagnostic for evidence logs.
const char* weft_tv_err_name(weft_tv_err_t e);

// ---------------------------------------------------------------------------
// Construction (value-type; allocates nothing)
// ---------------------------------------------------------------------------

/// Build a PACKED row-major view over `data` (dtype/dims/rank validated by
/// the same ladder; byte_len computed). Returns WEFT_TV_OK or the refusal.
weft_tv_err_t weft_tensor_view_init(weft_tensor_view_t* v,
                                    weft_tensor_dtype_t dtype,
                                    const uint32_t* dims, uint8_t rank,
                                    void* data, uint64_t schema_id);

/// Build a STRIDED view (strides in ELEMENTS, strides[0] ignored for
/// rank 1). Sets PACKED only when the strides are exactly canonical.
weft_tv_err_t weft_tensor_view_init_strided(weft_tensor_view_t* v,
                                            weft_tensor_dtype_t dtype,
                                            const uint32_t* dims,
                                            const uint32_t* strides,
                                            uint8_t rank, void* data,
                                            uint64_t schema_id);

/// WTS1 compose (RFC-0016 §5): view over a parsed WTS1 frame payload —
/// the ring's in-band tensor stream becomes adapter-visible geometry with
/// zero transforms. Available on trees carrying core/c/weft_tensor.h
/// (post-Series-10); returns WEFT_TV_ERR_ABI with a logged reason when
/// the WTS1 module is absent (the __has_include seam in the .c).
weft_tv_err_t weft_tensor_view_of_wts1(weft_tensor_view_t* v,
                                       const void* wts1_frame,
                                       size_t frame_len);

// ---------------------------------------------------------------------------
// Geometry math (pure; hot-path safe)
// ---------------------------------------------------------------------------

/// Element size for a dtype (the WTS1 table; 0 for invalid codes).
size_t weft_tensor_view_elem_size(weft_tensor_dtype_t t);

/// Offset of element (i0..i3) in BYTES from data (strided multiply-add).
/// Callers must have validated the view; this function trusts it.
uint64_t weft_tensor_view_offset(const weft_tensor_view_t* v,
                                 uint32_t i0, uint32_t i1,
                                 uint32_t i2, uint32_t i3);

/// Byte stride of axis k (element stride * elem_size).
uint64_t weft_tensor_view_byte_stride(const weft_tensor_view_t* v,
                                      uint32_t axis);

/// Canonical row-major element stride for axis k given dims (rank bounded).
uint32_t weft_tensor_view_canonical_stride(const uint32_t* dims,
                                           uint8_t rank, uint32_t axis);

#ifdef __cplusplus
}
#endif

#endif // WEFT_TENSOR_VIEW_H
