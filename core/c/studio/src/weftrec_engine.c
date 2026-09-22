/* weftrec_engine.c — the Weft Studio .weftrec v1 flight-recorder engine.
 *
 * Format (WEFTREC1, frozen in weft_studio.h):
 *   [0,64)       global header (one cacheline, CRC-32C over [0,60))
 *   [64,...)     frames: 64 B-aligned frame header + stored payload,
 *                zero-padded to the next 64 B boundary; every frame
 *                header carries the offset of the next frame (0 = last)
 *   [index]      weftrec_index_entry_t[frame_count], 64 B-aligned — the
 *                O(log n) timestamp -> offset map the seek SLA rides on
 *   [end]        zero pad to a 64 B multiple
 *
 * Law 1 (zero allocation): the reader walks a caller-pinned buffer in
 * place — the index is READ from the file, never rebuilt; the builder
 * writes into a caller-provided buffer with an in-buffer two-pass codec.
 * Law 3 (portability): this translation unit uses <string.h> only; the
 * SSE4.2 CRC-32C kernel is behind a target attribute + load-time probe,
 * and the whole hardware path compiles away under
 * WEFT_STUDIO_WASM_PORTABLE (the wasm32 profile keeps slicing-by-8).
 *
 * Integrity: CRC-32C per stored payload AND per frame header AND over
 * the global header — three independent tripwires, all fail-closed.
 */
#include "weft_studio_internal.h"
#include "weft_crc32c_tables.h"

