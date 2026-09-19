// weft_tensor.c — the WTS1 tensor stream bridge (RFC-0016 §5).

#include "weft_tensor.h"

#include <string.h>

#include "weft_f16_codec.h"

// ---------------------------------------------------------------------------
// Header wire codec (little-endian, the house style)
// ---------------------------------------------------------------------------

static void t_put_u16le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}
static void t_put_u32le(uint8_t* p, uint32_t v) {
    t_put_u16le(p, (uint16_t)(v & 0xFFFFu));
    t_put_u16le(p + 2, (uint16_t)(v >> 16));
}
static uint32_t t_get_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t dtype_size[] = {
    1, 2, 4, 8,  // u8..u64
    1, 2, 4, 8,  // i8..i64
    2, 4, 8,     // f16, f32, f64
};

size_t weft_tensor_elem_size(weft_tensor_dtype_t t) {
    if ((int)t < 0 || (size_t)t >= sizeof(dtype_size) / sizeof(dtype_size[0]))
        return 0;
    return dtype_size[t];
}

const char* weft_tensor_dtype_name(weft_tensor_dtype_t t) {
    static const char* names[] = {
        "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64",
        "f16", "f32", "f64",
    };
    if ((int)t < 0 || (size_t)t >= sizeof(names) / sizeof(names[0]))
        return "?";
    return names[t];
}

// ---------------------------------------------------------------------------
// Publisher
// ---------------------------------------------------------------------------

uint8_t* weft_tensor_frame_begin(weft_fanout_t* f, weft_tensor_dtype_t dt,
                                 const uint32_t* dims, uint8_t rank) {
    if (f == NULL || dims == NULL || rank < 1 || rank > 4) return NULL;
    const size_t esz = weft_tensor_elem_size(dt);
    if (esz == 0) return NULL;

    uint64_t count = 1;
    for (uint8_t k = 0; k < rank; k++) {
        if (dims[k] == 0) return NULL;
        count *= dims[k];
        if (count > 0xFFFFFFFFull) return NULL;  // elem_count is u32
    }
    const size_t payload_bytes =
        ((size_t)count * esz + 3u) & ~(size_t)3u;  // round up to u32 words
    if (WEFT_TENSOR_HEADER_BYTES + payload_bytes > f->payload_bytes)
        return NULL;  // refused BEFORE begin(): no seq burned (Law 4)

    uint8_t* cursor = weft_fanout_begin(f);
    if (cursor == NULL) return NULL;

    memset(cursor, 0, WEFT_TENSOR_HEADER_BYTES);
    t_put_u32le(cursor + 0, WEFT_TENSOR_MAGIC);
    cursor[4] = 1;              // version
    cursor[5] = (uint8_t)dt;
    cursor[6] = rank;
    cursor[7] = 0;              // flags (reserved, zero)
    t_put_u32le(cursor + 8, (uint32_t)count);
    t_put_u32le(cursor + 12, (uint32_t)(payload_bytes / 4u));
    for (uint8_t k = 0; k < 4; k++)
        t_put_u32le(cursor + 16 + 4u * k, k < rank ? dims[k] : 0u);
    // tail: the pad bytes stay zero (begin() invalidates the slot; the
    // fill writes elem bytes; the pad-to-word tail is zeroed here so a
    // short fill still leaves clean word granularity)
    memset(cursor + WEFT_TENSOR_HEADER_BYTES, 0, payload_bytes);
    return cursor + WEFT_TENSOR_HEADER_BYTES;
}

int weft_tensor_fill_bytes(uint8_t* payload_cursor, const void* src, size_t n) {
    if (payload_cursor == NULL || src == NULL) return -1;
    memcpy(payload_cursor, src, n);  // THE copy (the engine buffer -> slot)
    return 0;
}

int weft_tensor_fill_f32_as_f16(uint8_t* payload_cursor, const float* src,
                                uint32_t n) {
    if (payload_cursor == NULL || src == NULL) return -1;
    uint16_t* dst = (uint16_t*)payload_cursor;
    for (uint32_t i = 0; i < n; i++)
        dst[i] = weft_f32_to_f16(src[i]);  // the codec dialect, element-wise
    return 0;
}

