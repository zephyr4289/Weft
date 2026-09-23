// weft_tensor_view.c — the unified strided tensor descriptor engine
// (RFC-0021 §3). Pure arithmetic: no allocation, no syscalls, no state.
//
// The walking-extent wall (validate): for a view with axes k, the element
// addresses span
//
//     [byte_offset + L, byte_offset + U + dtype_size)
//     U    = sum over strides[k] > 0 of  strides[k] * (shape[k]-1)
//     L    = -sum over strides[k] < 0 of |strides[k]| * (shape[k]-1)
//
// validate() proves (i) byte_offset >= -L (the window never dips below the
// payload base — the standard flipped-image view sits exactly at 0), (ii)
// byte_length >= -L + U + dtype_size (the out-of-bounds-stride wall), and
// (iii) byte_offset + U + dtype_size does not wrap 2^64. All arithmetic is
// unsigned with explicit overflow walls — no signed UB anywhere.

#include "weft_tensor.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Overflow-checked primitives (__builtin_* on gcc/clang — the toolchains
// this tree ships; a manual fallback keeps -std=c11 honesty elsewhere).
// ---------------------------------------------------------------------------

static inline int wt_add_ovf(uint64_t a, uint64_t b, uint64_t* out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, out) ? 1 : 0;
#else
    if (b > UINT64_MAX - a) return 1;
    *out = a + b;
    return 0;
#endif
}

static inline int wt_mul_ovf(uint64_t a, uint64_t b, uint64_t* out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, out) ? 1 : 0;
#else
    if (a != 0 && b > UINT64_MAX / a) return 1;
    *out = a * b;
    return 0;
#endif
}

/// |stride| without int64 UB at INT64_MIN.
static inline uint64_t wt_abs_stride(int64_t s) {
    if (s >= 0) return (uint64_t)s;
    return (uint64_t)(-(s + 1)) + 1u;  // == (uint64_t)(-(s+1)) + 1 == |s|
}

// ---------------------------------------------------------------------------
// §1 dtype tables
// ---------------------------------------------------------------------------

uint32_t weft_dtype_size(weft_dtype_t dt) {
    switch (dt) {
        case WEFT_DTYPE_U8:
        case WEFT_DTYPE_I8:   return 1u;
        case WEFT_DTYPE_I16:
        case WEFT_DTYPE_F16:
        case WEFT_DTYPE_BF16: return 2u;
        case WEFT_DTYPE_F32:  return 4u;
        case WEFT_DTYPE_F64:  return 8u;
        default:              return 0u;
    }
}

uint32_t weft_dtype_align(weft_dtype_t dt) {
    // Natural alignment == element size for every dtype in the registry
    // (F16/BF16 are 2-byte aligned on all targets we ship).
    return weft_dtype_size(dt);
}

const char* weft_dtype_name(weft_dtype_t dt) {
    switch (dt) {
        case WEFT_DTYPE_U8:   return "u8";
        case WEFT_DTYPE_I8:   return "i8";
        case WEFT_DTYPE_I16:  return "i16";
        case WEFT_DTYPE_F16:  return "f16";
        case WEFT_DTYPE_BF16: return "bf16";
        case WEFT_DTYPE_F32:  return "f32";
        case WEFT_DTYPE_F64:  return "f64";
        default:              return "unknown";
    }
}