/* ------------------------------------------------------------------ */
/* CRC-32C: slicing-by-8 reference + runtime-dispatched hardware kernel */
/* ------------------------------------------------------------------ */
uint32_t weftrec_crc32c_sw(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i = 0;
    /* slicing-by-8 */
    while (len - i >= 8) {
        uint32_t lo = crc ^ (uint32_t)p[i]
                    ^ ((uint32_t)p[i + 1] << 8)
                    ^ ((uint32_t)p[i + 2] << 16)
                    ^ ((uint32_t)p[i + 3] << 24);
        uint32_t hi = (uint32_t)p[i + 4]
                    ^ ((uint32_t)p[i + 5] << 8)
                    ^ ((uint32_t)p[i + 6] << 16)
                    ^ ((uint32_t)p[i + 7] << 24);
        crc = weft_crc32c_slice8[7][lo & 0xFFu]
            ^ weft_crc32c_slice8[6][(lo >> 8) & 0xFFu]
            ^ weft_crc32c_slice8[5][(lo >> 16) & 0xFFu]
            ^ weft_crc32c_slice8[4][(lo >> 24) & 0xFFu]
            ^ weft_crc32c_slice8[3][hi & 0xFFu]
            ^ weft_crc32c_slice8[2][(hi >> 8) & 0xFFu]
            ^ weft_crc32c_slice8[1][(hi >> 16) & 0xFFu]
            ^ weft_crc32c_slice8[0][(hi >> 24) & 0xFFu];
        i += 8;
    }
    for (; i < len; i++)
        crc = weft_crc32c_slice8[0][(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

#if !defined(WEFT_STUDIO_WASM_PORTABLE) && \
    (defined(__x86_64__) || defined(__i386__))
#  define WEFTREC_CRC_X86 1
#  include <nmmintrin.h>
static uint32_t __attribute__((target("sse4.2")))
crc32c_sse42(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i = 0;
    while (len - i >= 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        crc = (uint32_t)_mm_crc32_u64(crc, w);
        i += 8;
    }
    if (len - i >= 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);
        crc = (uint32_t)_mm_crc32_u32(crc, w);
        i += 4;
    }
    for (; i < len; i++)
        crc = (uint32_t)_mm_crc32_u8(crc, p[i]);
    return crc ^ 0xFFFFFFFFu;
}
static uint32_t (*crc32c_dispatch)(const void *, size_t) = weftrec_crc32c_sw;
static int crc32c_hw = 0;

#if defined(__GNUC__)
__attribute__((constructor))
#endif
static void crc32c_probe(void)
{
#if defined(__GNUC__)
    if (__builtin_cpu_supports("sse4.2")) {
        crc32c_dispatch = crc32c_sse42;
        crc32c_hw = 1;
    }
#endif
}

uint32_t weftrec_crc32c(const void *data, size_t len)
{
    return crc32c_dispatch(data, len);
}
int weftrec_crc32c_hw_active(void)
{
    return crc32c_hw;
}

#elif !defined(WEFT_STUDIO_WASM_PORTABLE) && defined(__aarch64__)
#  if defined(__ARM_FEATURE_CRC32)
#    include <arm_acle.h>
static uint32_t crc32c_arm64(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i = 0;
    while (len - i >= 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        crc = __crc32cd(crc, w);
        i += 8;
    }
    if (len - i >= 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);
        crc = __crc32cw(crc, w);
        i += 4;
    }
    for (; i < len; i++)
        crc = __crc32cb(crc, p[i]);
    return crc ^ 0xFFFFFFFFu;
}
uint32_t weftrec_crc32c(const void *data, size_t len)
{
    return crc32c_arm64(data, len);
}
int weftrec_crc32c_hw_active(void)
{
    return 1;
}
#  else
uint32_t weftrec_crc32c(const void *data, size_t len)
{
    return weftrec_crc32c_sw(data, len);
}
int weftrec_crc32c_hw_active(void)
{
    return 0;
}
#  endif

#else /* portable / wasm profile */
uint32_t weftrec_crc32c(const void *data, size_t len)
{
    return weftrec_crc32c_sw(data, len);
}
int weftrec_crc32c_hw_active(void)
{
    return 0;
}
#endif

/* ------------------------------------------------------------------ */
/* dzv codec (delta-zigzag-varint over LE u32 words — RFC-0010 family, */
/* self-limiting: the builder only uses it when it shrinks the record)  */
/* ------------------------------------------------------------------ */
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint32_t zig32(uint32_t d)
{
    return (d << 1) ^ (0u - (d >> 31));
}
static uint32_t unzig32(uint32_t z)
{
    return (z >> 1) ^ (0u - (z & 1u));
}

/* Encodes src[0..len) into dst (cap bytes). Returns the encoded length,
 * or 0 when the stream would not shrink (self-limiting) or would not
 * fit. Tail bytes (len % 4) are stored verbatim after the varint run. */
uint32_t weftrec_dzv_encode(uint8_t *dst, uint32_t cap,
                            const uint8_t *src, uint32_t len)
{
    uint32_t words = len / 4u;
    uint32_t tail = len % 4u;
    uint32_t prev = 0;
    uint32_t w, o = 0;
    for (w = 0; w < words; w++) {
        uint32_t cur = rd_le32(src + 4u * w);
        uint32_t d = cur - prev;
        uint32_t z = zig32(d);
        /* LEB128, at most 5 bytes */
        while (z >= 0x80u) {
            if (o >= cap) return 0;
            dst[o++] = (uint8_t)(z | 0x80u);
            z >>= 7;
        }
        if (o >= cap) return 0;
        dst[o++] = (uint8_t)z;
        prev = cur;
    }
    if (o + tail >= len) return 0;   /* never grow a record */
    if (o + tail > cap) return 0;
    if (tail) memcpy(dst + o, src + 4u * words, tail);
    return o + tail;
}

/* Decodes a dzv stream (stored[0..stored_len), logical length
 * logical_len) into out (cap >= logical_len). Returns the number of
 * bytes consumed or a refusal. */
int weftrec_dzv_decode(const uint8_t *stored, uint32_t stored_len,
                       uint32_t logical_len, uint8_t *out, uint32_t cap)
{
    uint32_t words = logical_len / 4u;
    uint32_t tail = logical_len % 4u;
    uint32_t prev = 0;
    uint32_t w, o = 0;
    if (cap < logical_len) return WEFT_STUDIO_EBOUNDS;
    if ((uint64_t)stored_len < (uint64_t)tail) return WEFT_STUDIO_EPARSE;
    for (w = 0; w < words; w++) {
        uint32_t z = 0, shift = 0, k;
        for (k = 0; k < 5u; k++) {
            uint8_t b;
            if (o >= stored_len) return WEFT_STUDIO_EPARSE;
            b = stored[o++];
            z |= (uint32_t)(b & 0x7Fu) << shift;
            shift += 7u;
            if (!(b & 0x80u)) break;
        }
        if (k == 5u && (stored[o - 1u] & 0x80u)) return WEFT_STUDIO_EPARSE;
        prev = prev + unzig32(z);
        wr_le32(out + 4u * w, prev);
    }
    if (o + tail != stored_len) return WEFT_STUDIO_EPARSE;
    if (tail) memcpy(out + 4u * words, stored + o, tail);
    return WEFT_STUDIO_OK;
}

/* ------------------------------------------------------------------ */
/* Wire helpers (memcpy-only loads/stores; explicit LE)                */
/* ------------------------------------------------------------------ */
/* memcpy-based loads: alignment-safe, aliasing-safe, and on little-endian
 * hosts each compiles to a single load instruction (the shift-chain form
 * serializes and costs ~8x more in the seek binary search). */
static uint64_t rd_le64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static uint32_t rd_le32w(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint16_t rd_le16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static void wr_le64w(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) { p[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}
static void wr_le32w(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static void wr_le16w(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8);
}

static uint64_t align_up_64(uint64_t v)
{
    return (v + 63u) & ~UINT64_C(63);
}

/* The magic is the ASCII byte sequence "WEFTREC1" on disk (a hexdump of
 * any WEFTREC1 trace spells the format name); the ABI constant
 * WEFTREC1_MAGIC (0x5745465452454331) is its big-endian reading, exactly
 * as pinned by the directive. */
static const uint8_t WEFTREC1_MAGIC_BYTES[8] = {
    'W', 'E', 'F', 'T', 'R', 'E', 'C', '1'
};

static int magic_ok(const uint8_t *p)
{
    return memcmp(p, WEFTREC1_MAGIC_BYTES, 8) == 0;
}

/* Loads a frame header (wire discipline). */
static int load_frame_hdr(const uint8_t *base, uint64_t size, uint64_t off,
                          weftrec_frame_header_t *h)
{
    if (off > size || size - off < 64u) return WEFT_STUDIO_ETRUNC;
    if ((off & 63u) != 0u) return WEFT_STUDIO_EALIGN;
    {
        const uint8_t *p = base + off;
        h->magic = rd_le64(p + 0);
        h->timestamp_ns = rd_le64(p + 8);
        h->stream_id = rd_le64(p + 16);
        h->frame_seq = rd_le64(p + 24);
        h->payload_size = rd_le32w(p + 32);
        h->stored_size = rd_le32w(p + 36);
        h->codec = rd_le16(p + 40);
        h->flags = rd_le16(p + 42);
        h->crc32c = rd_le32w(p + 44);
        h->next_offset = rd_le64(p + 48);
        h->header_crc32c = rd_le32w(p + 56);
        h->reserved0 = rd_le32w(p + 60);
    }
    if (!magic_ok(base + off)) return WEFT_STUDIO_EPARSE;
    if (h->reserved0 != 0) return WEFT_STUDIO_EPARSE;
    if (h->codec != WEFTREC_CODEC_RAW && h->codec != WEFTREC_CODEC_DZV)
        return WEFT_STUDIO_EPARSE;
    /* header CRC covers [0,48): next_offset is patched by the NEXT
     * append, so it must live OUTSIDE the CRC'd region */
    if (h->header_crc32c != weftrec_crc32c(base + off, 48))
        return WEFT_STUDIO_ECRC;
    if ((uint64_t)h->stored_size > size - off - 64u)
        return WEFT_STUDIO_ETRUNC;
    return WEFT_STUDIO_OK;
}

/* ------------------------------------------------------------------ */
/* Reader                                                              */
/* ------------------------------------------------------------------ */
int weftrec_reader_open(weftrec_reader_t *r, const void *buf, uint64_t len)
{
    weftrec_header_t h;
    if (!r || !buf) return WEFT_STUDIO_EBOUNDS;
    memset(r, 0, sizeof *r);
    if (len < 64u) return WEFT_STUDIO_ETRUNC;
    if (((uintptr_t)buf & 63u) != 0u) return WEFT_STUDIO_EALIGN;
    {
        const uint8_t *p = (const uint8_t *)buf;
        h.magic = rd_le64(p + 0);
        h.version = rd_le32w(p + 8);
        h.flags = rd_le32w(p + 12);
        h.index_offset = rd_le64(p + 16);
        h.index_count = rd_le64(p + 24);
        h.frame_count = rd_le64(p + 32);
        h.first_ts_ns = rd_le64(p + 40);
        h.last_ts_ns = rd_le64(p + 48);
        h.stream_cardinality = rd_le32w(p + 56);
        h.crc32c = rd_le32w(p + 60);
    }
    if (!magic_ok((const uint8_t *)buf)) return WEFT_STUDIO_EPARSE;
    if (h.version != WEFTREC1_VERSION) return WEFT_STUDIO_EPARSE;
    if ((h.flags & ~WEFTREC_HF_KNOWN_MASK) != 0u) return WEFT_STUDIO_EPARSE;
    if (h.crc32c != weftrec_crc32c(buf, 60)) return WEFT_STUDIO_ECRC;
    if (h.frame_count == 0) {
        if (h.index_count != 0) return WEFT_STUDIO_EPARSE;
        if (h.first_ts_ns != 0 || h.last_ts_ns != 0) return WEFT_STUDIO_EPARSE;
    } else {
        if (h.index_count != h.frame_count) return WEFT_STUDIO_EPARSE;
        if (h.first_ts_ns > h.last_ts_ns) return WEFT_STUDIO_EPARSE;
        if ((h.index_offset & 63u) != 0u) return WEFT_STUDIO_EALIGN;
        if (h.index_offset > len ||
            h.index_count > (len - h.index_offset) / 64u)
            return WEFT_STUDIO_ETRUNC;
    }
    r->base = (const uint8_t *)buf;
    r->size = len;
    r->hdr = (const weftrec_header_t *)buf; /* 64 B-aligned overlay */
    r->index = (h.frame_count != 0)
               ? (const weftrec_index_entry_t *)
                 ((const uint8_t *)buf + h.index_offset) : NULL;
    r->index_count = h.index_count;
    r->cache_line = 64;
    r->flags = 0;
    return WEFT_STUDIO_OK;
}

int weftrec_seek_timestamp(const weftrec_reader_t *r, uint64_t target_ns,
                           weftrec_frame_view_t *out)
{
    uint32_t lo = 0, hi;
    uint32_t pick;
    uint64_t off;
    weftrec_frame_header_t h;
    int rc;
    if (!r || !out || r->index_count == 0) return WEFT_STUDIO_EBOUNDS;
    hi = (uint32_t)(r->index_count - 1u);
    if (target_ns < rd_le64((const uint8_t *)&r->index[0] + 0)) {
        pick = 0;
    } else {
        /* last index entry with timestamp <= target */
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo + 1u) / 2u;
            uint64_t mts = rd_le64((const uint8_t *)&r->index[mid] + 0);
            if (mts <= target_ns) lo = mid;
            else hi = mid - 1u;
        }
        pick = lo;
    }
    off = rd_le64((const uint8_t *)&r->index[pick] + 8); /* frame_offset */
    rc = load_frame_hdr(r->base, r->size, off, &h);
    if (rc) return rc;
    out->hdr = (const weftrec_frame_header_t *)(r->base + off);
    out->payload = r->base + off + 64u;   /* stored bytes (codec-aware) */
    out->payload_len = h.payload_size;    /* logical size */
    out->file_offset = off;
    return WEFT_STUDIO_OK;
}

/* ------------------------------------------------------------------ */
/* Walker                                                              */
/* ------------------------------------------------------------------ */
int weftrec_walker_init(weftrec_walker_t *w, weftrec_reader_t *r,
                        uint64_t from_offset, void *scratch,
                        uint32_t scratch_cap)
{
    if (!w || !r) return WEFT_STUDIO_EBOUNDS;
    w->reader = r;
    w->scratch = (uint8_t *)scratch;
    w->scratch_cap = scratch_cap;
    w->truncated = 0;
    w->steps = 0;
    if (r->index_count == 0) {
        w->next_offset = 0;
        return WEFT_STUDIO_OK;
    }
    w->next_offset = (from_offset == 0) ? 64u : from_offset;
    return WEFT_STUDIO_OK;
}

int weftrec_frame_next(weftrec_walker_t *w, weftrec_frame_view_t *view)
{
    weftrec_frame_header_t h;
    int rc;
    uint64_t off;
    if (!w || !view) return WEFT_STUDIO_EBOUNDS;
    if (w->next_offset == 0) {
        /* clean EOF or already-torn tail */
        w->truncated = 0;
        return WEFT_STUDIO_ETRUNC;
    }
    off = w->next_offset;
    /* cycle defense: next_offset lives outside the header CRC region
     * (the builder back-patches it), so a corrupted chain could loop;
     * a valid trace never has more frames than the index says. */
    if (w->steps >= w->reader->index_count + 1u) {
        w->next_offset = 0;
        return WEFT_STUDIO_EBOUNDS;
    }
    w->steps++;
    rc = load_frame_hdr(w->reader->base, w->reader->size, off, &h);
    if (rc) {
        w->next_offset = 0;
        w->truncated = (rc == WEFT_STUDIO_ETRUNC) ? 1u : 0u;
        return rc;
    }
    {
        const uint8_t *stored = w->reader->base + off + 64u;
        if (weftrec_crc32c(stored, h.stored_size) != h.crc32c) {
            w->next_offset = 0;
            return WEFT_STUDIO_ECRC;
        }
        view->file_offset = off;
        view->payload_len = h.payload_size;
        view->hdr = (const weftrec_frame_header_t *)
                    (w->reader->base + off);
        if (h.codec == WEFTREC_CODEC_RAW) {
            view->payload = stored;          /* zero copy */
        } else {
            if (!w->scratch || w->scratch_cap < h.payload_size) {
                w->next_offset = 0;
                return WEFT_STUDIO_EBOUNDS;
            }
            rc = weftrec_dzv_decode(stored, h.stored_size, h.payload_size,
                                    w->scratch, w->scratch_cap);
            if (rc) {
                w->next_offset = 0;
                return rc;
            }
            view->payload = w->scratch;
        }
    }
    w->next_offset = h.next_offset;
    return WEFT_STUDIO_OK;
}

/* ------------------------------------------------------------------ */
/* Builder                                                             */
/* ------------------------------------------------------------------ */
int weftrec_builder_init(weftrec_builder_t *b, void *buf, uint64_t cap,
                         weftrec_index_entry_t *index_mem, uint32_t index_cap)
{
    if (!b || !buf || !index_mem) return WEFT_STUDIO_EBOUNDS;
    if (cap < 64u) return WEFT_STUDIO_EBOUNDS;
    if (((uintptr_t)buf & 63u) != 0u) return WEFT_STUDIO_EALIGN;
    if (((uintptr_t)index_mem & 7u) != 0u) return WEFT_STUDIO_EALIGN;
    b->base = (uint8_t *)buf;
    b->cap = cap;
    b->cursor = 64;
    b->last_frame_off = 0;
    b->index = index_mem;
    b->index_cap = index_cap;
    b->frame_count = 0;
    b->first_ts_ns = 0;
    b->last_ts_ns = 0;
    b->max_stream_id = 0;
    b->reserved0 = 0;
    b->finished = 0;
    memset(b->base, 0, 64); /* header placeholder */
    return WEFT_STUDIO_OK;
}

int weftrec_builder_append(weftrec_builder_t *b, uint64_t ts_ns,
                           uint32_t stream_id, const void *payload,
                           uint32_t payload_len, int codec)
{
    uint64_t frame_off;
    uint64_t need;
    uint32_t stored_len = 0;
    uint16_t used_codec = WEFTREC_CODEC_RAW;
    weftrec_frame_header_t h;
    weftrec_index_entry_t *ie;
    if (!b || b->finished) return WEFT_STUDIO_EBOUNDS;
    if (codec != WEFTREC_CODEC_RAW && codec != WEFTREC_CODEC_DZV)
        return WEFT_STUDIO_EPARSE;
    if (payload_len != 0 && !payload) return WEFT_STUDIO_EBOUNDS;
    if (b->frame_count != 0 && ts_ns < b->last_ts_ns)
        return WEFT_STUDIO_EPARSE;   /* append order is time order */
    if (b->frame_count >= b->index_cap) return WEFT_STUDIO_EBOUNDS;
    frame_off = align_up_64(b->cursor);
    need = frame_off + 64u + (uint64_t)payload_len + 64u;
    if (need > b->cap) return WEFT_STUDIO_EBOUNDS;

    if (codec == WEFTREC_CODEC_DZV && payload_len >= 8u) {
        /* in-buffer first pass: encode after the (reserved) header slot */
        uint32_t enc = weftrec_dzv_encode(b->base + frame_off + 64u,
                                          (uint32_t)(b->cap - frame_off - 64u),
                                          (const uint8_t *)payload,
                                          payload_len);
        if (enc != 0) {
            stored_len = enc;
            used_codec = WEFTREC_CODEC_DZV;
        }
    }
    if (used_codec == WEFTREC_CODEC_RAW) {
        if (payload_len)
            memcpy(b->base + frame_off + 64u, payload, payload_len);
        stored_len = payload_len;
    }

    /* frame header */
    memset(&h, 0, sizeof h);
    h.magic = WEFTREC1_MAGIC;
    h.timestamp_ns = ts_ns;
    h.stream_id = stream_id;
    h.frame_seq = b->frame_count;
    h.payload_size = payload_len;
    h.stored_size = stored_len;
    h.codec = used_codec;
    h.flags = 0;
    h.crc32c = weftrec_crc32c(b->base + frame_off + 64u, stored_len);
    h.next_offset = 0;                    /* patched by the next append */
    h.header_crc32c = 0;                  /* computed below (excl. itself) */
    h.reserved0 = 0;
    {
        uint8_t *p = b->base + frame_off;
        uint64_t end = align_up_64(frame_off + 64u + stored_len);
        if (end > b->cap) return WEFT_STUDIO_EBOUNDS;
        memcpy(p + 0, WEFTREC1_MAGIC_BYTES, 8);
        wr_le64w(p + 8, h.timestamp_ns);
        wr_le64w(p + 16, h.stream_id);
        wr_le64w(p + 24, h.frame_seq);
        wr_le32w(p + 32, h.payload_size);
        wr_le32w(p + 36, h.stored_size);
        wr_le16w(p + 40, h.codec);
        wr_le16w(p + 42, h.flags);
        wr_le32w(p + 44, h.crc32c);
        wr_le64w(p + 48, h.next_offset);
        wr_le32w(p + 60, h.reserved0);   /* full 64 B always written */
    }
    {
        uint8_t *p = b->base + frame_off;
        uint32_t hcrc = weftrec_crc32c(p, 48); /* excludes next_offset */
        wr_le32w(p + 56, hcrc);
        /* zero-pad to the next 64 B boundary */
        {
            uint64_t end = align_up_64(frame_off + 64u + stored_len);
            if (end > frame_off + 64u + stored_len)
                memset(b->base + frame_off + 64u + stored_len, 0,
                       (size_t)(end - frame_off - 64u - stored_len));
            b->cursor = end;
        }
    }
    /* patch the PREVIOUS frame's next_offset + index next_offset */
    if (b->last_frame_off != 0)
        wr_le64w(b->base + b->last_frame_off + 48, frame_off);
    if (b->frame_count > 0)
        wr_le64w((uint8_t *)&b->index[b->frame_count - 1u] + 16, frame_off);

    /* index entry */
    ie = &b->index[b->frame_count];
    {
        uint8_t *p = (uint8_t *)ie;
        memset(p, 0, 64);
        wr_le64w(p + 0, ts_ns);
        wr_le64w(p + 8, frame_off);
        wr_le64w(p + 16, 0);               /* patched by the next append */
        wr_le64w(p + 24, b->frame_count);
        wr_le64w(p + 32, stream_id);
        wr_le32w(p + 40, payload_len);
        wr_le32w(p + 44, 0);
        wr_le64w(p + 48, b->frame_count == 0 ? 0u
                : ts_ns - b->last_ts_ns);
        wr_le64w(p + 56, 0);
    }
    if (b->frame_count == 0) b->first_ts_ns = ts_ns;
    b->last_ts_ns = ts_ns;
    if ((uint64_t)stream_id > b->max_stream_id) b->max_stream_id = stream_id;
    b->last_frame_off = frame_off;
    b->frame_count++;
    return WEFT_STUDIO_OK;
}

int weftrec_builder_finish(weftrec_builder_t *b, uint64_t *out_len)
{
    uint64_t index_off;
    uint64_t total;
    uint32_t i;
    if (!b || b->finished) return WEFT_STUDIO_EBOUNDS;
    index_off = align_up_64(b->cursor);
    if (b->frame_count != 0) {
        if (index_off > b->cap ||
            (uint64_t)b->frame_count * 64u > b->cap - index_off)
            return WEFT_STUDIO_EBOUNDS;
        memcpy(b->base + index_off, b->index,
               (size_t)((uint64_t)b->frame_count * 64u));
    }
    total = align_up_64(index_off + (uint64_t)b->frame_count * 64u);
    if (total > b->cap) return WEFT_STUDIO_EBOUNDS;
    if (total > index_off + (uint64_t)b->frame_count * 64u)
        memset(b->base + index_off + (uint64_t)b->frame_count * 64u, 0,
               (size_t)(total - index_off - (uint64_t)b->frame_count * 64u));
    /* global header */
    {
        uint8_t *p = b->base;
        memcpy(p + 0, WEFTREC1_MAGIC_BYTES, 8);
        wr_le32w(p + 8, WEFTREC1_VERSION);
        wr_le32w(p + 12, WEFTREC_HF_INDEX);
        wr_le64w(p + 16, index_off);
        wr_le64w(p + 24, b->frame_count);
        wr_le64w(p + 32, b->frame_count);
        wr_le64w(p + 40, b->first_ts_ns);
        wr_le64w(p + 48, b->last_ts_ns);
        wr_le32w(p + 56, b->frame_count != 0
                ? (uint32_t)(b->max_stream_id + 1u) : 0u);
        wr_le32w(p + 60, weftrec_crc32c(p, 60));
    }
    b->finished = 1;
    if (out_len) *out_len = total;
    (void)i;
    return WEFT_STUDIO_OK;
}
