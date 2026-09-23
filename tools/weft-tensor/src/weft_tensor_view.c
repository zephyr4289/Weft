// weft_tensor_view.c — RFC-0017 §2 implementation: the refusal ladder,
// view construction, and the geometry math. Pure functions throughout
// (Law 1: nothing allocates; the view is a 128-byte value type).

#include "weft/weft_tensor_view.h"

#include <string.h>

#ifdef WEFT_TENSOR_HAVE_WTS1
#include "weft_tensor.h"  // the parsed-header type for of_wts1()
#endif

// ---------------------------------------------------------------------------
// Error names (evidence lines; every refusal is named, never guessed)
// ---------------------------------------------------------------------------

const char* weft_tv_err_name(weft_tv_err_t e) {
    switch (e) {
    case WEFT_TV_OK:           return "ok";
    case WEFT_TV_ERR_NULL:     return "null-arg";
    case WEFT_TV_ERR_MAGIC:    return "bad-magic";
    case WEFT_TV_ERR_ABI:      return "abi-mismatch";
    case WEFT_TV_ERR_DTYPE:    return "dtype-out-of-dialect";
    case WEFT_TV_ERR_RANK:     return "rank-out-of-bounds";
    case WEFT_TV_ERR_DIMS:     return "bad-dims";
    case WEFT_TV_ERR_COUNT:    return "elem-count-mismatch";
    case WEFT_TV_ERR_STRIDES:  return "bad-strides";
    case WEFT_TV_ERR_SPAN:     return "geometry-exceeds-byte-len";
    case WEFT_TV_ERR_CAPACITY: return "geometry-exceeds-capacity";
    case WEFT_TV_ERR_PTR:      return "null-data";
    case WEFT_TV_ERR_FLAGS:    return "unknown-flags";
    case WEFT_TV_ERR_ENDIAN:   return "big-endian-payload";
    default:                   return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Geometry math
// ---------------------------------------------------------------------------

size_t weft_tensor_view_elem_size(weft_tensor_dtype_t t) {
    switch (t) {
    case WEFT_TENSOR_U8:
    case WEFT_TENSOR_I8:  return 1;
    case WEFT_TENSOR_U16:
    case WEFT_TENSOR_I16:
    case WEFT_TENSOR_F16: return 2;
    case WEFT_TENSOR_U32:
    case WEFT_TENSOR_I32:
    case WEFT_TENSOR_F32: return 4;
    case WEFT_TENSOR_U64:
    case WEFT_TENSOR_I64:
    case WEFT_TENSOR_F64: return 8;
    default:              return 0;
    }
}

uint32_t weft_tensor_view_canonical_stride(const uint32_t* dims,
                                           uint8_t rank, uint32_t axis) {
    if (!dims || axis >= rank || rank > WEFT_TENSOR_VIEW_MAX_RANK) return 0;
    uint64_t s = 1;
    for (uint32_t k = axis + 1; k < rank; k++) {
        s *= (uint64_t)dims[k];
        if (s > 0xFFFFFFFFull) return 0;  // overflow: refused, not wrapped
    }
    return (uint32_t)s;
}

uint64_t weft_tensor_view_byte_stride(const weft_tensor_view_t* v,
                                      uint32_t axis) {
    if (!v || axis >= v->rank) return 0;
    return (uint64_t)v->strides[axis] *
           (uint64_t)weft_tensor_view_elem_size((weft_tensor_dtype_t)v->dtype);
}

uint64_t weft_tensor_view_offset(const weft_tensor_view_t* v,
                                 uint32_t i0, uint32_t i1,
                                 uint32_t i2, uint32_t i3) {
    if (!v) return 0;
    const uint32_t idx[4] = {i0, i1, i2, i3};
    uint64_t elems = 0;
    for (uint32_t k = 0; k < v->rank; k++) {
        elems += (uint64_t)idx[k] * (uint64_t)v->strides[k];
    }
    return elems * (uint64_t)weft_tensor_view_elem_size(
                            (weft_tensor_dtype_t)v->dtype);
}

// ---------------------------------------------------------------------------
// The refusal ladder (Law 4)
// ---------------------------------------------------------------------------

weft_tv_err_t weft_tensor_view_validate(const weft_tensor_view_t* v,
                                        uint64_t byte_cap) {
    if (!v) return WEFT_TV_ERR_NULL;
    if (v->magic != WEFT_TENSOR_VIEW_MAGIC) return WEFT_TV_ERR_MAGIC;
    if (v->abi_version != WEFT_TENSOR_VIEW_ABI_VERSION) return WEFT_TV_ERR_ABI;
    if (v->reserved0 != 0) return WEFT_TV_ERR_FLAGS;  // discipline: zero

    // Unknown flag bits refuse (the version discipline every Weft wire
    // format follows — v1 knows 3 bits).
    if (v->flags & ~(uint8_t)(WEFT_TENSOR_VIEW_F_BIG_ENDIAN |
                               WEFT_TENSOR_VIEW_F_PINNED |
                               WEFT_TENSOR_VIEW_F_PACKED)) {
        return WEFT_TV_ERR_FLAGS;
    }

    if (v->rank == 0 || v->rank > WEFT_TENSOR_VIEW_MAX_RANK) {
        return WEFT_TV_ERR_RANK;
    }
    if (weft_tensor_view_elem_size((weft_tensor_dtype_t)v->dtype) == 0) {
        return WEFT_TV_ERR_DTYPE;
    }

    uint64_t prod = 1;
    for (uint32_t k = 0; k < v->rank; k++) {
        if (v->dims[k] < 1u) return WEFT_TV_ERR_DIMS;
        prod *= (uint64_t)v->dims[k];
        if (prod > 0xFFFFFFFFull) return WEFT_TV_ERR_COUNT;
    }
    for (uint32_t k = v->rank; k < 4; k++) {
        if (v->dims[k] != 0) return WEFT_TV_ERR_DIMS;  // tail must be zero
    }
    if (prod != (uint64_t)v->elem_count) return WEFT_TV_ERR_COUNT;

    // Strides: among REAL axes (dim > 1) the row-major strides must
    // strictly DECREASE toward the inner axis; size-1 axes are
    // unconstrained (their stride is never multiplied by a nonzero
    // index — canonical math yields stride TIES there, e.g. dims
    // {1024,1} -> strides {1,1}). When PACKED is claimed the strides
    // must additionally be exactly canonical.
    {
        uint64_t prev_real = 0;  // 0 = "innermost real axis not seen yet"
        for (int32_t k = (int32_t)v->rank - 1; k >= 0; k--) {
            if (v->dims[k] < 2u) continue;  // size-1 axis: unconstrained
            if (prev_real != 0 && v->strides[k] <= prev_real) {
                return WEFT_TV_ERR_STRIDES;  // not strictly decreasing
            }
            if (v->strides[k] == 0) {
                return WEFT_TV_ERR_STRIDES;  // a real axis needs a stride
            }
            prev_real = v->strides[k];
        }
    }
    for (uint32_t k = 0; k < v->rank; k++) {
        if (v->flags & WEFT_TENSOR_VIEW_F_PACKED) {
            if (v->strides[k] !=
                weft_tensor_view_canonical_stride(v->dims, v->rank, k)) {
                return WEFT_TV_ERR_STRIDES;
            }
        }
    }

    if (!v->data) return WEFT_TV_ERR_PTR;

    // The span: the furthest byte any (dims x strides) indexing can reach.
    // Computed as sum over axes of (dim[k]-1)*stride[k], in elements,
    // then scaled + elem_size. Multiplications in u64; a geometry that
    // overflows u64 refuses (SPAN) rather than wrapping.
    uint64_t elems = 0;
    for (uint32_t k = 0; k < v->rank; k++) {
        elems += (uint64_t)(v->dims[k] - 1u) * (uint64_t)v->strides[k];
    }
    uint64_t es = (uint64_t)weft_tensor_view_elem_size(
        (weft_tensor_dtype_t)v->dtype);
    uint64_t span_bytes;
    if (elems > (0xFFFFFFFFFFFFFFFFull - es) / es) return WEFT_TV_ERR_SPAN;
    span_bytes = elems * es + es;
    if (v->byte_len == 0 || span_bytes > v->byte_len) return WEFT_TV_ERR_SPAN;
    if (byte_cap != 0 && span_bytes > byte_cap) return WEFT_TV_ERR_CAPACITY;

    return WEFT_TV_OK;
}

weft_tv_err_t weft_tensor_view_gpu_ready(const weft_tensor_view_t* v) {
    weft_tv_err_t e = weft_tensor_view_validate(v, 0);
    if (e != WEFT_TV_OK) return e;
    // Endianness: a BE payload zero-copy-bound into an LE device is a
    // silent corruption — refused; the labeled conversion road is the
    // fallback (Law 2/Law 4).
    if (v->flags & WEFT_TENSOR_VIEW_F_BIG_ENDIAN) return WEFT_TV_ERR_ENDIAN;
    // Law 2: 16-byte float-vector alignment. u8 tensors ride the raw-byte
    // road and only need the pointer; F32/F16 must be vector-aligned.
    uint64_t es = (uint64_t)weft_tensor_view_elem_size(
        (weft_tensor_dtype_t)v->dtype);
    if (es >= 2 && (((uintptr_t)v->data & 15u) != 0)) {
        return WEFT_TV_ERR_PTR;  // misaligned for a GPU float-vector bind
    }
    return WEFT_TV_OK;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static void view_zero(weft_tensor_view_t* v) {
    memset(v, 0, sizeof(*v));
    v->magic = WEFT_TENSOR_VIEW_MAGIC;
    v->abi_version = WEFT_TENSOR_VIEW_ABI_VERSION;
}

weft_tv_err_t weft_tensor_view_init(weft_tensor_view_t* v,
                                    weft_tensor_dtype_t dtype,
                                    const uint32_t* dims, uint8_t rank,
                                    void* data, uint64_t schema_id) {
    if (!v || !dims || !data) return WEFT_TV_ERR_NULL;
    view_zero(v);
    v->dtype = (uint8_t)dtype;
    v->rank = rank;
    v->data = data;
    v->schema_id = schema_id;
    for (uint32_t k = 0; k < rank && k < 4; k++) v->dims[k] = dims[k];
    v->flags = WEFT_TENSOR_VIEW_F_PACKED;
    for (uint32_t k = 0; k < rank && k < 4; k++) {
        v->strides[k] = weft_tensor_view_canonical_stride(v->dims, rank, k);
    }
    uint64_t prod = 1;
    for (uint32_t k = 0; k < rank && k < 4; k++) {
        prod *= (uint64_t)(dims[k] ? dims[k] : 1u);
    }
    v->elem_count = (uint32_t)prod;
    uint64_t es = (uint64_t)weft_tensor_view_elem_size(dtype);
    if (es == 0) return WEFT_TV_ERR_DTYPE;  // init already stamped fields;
                                            // the ladder names it for callers
    v->byte_len = prod * es;
    return weft_tensor_view_validate(v, 0);
}

weft_tv_err_t weft_tensor_view_init_strided(weft_tensor_view_t* v,
                                            weft_tensor_dtype_t dtype,
                                            const uint32_t* dims,
                                            const uint32_t* strides,
                                            uint8_t rank, void* data,
                                            uint64_t schema_id) {
    if (!v || !dims || !strides || !data) return WEFT_TV_ERR_NULL;
    weft_tv_err_t e = weft_tensor_view_init(v, dtype, dims, rank, data,
                                            schema_id);
    if (e != WEFT_TV_OK && e != WEFT_TV_ERR_DTYPE) return e;
    for (uint32_t k = 0; k < rank && k < 4; k++) v->strides[k] = strides[k];
    // PACKED only when exactly canonical (validate() re-derives it).
    v->flags &= (uint8_t)~WEFT_TENSOR_VIEW_F_PACKED;
    int canonical = 1;
    for (uint32_t k = 0; k < rank && k < 4; k++) {
        if (v->strides[k] !=
            weft_tensor_view_canonical_stride(v->dims, rank, k)) {
            canonical = 0;
            break;
        }
    }
    if (canonical) v->flags |= WEFT_TENSOR_VIEW_F_PACKED;
    // byte_len reflects the STRIDED span (the furthest addressed byte),
    // not the packed product — a strided view's extent IS its reach.
    uint64_t es = (uint64_t)weft_tensor_view_elem_size(dtype);
    uint64_t elems = 0;
    for (uint32_t k = 0; k < rank && k < 4; k++) {
        elems += (uint64_t)(v->dims[k] - 1u) * (uint64_t)v->strides[k];
    }
    if (es != 0) {
        v->byte_len = elems * es + es;
    }
    return weft_tensor_view_validate(v, 0);
}

weft_tv_err_t weft_tensor_view_of_wts1(weft_tensor_view_t* v,
                                       const void* wts1_frame,
                                       size_t frame_len) {
    if (!v || !wts1_frame) return WEFT_TV_ERR_NULL;
#ifdef WEFT_TENSOR_HAVE_WTS1
    weft_tensor_hdr_t hdr;
    const void* payload = NULL;
    if (weft_tensor_frame_parse(wts1_frame, frame_len, &hdr, &payload) != 0) {
        return WEFT_TV_ERR_MAGIC;  // the WTS1 ladder already refused; the
                                   // view refuses with it (never a guess)
    }
    view_zero(v);
    v->dtype = hdr.dtype;
    v->rank = hdr.rank;
    v->data = (void*)payload;  // read-only consumption of the slot payload
    for (uint32_t k = 0; k < 4; k++) v->dims[k] = hdr.dims[k];
    v->flags = WEFT_TENSOR_VIEW_F_PACKED | WEFT_TENSOR_VIEW_F_PINNED;
    for (uint32_t k = 0; k < hdr.rank; k++) {
        v->strides[k] =
            weft_tensor_view_canonical_stride(v->dims, hdr.rank, k);
    }
    v->elem_count = hdr.elem_count;
    uint64_t es = (uint64_t)weft_tensor_view_elem_size(
        (weft_tensor_dtype_t)hdr.dtype);
    v->byte_len = (uint64_t)hdr.elem_count * es;
    return weft_tensor_view_validate(v, 0);
#else
    (void)frame_len;
    return WEFT_TV_ERR_ABI;  // WTS1 module absent — the __has_include seam
#endif
}