const char* weft_tensor_status_name(int st) {
    switch (st) {
        case WEFT_TENSOR_OK:        return "WEFT_TENSOR_OK";
        case WEFT_TENSOR_EINVAL:    return "WEFT_TENSOR_EINVAL";
        case WEFT_TENSOR_ENOMEM:    return "WEFT_TENSOR_ENOMEM";
        case WEFT_TENSOR_EAGAIN:    return "WEFT_TENSOR_EAGAIN";
        case WEFT_TENSOR_ETIMEOUT:  return "WEFT_TENSOR_ETIMEOUT";
        case WEFT_TENSOR_ERANGE:    return "WEFT_TENSOR_ERANGE";
        case WEFT_TENSOR_EOVERFLOW: return "WEFT_TENSOR_EOVERFLOW";
        case WEFT_TENSOR_EMISALIGN: return "WEFT_TENSOR_EMISALIGN";
        case WEFT_TENSOR_ERESHAPE:  return "WEFT_TENSOR_ERESHAPE";
        case WEFT_TENSOR_EDIM:      return "WEFT_TENSOR_EDIM";
        case WEFT_TENSOR_EDTYPE:    return "WEFT_TENSOR_EDTYPE";
        case WEFT_TENSOR_EMAGIC:    return "WEFT_TENSOR_EMAGIC";
        case WEFT_TENSOR_EGEOMETRY: return "WEFT_TENSOR_EGEOMETRY";
        default:                    return "WEFT_TENSOR_EUNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// Internal geometry helpers
// ---------------------------------------------------------------------------

/// Shallow shape/stride tail canonical-zero check (deep checks live in
/// validate). Returns EDIM on a dirty tail.
static int wt_tail_canonical(const weft_tensor_view_t* v) {
    for (uint8_t k = v->ndim; k < WEFT_TENSOR_MAX_DIMS; k++) {
        if (v->shape[k] != 0 || v->strides[k] != 0) return 0;
    }
    return 1;
}

/// Walking extent: U and |L| (see file banner). 0 on success, else
/// EOVERFLOW / EDIM / EDTYPE / ERANGE (shape zero).
static int wt_extent(const weft_tensor_view_t* v, uint64_t* u_out,
                     uint64_t* lneg_out) {
    const uint32_t dsz = weft_dtype_size(v->dtype);
    if (dsz == 0) return WEFT_TENSOR_EDTYPE;
    if (v->ndim < 1 || v->ndim > WEFT_TENSOR_MAX_DIMS) return WEFT_TENSOR_EDIM;

    uint64_t u = 0, lneg = 0;
    for (uint8_t k = 0; k < v->ndim; k++) {
        if (v->shape[k] == 0) return WEFT_TENSOR_ERANGE;
        uint64_t reach;
        if (wt_mul_ovf(wt_abs_stride(v->strides[k]), v->shape[k] - 1u, &reach))
            return WEFT_TENSOR_EOVERFLOW;
        if (v->strides[k] >= 0) {
            if (wt_add_ovf(u, reach, &u)) return WEFT_TENSOR_EOVERFLOW;
        } else {
            if (wt_add_ovf(lneg, reach, &lneg)) return WEFT_TENSOR_EOVERFLOW;
        }
    }
    *u_out = u;
    *lneg_out = lneg;
    return WEFT_TENSOR_OK;
}

/// Tight span the view's window occupies: |L| + U + dtype_size.
static int wt_span(const weft_tensor_view_t* v, uint64_t* span_out) {
    uint64_t u, lneg;
    const int st = wt_extent(v, &u, &lneg);
    if (st != WEFT_TENSOR_OK) return st;
    const uint32_t dsz = weft_dtype_size(v->dtype);
    uint64_t span = lneg;
    if (wt_add_ovf(span, u, &span)) return WEFT_TENSOR_EOVERFLOW;
    if (wt_add_ovf(span, dsz, &span)) return WEFT_TENSOR_EOVERFLOW;
    *span_out = span;
    return WEFT_TENSOR_OK;
}

// ---------------------------------------------------------------------------
// §3.1 Construction
// ---------------------------------------------------------------------------

int weft_tensor_view_init(weft_tensor_view_t* v, uint64_t tensor_id,
                          weft_dtype_t dtype, uint8_t ndim,
                          const uint64_t* shape, uintptr_t base,
                          uint64_t byte_offset) {
    if (v == NULL || shape == NULL) return WEFT_TENSOR_EINVAL;
    if (weft_dtype_size(dtype) == 0) return WEFT_TENSOR_EDTYPE;
    if (ndim < 1 || ndim > WEFT_TENSOR_MAX_DIMS) return WEFT_TENSOR_EDIM;
    for (uint8_t k = 0; k < ndim; k++) {
        if (shape[k] == 0) return WEFT_TENSOR_ERANGE;
    }

    const uint32_t dsz = weft_dtype_size(dtype);

    // Row-major byte strides, last axis tight. Strides live in int64;
    // a contiguous stride beyond 2^63-1 means a >8-exabyte span — refused
    // as EOVERFLOW rather than cast to a negative stride (honest wall).
    int64_t strides[WEFT_TENSOR_MAX_DIMS];
    memset(strides, 0, sizeof(strides));
    uint64_t acc = dsz;  // running trailing product (bytes)
    for (int k = (int)ndim - 1; k >= 0; k--) {
        if (acc > (uint64_t)INT64_MAX) return WEFT_TENSOR_EOVERFLOW;
        strides[k] = (int64_t)acc;
        if (k > 0 && wt_mul_ovf(acc, shape[k], &acc))
            return WEFT_TENSOR_EOVERFLOW;
    }

    // byte_length = nelements * dtype_size (overflow wall).
    uint64_t nelem = 1;
    for (uint8_t k = 0; k < ndim; k++) {
        if (wt_mul_ovf(nelem, shape[k], &nelem)) return WEFT_TENSOR_EOVERFLOW;
    }
    uint64_t byte_length;
    if (wt_mul_ovf(nelem, dsz, &byte_length)) return WEFT_TENSOR_EOVERFLOW;

    memset(v, 0, sizeof(*v));
    v->tensor_id = tensor_id;
    v->dtype = dtype;
    v->ndim = ndim;
    for (uint8_t k = 0; k < ndim; k++) v->shape[k] = shape[k];
    memcpy(v->strides, strides, sizeof(strides));
    v->byte_offset = byte_offset;
    v->byte_length = byte_length;
    v->physical_or_shm_addr = base;
    return WEFT_TENSOR_OK;
}

int weft_tensor_view_init_strided(weft_tensor_view_t* v, uint64_t tensor_id,
                                  weft_dtype_t dtype, uint8_t ndim,
                                  const uint64_t* shape,
                                  const int64_t* strides, uintptr_t base,
                                  uint64_t byte_offset,
                                  uint64_t byte_length) {
    if (v == NULL || shape == NULL || strides == NULL) return WEFT_TENSOR_EINVAL;
    if (weft_dtype_size(dtype) == 0) return WEFT_TENSOR_EDTYPE;
    if (ndim < 1 || ndim > WEFT_TENSOR_MAX_DIMS) return WEFT_TENSOR_EDIM;
    for (uint8_t k = 0; k < ndim; k++) {
        if (shape[k] == 0) return WEFT_TENSOR_ERANGE;
    }

    memset(v, 0, sizeof(*v));
    v->tensor_id = tensor_id;
    v->dtype = dtype;
    v->ndim = ndim;
    for (uint8_t k = 0; k < ndim; k++) {
        v->shape[k] = shape[k];
        v->strides[k] = strides[k];
    }
    v->byte_offset = byte_offset;
    v->byte_length = byte_length;
    v->physical_or_shm_addr = base;
    return WEFT_TENSOR_OK;
}

// ---------------------------------------------------------------------------
// §3.1 Validation — the static-analysis wall
// ---------------------------------------------------------------------------

int weft_tensor_view_validate(const weft_tensor_view_t* v, uint32_t align_req) {
    if (v == NULL) return WEFT_TENSOR_EINVAL;
    switch (align_req) {
        case WEFT_TENSOR_ALIGN_NONE:
        case WEFT_TENSOR_ALIGN_16:
        case WEFT_TENSOR_ALIGN_32:
        case WEFT_TENSOR_ALIGN_64:
        case WEFT_TENSOR_ALIGN_128:
            break;
        default:
            return WEFT_TENSOR_EINVAL;
    }
    if (weft_dtype_size(v->dtype) == 0) return WEFT_TENSOR_EDTYPE;
    if (v->ndim < 1 || v->ndim > WEFT_TENSOR_MAX_DIMS) return WEFT_TENSOR_EDIM;
    if (!wt_tail_canonical(v)) return WEFT_TENSOR_EDIM;

    uint64_t u, lneg;
    int st = wt_extent(v, &u, &lneg);
    if (st != WEFT_TENSOR_OK) return st;

    const uint32_t dsz = weft_dtype_size(v->dtype);

    // (i) the window never dips below the payload base.
    if (v->byte_offset < lneg) return WEFT_TENSOR_ERANGE;

    // (iii) the far end of the window does not wrap 2^64.
    uint64_t far_end;
    if (wt_add_ovf(v->byte_offset, u, &far_end)) return WEFT_TENSOR_EOVERFLOW;
    if (wt_add_ovf(far_end, dsz, &far_end)) return WEFT_TENSOR_EOVERFLOW;

    // (ii) the declared span covers the tight walking window.
    uint64_t span;
    if (wt_add_ovf(lneg, u, &span)) return WEFT_TENSOR_EOVERFLOW;
    if (wt_add_ovf(span, dsz, &span)) return WEFT_TENSOR_EOVERFLOW;
    if (v->byte_length < span) return WEFT_TENSOR_ERANGE;

    // Law 4: alignment honesty — dtype-natural, then the explicit class.
    const uint64_t anchor = (uint64_t)v->physical_or_shm_addr + v->byte_offset;
    if (anchor % weft_dtype_align(v->dtype) != 0) return WEFT_TENSOR_EMISALIGN;
    if (align_req != WEFT_TENSOR_ALIGN_NONE && anchor % align_req != 0)
        return WEFT_TENSOR_EMISALIGN;

    return WEFT_TENSOR_OK;
}

// ---------------------------------------------------------------------------
// §3.2 Address math
// ---------------------------------------------------------------------------

int weft_tensor_view_element_offset(const weft_tensor_view_t* v,
                                    const uint64_t* idx, uint64_t* out_offset) {
    if (v == NULL || idx == NULL || out_offset == NULL) return WEFT_TENSOR_EINVAL;
    if (weft_dtype_size(v->dtype) == 0) return WEFT_TENSOR_EDTYPE;
    if (v->ndim < 1 || v->ndim > WEFT_TENSOR_MAX_DIMS) return WEFT_TENSOR_EDIM;

    uint64_t acc = v->byte_offset;
    for (uint8_t k = 0; k < v->ndim; k++) {
        if (idx[k] >= v->shape[k]) return WEFT_TENSOR_ERANGE;
        const int64_t s = v->strides[k];
        if (s >= 0) {
            uint64_t term;
            if (wt_mul_ovf((uint64_t)s, idx[k], &term)) return WEFT_TENSOR_EOVERFLOW;
            if (wt_add_ovf(acc, term, &acc)) return WEFT_TENSOR_EOVERFLOW;
        } else {
            uint64_t term;
            if (wt_mul_ovf(wt_abs_stride(s), idx[k], &term)) return WEFT_TENSOR_EOVERFLOW;
            if (acc < term) return WEFT_TENSOR_ERANGE;  // below payload base
            acc -= term;
        }
    }
    *out_offset = acc;
    return WEFT_TENSOR_OK;
}

void* weft_tensor_view_element_addr(const weft_tensor_view_t* v,
                                    const uint64_t* idx) {
    if (v == NULL || idx == NULL || v->physical_or_shm_addr == 0) return NULL;
    uint64_t off;
    if (weft_tensor_view_element_offset(v, idx, &off) != WEFT_TENSOR_OK) return NULL;
    return (void*)(v->physical_or_shm_addr + off);
}

void* weft_tensor_view_element_addr_at(const weft_tensor_view_t* v,
                                       const uint64_t* idx,
                                       const void* payload_base) {
    if (v == NULL || idx == NULL || payload_base == NULL) return NULL;
    uint64_t off;
    if (weft_tensor_view_element_offset(v, idx, &off) != WEFT_TENSOR_OK) return NULL;
    return (void*)((uintptr_t)payload_base + off);
}

// ---------------------------------------------------------------------------
// §3.3 View algebra (zero allocation: dst is caller stack storage)
// ---------------------------------------------------------------------------

int weft_tensor_view_slice(weft_tensor_view_t* dst,
                           const weft_tensor_view_t* src,
                           uint8_t axis, uint64_t start, uint64_t count) {
    if (dst == NULL || src == NULL) return WEFT_TENSOR_EINVAL;
    if (src->ndim < 1 || axis >= src->ndim) return WEFT_TENSOR_EDIM;
    if (count == 0) return WEFT_TENSOR_ERANGE;
    uint64_t end;
    if (wt_add_ovf(start, count, &end)) return WEFT_TENSOR_EOVERFLOW;
    if (end > src->shape[axis]) return WEFT_TENSOR_ERANGE;

    // Shallow geometry check before deriving.
    if (weft_dtype_size(src->dtype) == 0) return WEFT_TENSOR_EDTYPE;
    if (!wt_tail_canonical(src)) return WEFT_TENSOR_EDIM;

    // Displace the window origin along `axis`.
    uint64_t byte_offset = src->byte_offset;
    const int64_t s = src->strides[axis];
    if (s >= 0) {
        uint64_t term;
        if (wt_mul_ovf((uint64_t)s, start, &term)) return WEFT_TENSOR_EOVERFLOW;
        if (wt_add_ovf(byte_offset, term, &byte_offset)) return WEFT_TENSOR_EOVERFLOW;
    } else {
        uint64_t term;
        if (wt_mul_ovf(wt_abs_stride(s), start, &term)) return WEFT_TENSOR_EOVERFLOW;
        if (byte_offset < term) return WEFT_TENSOR_ERANGE;
        byte_offset -= term;
    }

    *dst = *src;
    dst->shape[axis] = count;
    dst->byte_offset = byte_offset;

    // Tight span for the narrowed window (monotone-shrinking: provably a
    // sub-box of the parent's window, so the validate walls hold).
    uint64_t span;
    const int st = wt_span(dst, &span);
    if (st != WEFT_TENSOR_OK) return st;
    dst->byte_length = span;
    return WEFT_TENSOR_OK;
}

int weft_tensor_view_subwindow(weft_tensor_view_t* dst,
                               const weft_tensor_view_t* src,
                               const uint64_t* offset, const uint64_t* count) {
    if (dst == NULL || src == NULL || offset == NULL || count == NULL)
        return WEFT_TENSOR_EINVAL;
    if (src->ndim < 1 || src->ndim > WEFT_TENSOR_MAX_DIMS) return WEFT_TENSOR_EDIM;
    if (weft_dtype_size(src->dtype) == 0) return WEFT_TENSOR_EDTYPE;
    if (!wt_tail_canonical(src)) return WEFT_TENSOR_EDIM;

    // Displace along every axis simultaneously (signed, walled).
    uint64_t byte_offset = src->byte_offset;
    for (uint8_t k = 0; k < src->ndim; k++) {
        if (count[k] == 0) return WEFT_TENSOR_ERANGE;
        uint64_t end;
        if (wt_add_ovf(offset[k], count[k], &end)) return WEFT_TENSOR_EOVERFLOW;
        if (end > src->shape[k]) return WEFT_TENSOR_ERANGE;

        const int64_t s = src->strides[k];
        if (s >= 0) {
            uint64_t term;
            if (wt_mul_ovf((uint64_t)s, offset[k], &term)) return WEFT_TENSOR_EOVERFLOW;
            if (wt_add_ovf(byte_offset, term, &byte_offset)) return WEFT_TENSOR_EOVERFLOW;
        } else {
            uint64_t term;
            if (wt_mul_ovf(wt_abs_stride(s), offset[k], &term)) return WEFT_TENSOR_EOVERFLOW;
            if (byte_offset < term) return WEFT_TENSOR_ERANGE;
            byte_offset -= term;
        }
    }

    *dst = *src;
    for (uint8_t k = 0; k < src->ndim; k++) dst->shape[k] = count[k];
    dst->byte_offset = byte_offset;

    uint64_t span;
    const int st = wt_span(dst, &span);
    if (st != WEFT_TENSOR_OK) return st;
    dst->byte_length = span;
    return WEFT_TENSOR_OK;
}

int weft_tensor_view_permute(weft_tensor_view_t* dst,
                             const weft_tensor_view_t* src,
                             const uint8_t* perm) {
    if (dst == NULL || src == NULL || perm == NULL) return WEFT_TENSOR_EINVAL;
    if (src->ndim < 1 || src->ndim > WEFT_TENSOR_MAX_DIMS) return WEFT_TENSOR_EDIM;
    if (weft_dtype_size(src->dtype) == 0) return WEFT_TENSOR_EDTYPE;
    if (!wt_tail_canonical(src)) return WEFT_TENSOR_EDIM;

    // Bijection proof (a non-permutation is EINVAL, never trusted).
    uint8_t seen[WEFT_TENSOR_MAX_DIMS] = {0};
    for (uint8_t k = 0; k < src->ndim; k++) {
        if (perm[k] >= src->ndim) return WEFT_TENSOR_EINVAL;
        if (seen[perm[k]]) return WEFT_TENSOR_EINVAL;
        seen[perm[k]] = 1;
    }

    *dst = *src;
    for (uint8_t k = 0; k < src->ndim; k++) {
        dst->shape[k] = src->shape[perm[k]];
        dst->strides[k] = src->strides[perm[k]];
    }
    // byte_offset / byte_length / element set unchanged: permuting the
    // index order permutes which (i,k) reaches which address, not the
    // window itself.
    return WEFT_TENSOR_OK;
}

int weft_tensor_view_reshape(weft_tensor_view_t* dst,
                             const weft_tensor_view_t* src,
                             uint8_t new_ndim, const uint64_t* new_shape) {
    if (dst == NULL || src == NULL || new_shape == NULL) return WEFT_TENSOR_EINVAL;
    if (new_ndim < 1 || new_ndim > WEFT_TENSOR_MAX_DIMS) return WEFT_TENSOR_EDIM;
    if (weft_dtype_size(src->dtype) == 0) return WEFT_TENSOR_EDTYPE;
    if (src->ndim < 1 || src->ndim > WEFT_TENSOR_MAX_DIMS) return WEFT_TENSOR_EDIM;
    for (uint8_t k = 0; k < new_ndim; k++) {
        if (new_shape[k] == 0) return WEFT_TENSOR_ERANGE;
    }

    // Zero-copy reshape requires C-contiguity — a strided view's element
    // order is not a contiguous address run; materializing it is a COPY,
    // which is Engineer 3's business, never silently ours (ERESHAPE).
    if (!weft_tensor_view_is_contiguous(src)) return WEFT_TENSOR_ERESHAPE;

    // Element-count preservation, both sides overflow-walled.
    uint64_t old_nelem = 1;
    for (uint8_t k = 0; k < src->ndim; k++) {
        if (wt_mul_ovf(old_nelem, src->shape[k], &old_nelem))
            return WEFT_TENSOR_EOVERFLOW;
    }
    uint64_t new_nelem = 1;
    for (uint8_t k = 0; k < new_ndim; k++) {
        if (wt_mul_ovf(new_nelem, new_shape[k], &new_nelem))
            return WEFT_TENSOR_EOVERFLOW;
    }
    if (old_nelem != new_nelem) return WEFT_TENSOR_ERESHAPE;

    *dst = *src;
    dst->ndim = new_ndim;
    memset(dst->shape, 0, sizeof(dst->shape));
    memset(dst->strides, 0, sizeof(dst->strides));
    for (uint8_t k = 0; k < new_ndim; k++) dst->shape[k] = new_shape[k];

    // Fresh row-major strides over the new shape (same INT64_MAX wall).
    const uint32_t dsz = weft_dtype_size(src->dtype);
    uint64_t acc = dsz;
    for (int k = (int)new_ndim - 1; k >= 0; k--) {
        if (acc > (uint64_t)INT64_MAX) return WEFT_TENSOR_EOVERFLOW;
        dst->strides[k] = (int64_t)acc;
        if (k > 0 && wt_mul_ovf(acc, new_shape[k], &acc))
            return WEFT_TENSOR_EOVERFLOW;
    }
    // byte_offset passes through; byte_length (declared span) is PRESERVED
    // — monotone: the new tight extent equals old_nelem*dsz which was
    // already covered.
    return WEFT_TENSOR_OK;
}

int weft_tensor_view_is_contiguous(const weft_tensor_view_t* v) {
    if (v == NULL) return 0;
    if (weft_dtype_size(v->dtype) == 0) return 0;
    if (v->ndim < 1 || v->ndim > WEFT_TENSOR_MAX_DIMS) return 0;

    const uint32_t dsz = weft_dtype_size(v->dtype);
    uint64_t expected = dsz;
    for (int k = (int)v->ndim - 1; k >= 0; k--) {
        if (v->shape[k] == 1) continue;  // size-1 axes constrain nothing
        if ((uint64_t)v->strides[k] != expected) return 0;
        if (wt_mul_ovf(expected, v->shape[k], &expected)) return 0;
    }
    return 1;
}

uint64_t weft_tensor_view_nelements(const weft_tensor_view_t* v) {
    if (v == NULL) return 0;
    if (v->ndim < 1 || v->ndim > WEFT_TENSOR_MAX_DIMS) return 0;
    uint64_t n = 1;
    for (uint8_t k = 0; k < v->ndim; k++) {
        if (wt_mul_ovf(n, v->shape[k], &n)) return UINT64_MAX;  // saturated
    }
    return n;
}