uint64_t weft_tensor_publish_f16(weft_fanout_t* f, const float* vec, uint32_t n) {
    uint32_t dims[4] = { n, 0, 0, 0 };
    uint8_t* cur = weft_tensor_frame_begin(f, WEFT_TENSOR_F16, dims, 1);
    if (cur == NULL) return 0;
    if (weft_tensor_fill_f32_as_f16(cur, vec, n) != 0) return 0;
    return weft_fanout_publish(f);
}

uint64_t weft_tensor_publish_tokens(weft_fanout_t* f, const uint32_t* tokens,
                                    uint32_t n) {
    uint32_t dims[4] = { n, 0, 0, 0 };
    uint8_t* cur = weft_tensor_frame_begin(f, WEFT_TENSOR_U32, dims, 1);
    if (cur == NULL) return 0;
    if (weft_tensor_fill_bytes(cur, tokens, (size_t)n * 4u) != 0) return 0;
    return weft_fanout_publish(f);
}

uint64_t weft_tensor_publish_audio_f32(weft_fanout_t* f, const float* pcm,
                                       uint32_t n) {
    uint32_t dims[4] = { n, 0, 0, 0 };
    uint8_t* cur = weft_tensor_frame_begin(f, WEFT_TENSOR_F32, dims, 1);
    if (cur == NULL) return 0;
    if (weft_tensor_fill_bytes(cur, pcm, (size_t)n * 4u) != 0) return 0;
    return weft_fanout_publish(f);
}

// ---------------------------------------------------------------------------
// Consumer
// ---------------------------------------------------------------------------

int weft_tensor_frame_parse(const void* frame, size_t len,
                            weft_tensor_hdr_t* out_hdr,
                            const void** out_payload) {
    if (frame == NULL || len < WEFT_TENSOR_HEADER_BYTES) return -1;
    const uint8_t* p = (const uint8_t*)frame;
    if (t_get_u32le(p + 0) != WEFT_TENSOR_MAGIC) return -1;
    const uint8_t version = p[4];
    const uint8_t dtype = p[5];
    const uint8_t rank = p[6];
    const uint8_t flags = p[7];
    if (version != 1) return -1;
    if (flags != 0) return -1;               // unknown bits reject
    if (rank < 1 || rank > 4) return -1;
    const size_t esz = weft_tensor_elem_size((weft_tensor_dtype_t)dtype);
    if (esz == 0) return -1;
    const uint32_t count = t_get_u32le(p + 8);
    const uint32_t words = t_get_u32le(p + 12);

    uint64_t prod = 1;
    for (uint8_t k = 0; k < 4; k++) {
        const uint32_t d = t_get_u32le(p + 16 + 4u * k);
        if (k < rank) {
            if (d == 0) return -1;
            prod *= d;
        } else if (d != 0) {
            return -1;                       // dims beyond rank must be zero
        }
    }
    if (prod != count) return -1;
    const size_t expect_bytes = ((size_t)count * esz + 3u) & ~(size_t)3u;
    if ((size_t)words * 4u != expect_bytes) return -1;
    if (WEFT_TENSOR_HEADER_BYTES + (size_t)words * 4u > len) return -1;

    if (out_hdr) {
        out_hdr->version = version;
        out_hdr->dtype = dtype;
        out_hdr->rank = rank;
        out_hdr->flags = flags;
        out_hdr->elem_count = count;
        out_hdr->payload_words = words;
        for (uint8_t k = 0; k < 4; k++)
            out_hdr->dims[k] = t_get_u32le(p + 16 + 4u * k);
    }
    if (out_payload) *out_payload = p + WEFT_TENSOR_HEADER_BYTES;
    return 0;
}

float weft_tensor_f16_at(const void* payload, uint32_t i) {
    const uint16_t* h = (const uint16_t*)payload;
    return weft_f16_to_f32(h[i]);
}

uint32_t weft_tensor_u32_at(const void* payload, uint32_t i) {
    const uint32_t* w = (const uint32_t*)payload;
    return w[i];
}

float weft_tensor_f32_at(const void* payload, uint32_t i) {
    const float* v = (const float*)payload;
    return v[i];
}
