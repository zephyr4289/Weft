// weft_cluster_frame.c — the WCF1 header (see weft_cluster_frame.h for
// the contract). The validator is the EXACT user-space mirror of the
// eBPF filter's fast-path decisions (backends/xdp) — same rungs, same
// order, same constants; the CL-X golden vectors drive BOTH so a drift
// between the two parsers is a gate failure, not a field incident.

#include "weft_cluster_frame.h"

#include <string.h>

// ---- frozen layout double-entry (FNV-1a 64) --------------------------------

static const char k_wcf1_layout[] =
    "wcf1|magic u8x4|version u16|hdr_len u16|cluster_id u32|schema_id u32"
    "|frame_seq u32|payload_len u32|timestamp_ns u64|src_node u16"
    "|dst_node u16|flags u32|wcr1_offset u64|reserved u8x16";

uint64_t weft_wcf1_schema_hash(void) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; k_wcf1_layout[i] != '\0'; i++) {
        h ^= (uint8_t)k_wcf1_layout[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

// ---- static layout law (Law 2) ----------------------------------------------

// Field offsets the eBPF immediates and the WRH1 handshake rely on.
_Static_assert(sizeof(weft_wcf1_t) == 64, "wcf1: header must be 64 bytes");
_Static_assert(offsetof(weft_wcf1_t, version)     == 4,  "wcf1: version@4");
_Static_assert(offsetof(weft_wcf1_t, cluster_id)  == 8,  "wcf1: cluster_id@8");
_Static_assert(offsetof(weft_wcf1_t, schema_id)   == 12, "wcf1: schema_id@12");
_Static_assert(offsetof(weft_wcf1_t, frame_seq)   == 16, "wcf1: frame_seq@16");
_Static_assert(offsetof(weft_wcf1_t, payload_len) == 20, "wcf1: payload_len@20");
_Static_assert(offsetof(weft_wcf1_t, timestamp_ns)== 24, "wcf1: timestamp_ns@24");
_Static_assert(offsetof(weft_wcf1_t, src_node)    == 32, "wcf1: src_node@32");
_Static_assert(offsetof(weft_wcf1_t, dst_node)    == 34, "wcf1: dst_node@34");
_Static_assert(offsetof(weft_wcf1_t, flags)       == 36, "wcf1: flags@36");
_Static_assert(offsetof(weft_wcf1_t, wcr1_offset) == 40, "wcf1: wcr1_offset@40");
_Static_assert(offsetof(weft_wcf1_t, reserved)    == 48, "wcf1: reserved@48");

const char* weft_wcf1_refusal_str(weft_wcf1_refusal_t r) {
    switch (r) {
        case WEFT_WCF1_OK:                 return "ok";
        case WEFT_WCF1_REFUSE_SHORT:       return "short";
        case WEFT_WCF1_REFUSE_MAGIC:       return "magic";
        case WEFT_WCF1_REFUSE_VERSION:     return "version";
        case WEFT_WCF1_REFUSE_HDRLEN:      return "hdr_len";
        case WEFT_WCF1_REFUSE_RESERVED:    return "reserved";
        case WEFT_WCF1_REFUSE_CLUSTER:     return "cluster_id";
        case WEFT_WCF1_REFUSE_SCHEMA:      return "schema_id";
        case WEFT_WCF1_REFUSE_LEN:         return "payload_len";
        case WEFT_WCF1_REFUSE_PLACEMENT:   return "placement";
    }
    return "?";
}

// ---- prepare / validate ------------------------------------------------------

void weft_wcf1_prepare(weft_wcf1_t* h, uint32_t cluster_id,
                       uint32_t schema_id, uint32_t frame_seq,
                       uint32_t payload_len, uint16_t src_node,
                       uint16_t dst_node, uint32_t flags,
                       uint64_t dst_wcr1_offset, uint64_t now_ns) {
    h->magic[0] = WEFT_WCF1_MAGIC0;
    h->magic[1] = WEFT_WCF1_MAGIC1;
    h->magic[2] = WEFT_WCF1_MAGIC2;
    h->magic[3] = WEFT_WCF1_MAGIC3;
    h->version      = WEFT_WCF1_VERSION;
    h->hdr_len      = WEFT_WCF1_HDR_BYTES;
    h->cluster_id   = cluster_id;
    h->schema_id    = schema_id;
    h->frame_seq    = frame_seq;
    h->payload_len  = payload_len;
    h->timestamp_ns = now_ns;
    h->src_node     = src_node;
    h->dst_node     = dst_node;
    h->flags        = flags;
    h->wcr1_offset  = dst_wcr1_offset;
    memset(h->reserved, 0, sizeof(h->reserved));
}

weft_wcf1_refusal_t weft_wcf1_validate(const void* buf, size_t avail,
                                       uint32_t cluster_filter,
                                       uint32_t schema_filter,
                                       uint32_t chunk_size,
                                       const weft_wcf1_t** out_hdr,
                                       const void** out_payload) {
    if (out_hdr) *out_hdr = NULL;
    if (out_payload) *out_payload = NULL;
    if (!buf || avail < WEFT_WCF1_HDR_BYTES) return WEFT_WCF1_REFUSE_SHORT;

    const weft_wcf1_t* h = (const weft_wcf1_t*)buf;
    if (h->magic[0] != WEFT_WCF1_MAGIC0 || h->magic[1] != WEFT_WCF1_MAGIC1 ||
        h->magic[2] != WEFT_WCF1_MAGIC2 || h->magic[3] != WEFT_WCF1_MAGIC3) {
        return WEFT_WCF1_REFUSE_MAGIC;
    }
    if (h->version != WEFT_WCF1_VERSION)   return WEFT_WCF1_REFUSE_VERSION;
    if (h->hdr_len != WEFT_WCF1_HDR_BYTES) return WEFT_WCF1_REFUSE_HDRLEN;

    // Unknown bits are a version violation on receive — refuse, never
    // guess (the WFSH attach discipline, applied to the wire).
    for (size_t i = 0; i < sizeof(h->reserved); i++) {
        if (h->reserved[i] != 0) return WEFT_WCF1_REFUSE_RESERVED;
    }

    // Steering (the XDP filter's job at line rate; mirrored here as
    // defense in depth for every non-XDP road).
    if (cluster_filter != 0 && h->cluster_id != cluster_filter) {
        return WEFT_WCF1_REFUSE_CLUSTER;
    }
    if (schema_filter != 0 && h->schema_id != schema_filter) {
        return WEFT_WCF1_REFUSE_SCHEMA;
    }

    // The fit law: a frame that does not fit a chunk is refused by name.
    if (h->payload_len > chunk_size - WEFT_WCF1_HDR_BYTES) {
        return WEFT_WCF1_REFUSE_LEN;
    }
    // The placement law: wcr1_offset must be a chunk boundary of the
    // local geometry (chunk0-relative). A mid-chunk landing would tear
    // Engineer 1's slot protocol — refuse.
    if (h->wcr1_offset % chunk_size != 0) {
        return WEFT_WCF1_REFUSE_PLACEMENT;
    }

    if (out_hdr) *out_hdr = h;
    if (out_payload) {
        *out_payload = (const uint8_t*)buf + WEFT_WCF1_HDR_BYTES;
    }
    return WEFT_WCF1_OK;
}
