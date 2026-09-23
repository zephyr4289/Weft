// weft_cluster_frame.h — RFC-0019 §2.2: the WCF1 cluster frame header
// (64 bytes, the cross-node wire + placement contract).
//
// WHY EXISTS: every Pillar-3 engine moves the SAME thing: one WCR1 chunk
// (or a slice of one) from node A to node B. RDMA writes raw bytes and
// the receiver's CPU never wakes (one-sided semantics — the NIC writes
// physical memory); XDP steers by header fields AT LINE RATE inside the
// NIC driver; io_uring batches whole datagrams. All three need to agree,
// BYTE-FROZEN, on what sits at the start of a chunk: who it is for
// (cluster_id), what it IS (schema_id — Pillar 1's dialect id), where it
// LANDS (wcr1_offset — the destination chunk offset inside the remote
// region), and whether the receiver should echo it back (bench ping).
// The XDP eBPF filter (backends/xdp) parses THIS layout at offsets
// encoded as immediates; weft_cluster_frame.c's user-space validator is
// its exact mirror — the CL-X gates cross-check both against golden
// vectors so the two parsers can never drift (the one-multiply
// bit-exactness discipline, applied to protocol parsing).
//
// LAYOUT (little-endian, 64 bytes, static-asserted):
//   offset  0  magic[4]    ".wft"                  0x7466772E LE
//   offset  4  version     u16 = 1
//   offset  6  hdr_len     u16 = 64
//   offset  8  cluster_id  u32                     steering key (XDP)
//   offset 12  schema_id   u32                     Pillar-1 dialect id
//   offset 16  frame_seq   u32                     per-sender counter
//   offset 20  payload_len u32                     bytes after header
//   offset 24  timestamp_ns u64                    sender CLOCK_MONOTONIC
//   offset 32  src_node    u16
//   offset 34  dst_node    u16
//   offset 36  flags       u32                     WEFT_WCF_F_*
//   offset 40  wcr1_offset u64                     dest chunk offset
//   offset 48  reserved[16]                        zero; unknown bits on
//                                                     receive = refuse
//
// PROOF DISCIPLINE: the layout string below is hashed (FNV-1a 64) at
// build time into WEFT_WCF1_SCHEMA_HASH and re-derived at runtime by
// weft_wcf1_schema_hash(); the CL-F gates assert the pair — a header
// edit that forgets to bump the frozen hash fails the gate, the same
// double-entry bookkeeping the GPU layout generators use.
//
// LAW 2: the layout is frozen; payload_len <= chunk_size - 64 is
//        enforced on BOTH ends (a frame that does not FIT a chunk is
//        refused by name, never truncated silently).
// LAW 4: reserved != 0 on receive is a named refusal, not a guess.

#ifndef WEFT_CLUSTER_FRAME_H
#define WEFT_CLUSTER_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "weft_wcr1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEFT_WCF1_MAGIC0 '.'   // 0x2E
#define WEFT_WCF1_MAGIC1 'w'   // 0x77
#define WEFT_WCF1_MAGIC2 'f'   // 0x66
#define WEFT_WCF1_MAGIC3 't'   // 0x74
#define WEFT_WCF1_VERSION  1u
#define WEFT_WCF1_HDR_BYTES 64u

/// Default cluster UDP port (io_uring fallback + XDP dport filter).
/// 47911 = 0xBB27 — registered nowhere; boring, unprivileged range.
#define WEFT_CLUSTER_UDP_PORT 47911u

/// Frame flags.
#define WEFT_WCF_F_NONE  0u
#define WEFT_WCF_F_ECHO  1u   ///< receiver echoes this frame back (bench)
#define WEFT_WCF_F_TS    2u   ///< timestamp_ns is meaningful (RTT calc)

/// FNV-1a 64 over the frozen layout string (double-entry with the
/// runtime re-derivation — see weft_cluster_frame.c).
#define WEFT_WCF1_SCHEMA_HASH UINT64_C(0x1db3fe15913e3993)

