// weft_ggml_plan.c — RFC-0017 §6 plan layer (pure logic; gated
// everywhere — the AC-G battery runs this without libggml present).
//
// GGML GEOMETRY ORDER (the classic trap this layer exists to own): the
// view's dims are row-major with dims[rank-1] INNERMOST (strides
// canonical, strides[rank-1] == 1); ggml's ne[] puts ne[0] innermost.
// The mapping is the REVERSE of the view's dims, and nb follows the
// ggml recurrence (nb[0] = type size; nb[i] = nb[i-1] * ne[i-1] —
// with the block-size refinement for quantized types, none of which
// the WTS1 dialect maps, so nb[0] is the plain element size here).

#include "weft_ggml_bridge.h"

#include <string.h>

int weft_ggml_type_code(weft_tensor_dtype_t t) {
    switch (t) {
    case WEFT_TENSOR_F32: return WEFT_GGML_TYPE_F32;
    case WEFT_TENSOR_F16: return WEFT_GGML_TYPE_F16;
    case WEFT_TENSOR_I8:  return WEFT_GGML_TYPE_I8;
    case WEFT_TENSOR_I16: return WEFT_GGML_TYPE_I16;
    case WEFT_TENSOR_I32: return WEFT_GGML_TYPE_I32;
    case WEFT_TENSOR_I64: return WEFT_GGML_TYPE_I64;
    case WEFT_TENSOR_F64: return WEFT_GGML_TYPE_F64;
    default:
        // ggml carries no unsigned element types — the NAMED refusal
        // (u8/u16/u32/u64 map through the [FALLBACK-COPY] conversion
        // road: widen to i32/f32 with a labeled copy, never a guess).
        return -1;
    }
}

static size_t ggml_elem_size(int ggml_type) {
    switch (ggml_type) {
    case WEFT_GGML_TYPE_F32:
    case WEFT_GGML_TYPE_I32: return 4;
    case WEFT_GGML_TYPE_F16:
    case WEFT_GGML_TYPE_I16: return 2;
    case WEFT_GGML_TYPE_I8:  return 1;
    case WEFT_GGML_TYPE_I64:
    case WEFT_GGML_TYPE_F64: return 8;
    default:                 return 0;
    }
}

int weft_ggml_shape_of_view(const weft_tensor_view_t* v,
                            weft_ggml_shape_t* out) {
    if (!v || !out) return -1;
    memset(out, 0, sizeof(*out));

    out->err = weft_tensor_view_validate(v, 0);
    if (out->err != WEFT_TV_OK) return -1;
    if (v->flags & WEFT_TENSOR_VIEW_F_BIG_ENDIAN) {
        out->err = WEFT_TV_ERR_ENDIAN;  // conversion is the fallback road
        return -1;
    }
    if (!(v->flags & WEFT_TENSOR_VIEW_F_PACKED)) {
        // Non-contiguous views need ggml views (the caller's road, the
        // plan layer never pretends a stride is a shape).
        out->err = WEFT_TV_ERR_STRIDES;
        return -1;
    }
    out->ggml_type = weft_ggml_type_code((weft_tensor_dtype_t)v->dtype);
    if (out->ggml_type < 0) {
        out->err = WEFT_TV_ERR_DTYPE;
        return -1;
    }

    // view dims[0..rank) (outermost..innermost) -> ggml ne[0..rank)
    // (innermost..outermost): REVERSED, tail = 1.
    for (int k = 0; k < 4; k++) out->ne[k] = 1;
    for (uint8_t k = 0; k < v->rank; k++) {
        out->ne[k] = (int64_t)v->dims[v->rank - 1u - k];
    }

    size_t es = ggml_elem_size(out->ggml_type);
    out->nb[0] = es;
    for (int k = 1; k < 4; k++) {
        out->nb[k] = out->nb[k - 1] * (size_t)out->ne[k - 1];
    }
    out->nbytes = (uint64_t)out->nb[3] * (uint64_t)out->ne[3];
    out->ok = 1;

    // Cross-check against the view's own byte math (a disagreement is a
    // plan bug — caught here, never at the tensor).
    if (out->nbytes != v->byte_len) {
        out->ok = 0;
        out->err = WEFT_TV_ERR_SPAN;
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Placement planner
// ---------------------------------------------------------------------------

void weft_ggml_planner_init(weft_ggml_planner_t* p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->align = 64;  // Law 2: the float-vector alignment floor
}

int weft_ggml_planner_register(weft_ggml_planner_t* p, void* base,
                               uint64_t bytes) {
    if (!p || !base || bytes < 64) return -1;
    if (p->n >= WEFT_GGML_MAX_SPANS) return -1;
    if (((uintptr_t)base & 15u) != 0) return -1;  // 16-byte floor (Law 2)
    p->spans[p->n].base = (uint8_t*)base;
    p->spans[p->n].bytes = bytes;
    p->spans[p->n].used = 0;
    p->n++;
    return 0;
}

static uint64_t align_up(uint64_t x, uint64_t a) {
    return ((x + a - 1) / a) * a;
}

int weft_ggml_planner_place(weft_ggml_planner_t* p, uint64_t nbytes,
                            int* out_span, uint64_t* out_off) {
    if (!p || nbytes == 0) return -1;
    for (int s = 0; s < p->n; s++) {
        uint64_t off = align_up(p->spans[s].used, p->align);
        if (off + nbytes <= p->spans[s].bytes) {
            p->spans[s].used = off + nbytes;
            p->placed++;
            if (out_span) *out_span = s;
            if (out_off) *out_off = off;
            return 0;
        }
    }
    p->refused++;  // counted, never silent — the [FALLBACK-COPY] signal
    return -1;
}

int weft_ggml_planner_place_at(weft_ggml_planner_t* p, int span,
                               uint64_t off, uint64_t nbytes) {
    if (!p || span < 0 || span >= p->n) return -1;
    if ((off % p->align) != 0) return -1;  // the alignment law, enforced
    if (off + nbytes > p->spans[span].bytes) return -1;
    if (off + nbytes > p->spans[span].used) {
        p->spans[span].used = off + nbytes;  // advance the cursor
    }
    p->placed++;
    return 0;
}

void weft_ggml_planner_reset(weft_ggml_planner_t* p) {
    if (!p) return;
    for (int s = 0; s < p->n; s++) p->spans[s].used = 0;
}

// ---------------------------------------------------------------------------
// Audio windowing
// ---------------------------------------------------------------------------

int weft_ggml_audio_plan(uint32_t total_elems, uint32_t window,
                         uint32_t hop, int allow_partial,
                         weft_ggml_audio_win_t* out_wins,
                         uint32_t max_wins, uint32_t* out_n) {
    if (!out_wins || !out_n || window == 0 || hop == 0 || hop > window) {
        return -1;  // hop > window would drop samples — refused, never
                    // silently resampled
    }
    if (total_elems == 0) {
        *out_n = 0;
        return 0;
    }
    uint32_t n = 0;
    uint32_t off = 0;
    while (off < total_elems) {
        uint32_t avail = total_elems - off;
        uint32_t len = (avail >= window) ? window : avail;
        int partial = (len < window);
        if (partial && !allow_partial) break;  // tail DROPPED by policy
        if (n >= max_wins) return -1;          // caller under-provisioned
        out_wins[n].offset_elems = off;
        out_wins[n].n_elems = len;
        out_wins[n].partial = partial;
        n++;
        off += hop;
    }
    *out_n = n;
    return 0;
}
