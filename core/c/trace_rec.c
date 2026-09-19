// trace_rec.c — RFC 0014 .weftrec v4 kernel trace events, C reference codec.
// See trace_rec.h for the format contract.

#include "trace_rec.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// CRC-32/zlib — §1.4 contract (reflected 0xEDB88320, init/final 0xFFFFFFFF)
// ---------------------------------------------------------------------------

uint32_t weft_trace_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int kind_valid(uint16_t k) {
    return k >= WEFT_TRACE_PUBLISH && k <= WEFT_TRACE_CANARY_FAIL;
}

void weft_trace_event_pack(const weft_trace_event* e, uint8_t out[8]) {
    put16(out, e->kind);
    put16(out + 2, e->aux);
    put32(out + 4, e->data);
}

int weft_trace_event_unpack(const uint8_t in[8], weft_trace_event* e) {
    e->kind = get16(in);
    e->aux = get16(in + 2);
    e->data = get32(in + 4);
    if (!kind_valid(e->kind)) return -1;
    return 0;
}

// ---------------------------------------------------------------------------

size_t weft_trace_writer_needed(size_t max_events) {
    return WEFT_TRACE_HDR_SIZE + WEFT_TRACE_REC_SIZE * max_events;
}

void weft_trace_writer_open(weft_trace_writer* w, uint8_t* buf, size_t cap) {
    memset(w, 0, sizeof *w);
    w->buf = buf;
    w->cap = cap;
    if (cap < WEFT_TRACE_HDR_SIZE) { w->overflow = 1; return; }
    memcpy(buf, "WREC", 4);
    put16(buf + 4, WEFT_TRACE_VERSION);
    put16(buf + 6, WEFT_TRACE_HDR_SIZE);
    put32(buf + 8, WEFT_TRACE_FLAG);
    put32(buf + 12, 0);  // envelope_version = 0 (no payload frames in v4)
    put32(buf + 16, 0);  // event_count placeholder (patched at close)
    put32(buf + 20, 0);  // header crc placeholder (patched at close)
    memset(buf + 24, 0, 8);  // reserved
    w->off = WEFT_TRACE_HDR_SIZE;
}

int weft_trace_writer_event(weft_trace_writer* w, const weft_trace_event* e) {
    if (w->overflow) return -1;
    if (!kind_valid(e->kind)) return -1;
    if (w->off + WEFT_TRACE_REC_SIZE > w->cap) { w->overflow = 1; return -1; }
    uint8_t rec[WEFT_TRACE_REC_SIZE];
    weft_trace_event_pack(e, rec);
    uint32_t crc = weft_trace_crc32(rec, 8);
    put32(rec + 8, crc);
    memcpy(w->buf + w->off, rec, WEFT_TRACE_REC_SIZE);
    w->off += WEFT_TRACE_REC_SIZE;
    w->count++;
    return 0;
}

int weft_trace_writer_close(weft_trace_writer* w) {
    if (w->overflow) return -1;
    put32(w->buf + 16, w->count);
    put32(w->buf + 20, weft_trace_crc32(w->buf, 20));
    return 0;
}

// ---------------------------------------------------------------------------

int weft_trace_validate(const uint8_t* buf, size_t len, uint32_t* event_count) {
    if (len < WEFT_TRACE_HDR_SIZE) return -1;                 // short header
    if (memcmp(buf, "WREC", 4) != 0) return -1;               // magic
    if (get16(buf + 4) != WEFT_TRACE_VERSION) return -1;      // version gate
    if (get16(buf + 6) != WEFT_TRACE_HDR_SIZE) return -1;     // header_size
    if ((get32(buf + 8) & WEFT_TRACE_FLAG) == 0) return -1;   // TRACE flag
    if (get32(buf + 12) != 0) return -1;                      // no payload frames
    uint32_t crc_stored = get32(buf + 20);
    if (crc_stored != weft_trace_crc32(buf, 20)) return -1;   // header CRC
    uint32_t count = get32(buf + 16);
    if ((size_t)WEFT_TRACE_HDR_SIZE + (size_t)count * WEFT_TRACE_REC_SIZE != len)
        return -1;                                            // exact size
    if (event_count) *event_count = count;
    return 0;
}

int weft_trace_next(const uint8_t* buf, size_t len, size_t* cursor, weft_trace_event* e) {
    if (*cursor < WEFT_TRACE_HDR_SIZE || *cursor > len) return -1;
    if (*cursor == len) return 0;
    if (*cursor + WEFT_TRACE_REC_SIZE > len) return -1;
    const uint8_t* rec = buf + *cursor;
    if (get32(rec + 8) != weft_trace_crc32(rec, 8)) return -1;  // record CRC
    if (weft_trace_event_unpack(rec, e) != 0) return -1;        // kind gate
    *cursor += WEFT_TRACE_REC_SIZE;
    return 1;
}

// ---------------------------------------------------------------------------
// JSON-lines export — the human-readable contract validated against
// schemas/weftrec-trace.schema.json (trace-event objects + header).
// ---------------------------------------------------------------------------

static const char* kind_name(uint16_t k) {
    switch (k) {
        case WEFT_TRACE_PUBLISH:     return "publish";
        case WEFT_TRACE_CLAIM:       return "claim";
        case WEFT_TRACE_DROP:        return "drop";
        case WEFT_TRACE_REVOKE:      return "revoke";
        case WEFT_TRACE_ACK:         return "ack";
        case WEFT_TRACE_STALL:       return "stall";
        case WEFT_TRACE_TEAR:        return "tear";
        case WEFT_TRACE_CANARY_FAIL: return "canary_fail";
        default: return "unknown";
    }
}

size_t weft_trace_to_json(const uint8_t* buf, size_t len, uint8_t* out, size_t cap) {
    uint32_t count = 0;
    if (weft_trace_validate(buf, len, &count) != 0) return 0;

    size_t used = 0;
    // Bounded formatting without snprintf-per-byte games:
    // we format into out via a cursor and bounds-check every write.
    char tmp[256];
    int n = snprintf(tmp, sizeof tmp,
                     "{\"format\":\"weftrec\",\"version\":%u,\"kind\":\"trace\",\"events\":%u}\n",
                     WEFT_TRACE_VERSION, count);
    if (n < 0 || (size_t)n > cap - used) return 0;
    memcpy(out + used, tmp, (size_t)n); used += (size_t)n;

    size_t cur = WEFT_TRACE_HDR_SIZE;
    weft_trace_event e;
    uint32_t i = 0;
    int rc;
    while ((rc = weft_trace_next(buf, len, &cur, &e)) == 1) {
        n = snprintf(tmp, sizeof tmp,
                     "{\"i\":%u,\"kind\":\"%s\",\"aux\":%u,\"data\":%u}\n",
                     i++, kind_name(e.kind), e.aux, e.data);
        if (n < 0 || (size_t)n > cap - used) return 0;
        memcpy(out + used, tmp, (size_t)n); used += (size_t)n;
    }
    if (rc < 0) return 0;
    return used;
}