/// The 64-byte header (static asserts pin every field offset in .c).
typedef struct weft_wcf1 {
    uint8_t  magic[4];      ///< ".wft"
    uint16_t version;       ///< 1
    uint16_t hdr_len;       ///< 64
    uint32_t cluster_id;    ///< steering key
    uint32_t schema_id;     ///< payload dialect (Pillar 1 ids)
    uint32_t frame_seq;     ///< per-sender monotonic
    uint32_t payload_len;   ///< bytes following this header (<= chunk-64)
    uint64_t timestamp_ns;  ///< sender monotonic clock (F_TS)
    uint16_t src_node;
    uint16_t dst_node;
    uint32_t flags;         ///< WEFT_WCF_F_*
    uint64_t wcr1_offset;   ///< destination chunk offset in remote region
    uint8_t  reserved[16];  ///< zero on send; nonzero on recv = refuse
} weft_wcf1_t;

/// Named validation rungs (the mirror of the eBPF filter's decisions).
typedef enum {
    WEFT_WCF1_OK = 0,
    WEFT_WCF1_REFUSE_SHORT,       ///< fewer than 64 bytes present
    WEFT_WCF1_REFUSE_MAGIC,       ///< magic != ".wft"
    WEFT_WCF1_REFUSE_VERSION,     ///< version != 1
    WEFT_WCF1_REFUSE_HDRLEN,      ///< hdr_len != 64
    WEFT_WCF1_REFUSE_RESERVED,    ///< reserved != 0 (unknown bits)
    WEFT_WCF1_REFUSE_CLUSTER,     ///< cluster_id mismatch (filter)
    WEFT_WCF1_REFUSE_SCHEMA,      ///< schema_id mismatch (filter)
    WEFT_WCF1_REFUSE_LEN,         ///< payload_len > chunk_size - 64
    WEFT_WCF1_REFUSE_PLACEMENT,   ///< wcr1_offset not a chunk boundary
} weft_wcf1_refusal_t;

/// Fill a header (setup or hot-path — writes only the 64 bytes given).
void weft_wcf1_prepare(weft_wcf1_t* h, uint32_t cluster_id,
                       uint32_t schema_id, uint32_t frame_seq,
                       uint32_t payload_len, uint16_t src_node,
                       uint16_t dst_node, uint32_t flags,
                       uint64_t dst_wcr1_offset, uint64_t now_ns);

/// Validate a received frame header against a filter context:
///   cluster_filter: the local cluster id (0 = accept any — tests only)
///   schema_filter:  0 = accept any schema
///   chunk_size:     the LOCAL region chunk size (placement + len law)
/// `rungr` (may be NULL) receives the named refusal. Bounded: reads at
/// most `avail` bytes, never more.
weft_wcf1_refusal_t weft_wcf1_validate(const void* buf, size_t avail,
                                       uint32_t cluster_filter,
                                       uint32_t schema_filter,
                                       uint32_t chunk_size,
                                       const weft_wcf1_t** out_hdr,
                                       const void** out_payload);

/// Runtime FNV-1a 64 over the frozen layout descriptor (must equal
/// WEFT_WCF1_SCHEMA_HASH — asserted by the CL-F gates).
uint64_t weft_wcf1_schema_hash(void);

/// Refusal name for evidence lines ("ok"/"magic"/...).
const char* weft_wcf1_refusal_str(weft_wcf1_refusal_t r);

/// A received frame — the SHAPED record every transport road returns
/// (XDP, loopback, and the io_uring bench wrapper produce this same
/// shape; entries point INTO the destination region and stay valid
/// until the chunk is recycled).
typedef struct {
    uint32_t           chunk_idx;
    const weft_wcf1_t* hdr;
    const void*        payload;
    uint32_t           len;          ///< payload bytes
} weft_cluster_rx_ent_t;

#ifdef __cplusplus
}
#endif

#endif // WEFT_CLUSTER_FRAME_H
