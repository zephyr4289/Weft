// weft_metal_core.c — RFC-0017 §4 portable core.
//
// Everything here compiles and is gate-tested on EVERY platform: the
// geometry math (the wrap descriptor discipline), the frozen MSL text,
// and the non-Apple truth (weak stubs that refuse honestly). The device
// roads live in weft_metal_apple.mm and override the weak symbols at
// link time on __APPLE__ (single API surface, zero ifdefs at call sites).

#include "weft_metal_bridge.h"

#include <string.h>

#include "weft/weft_accel_common.h"

// ---------------------------------------------------------------------------
// The frozen MSL kernel (mirrors weft_preprocess.{comp,wgsl} — the
// one-multiply normalize contract; committed as
// shaders/weft_preprocess.metal, byte-identical to this string)
// ---------------------------------------------------------------------------

static const char kWeftPreprocessMSL[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct WeftPreprocessPush {\n"
"    uint src_word_off;\n"
"    uint dst_elem_off;\n"
"    uint n_pixels;\n"
"    float scale;\n"
"};\n"
"\n"
"kernel void weft_preprocess(device const uint* src_words [[buffer(0)]],\n"
"                            device float* dst_elems   [[buffer(1)]],\n"
"                            constant WeftPreprocessPush& p [[buffer(2)]],\n"
"                            uint id [[thread_position_in_grid]]) {\n"
"    if (id >= p.n_pixels) return;\n"
"    uint w = src_words[p.src_word_off + id];\n"
"    uint o = p.dst_elem_off + id * 4u;\n"
"    dst_elems[o + 0u] = float(w & 0xFFu)         * p.scale;\n"
"    dst_elems[o + 1u] = float((w >> 8u) & 0xFFu)  * p.scale;\n"
"    dst_elems[o + 2u] = float((w >> 16u) & 0xFFu) * p.scale;\n"
"    dst_elems[o + 3u] = float((w >> 24u) & 0xFFu) * p.scale;\n"
"}\n";

const char* weft_metal_preprocess_msl(size_t* out_len) {
    if (out_len) *out_len = sizeof(kWeftPreprocessMSL) - 1u;
    return kWeftPreprocessMSL;
}

uint64_t weft_metal_preprocess_frozen_id(void) {
    return weft_accel_frozen_id(kWeftPreprocessMSL,
                                sizeof(kWeftPreprocessMSL) - 1u);
}

// ---------------------------------------------------------------------------
// Geometry (pure math + the refusal ladder)
// ---------------------------------------------------------------------------

int weft_metal_geometry_for_view(const weft_tensor_view_t* v,
                                 weft_metal_geometry_t* out) {
    if (!v || !out) return -1;
    memset(out, 0, sizeof(*out));

    out->view_err = weft_tensor_view_gpu_ready(v);  // LE + alignment + span
    if (out->view_err != WEFT_TV_OK) return -1;

    const int is_u8 = (v->dtype == (uint8_t)WEFT_TENSOR_U8);
    const int is_f32 = (v->dtype == (uint8_t)WEFT_TENSOR_F32);
    if (!is_u8 && !is_f32) {
        out->view_err = WEFT_TV_ERR_DTYPE;  // the two v1 wrap roads
        return -1;
    }
    if (!(v->flags & WEFT_TENSOR_VIEW_F_PACKED)) {
        out->view_err = WEFT_TV_ERR_STRIDES;  // wraps need contiguous rows
        return -1;
    }

    // Row/geometry derivation (NHWC discipline): one ROW spans the last
    // rank-1 axes (W*C for a rank-3 camera frame); dims[0] is the row
    // count. RGBA8 camera road: the row's ELEMENT count must be a whole
    // number of 4-byte pixels. F32 plane road: rows of prod(dims[1..)).
    uint64_t elems_per_row;
    uint64_t rows;
    if (v->rank >= 2) {
        rows = v->dims[0];
        elems_per_row = 1;
        for (uint32_t k = 1; k < v->rank; k++) {
            elems_per_row *= (uint64_t)v->dims[k];
        }
    } else {
        elems_per_row = v->elem_count;
        rows = 1;
    }
    if (rows == 0) {
        out->view_err = WEFT_TV_ERR_DIMS;
        return -1;
    }
    if (is_u8) {
        if ((elems_per_row & 3u) != 0) {
            out->view_err = WEFT_TV_ERR_DIMS;  // whole RGBA pixels only
            return -1;
        }
        out->bytes_per_element = 4;  // one RGBA8 pixel
        elems_per_row /= 4u;
    } else {
        out->bytes_per_element = 4;  // f32
    }

    out->width = (uint32_t)elems_per_row;
    out->height = (uint32_t)(rows > 0xFFFFFFFFull ? 0 : rows);
    if (out->width == 0 || out->height == 0) {
        out->view_err = WEFT_TV_ERR_DIMS;
        return -1;
    }

    // Law 2: rows >= 16 bytes, 64-aligned bytesPerRow (the IOSurface /
    // CVPixelBuffer discipline), and the 16-byte pointer gate already ran
    // in gpu_ready. Padding bytes (row tail) are the PRODUCER's bytes —
    // the wrap covers span_bytes, the view's byte_len covers payload only.
    uint64_t raw_row = (uint64_t)out->width * out->bytes_per_element;
    if (raw_row < 16) {
        out->view_err = WEFT_TV_ERR_SPAN;  // bytesNoCopy floor: >= 16 B
        return -1;
    }
    out->bytes_per_row = (uint32_t)((raw_row + 63u) & ~63u);
    out->span_bytes = (uint64_t)out->bytes_per_row * out->height;
    if (out->span_bytes < v->byte_len) {
        out->view_err = WEFT_TV_ERR_CAPACITY;  // padded rows exceed the
        return -1;                             // backing span — refused
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Capability probe + device roads — WEAK stubs (the non-Apple truth).
// weft_metal_apple.mm overrides these on __APPLE__; on every other
// platform they are the linked truth: honest UNSUPPORTED refusals.
// ---------------------------------------------------------------------------

__attribute__((weak)) weft_metal_caps_t weft_metal_probe(void) {
    return WEFT_METAL_UNSUPPORTED;
}

const char* weft_metal_caps_name(weft_metal_caps_t c) {
    switch (c) {
    case WEFT_METAL_UNIFIED:    return "APPLE-UNIFIED (zero-copy road)";
    case WEFT_METAL_DISCRETE:   return "APPLE-DISCRETE (wraps refuse)";
    default:                    return "UNSUPPORTED (non-Apple weak stub)";
    }
}

__attribute__((weak)) int weft_metal_wrap_mtlbuffer(
    const weft_tensor_view_t* v, void** out_buffer) {
    (void)v;
    if (out_buffer) *out_buffer = NULL;
    return -1;  // non-Apple: the honest refusal (never a pretend wrap)
}

__attribute__((weak)) int weft_metal_wrap_cvpixelbuffer(
    const weft_tensor_view_t* v, void** out_pb) {
    (void)v;
    if (out_pb) *out_pb = NULL;
    return -1;
}

__attribute__((weak)) int weft_metal_iosurface_span(
    uint32_t width, uint32_t height, uint32_t bytes_per_element,
    void** out_surface, void** out_bytes, uint64_t* out_span) {
    (void)width; (void)height; (void)bytes_per_element;
    if (out_surface) *out_surface = NULL;
    if (out_bytes)   *out_bytes = NULL;
    if (out_span)    *out_span = 0;
    return -1;
}

__attribute__((weak)) void weft_metal_release(void* handle) {
    (void)handle;  // nothing to release on the stub platform
}

__attribute__((weak)) int weft_metal_pool_create(void** out_pool,
                                                 const char* msl_source,
                                                 size_t msl_len,
                                                 uint64_t frozen_id_expect) {
    (void)msl_source; (void)msl_len; (void)frozen_id_expect;
    if (out_pool) *out_pool = NULL;
    return -1;
}

__attribute__((weak)) int weft_metal_pool_preprocess(
    void* pool, void* src_buffer, uint64_t src_byte_off, void* dst_buffer,
    uint64_t dst_byte_off, const weft_metal_preprocess_push_t* push) {
    (void)pool; (void)src_buffer; (void)src_byte_off; (void)dst_buffer;
    (void)dst_byte_off; (void)push;
    return -1;
}
