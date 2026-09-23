// weft_loopback.h — RFC-0019 §6.4: the in-process loopback transport
// (the fabric's last rung — always available, honestly [FALLBACK-COPY]).
//
// WHY EXISTS: a cluster fabric needs a ground-truth road: CI runners
// without RDMA/XDP/io_uring, single-node dev, and the fabric cascade's
// terminal rung. The loopback engine moves WCF1 frames between two
// WCR1 regions IN-PROCESS with plain memcpy — one copy, zero syscalls,
// zero allocations, and every byte of the WCF1/geometry laws exercised
// exactly as on the fast roads. It is labeled [FALLBACK-COPY] wherever
// it reports itself: no road may launder a copy into a zero-copy claim.
//
// LAW 1: send/recv_batch touch only pre-allocated state (a bounded
//        seq FIFO of chunk indices) and the regions themselves.
// LAW 2: FIFO capacity is chunk_count-bounded; overflow is a named
//        BUSY refusal, never a drop, never a silent growth.
// LAW 4: every refusal is named; the stats carry the [FALLBACK-COPY]
//        label for evidence lines.

#ifndef WEFT_LOOPBACK_H
#define WEFT_LOOPBACK_H

#include <stddef.h>
#include <stdint.h>

#include "weft_wcr1.h"
#include "weft_cluster_core.h"
#include "weft_cluster_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t frames, bytes, refusals, overflow;
    int      copy_label;   ///< always 1: [FALLBACK-COPY], by construction
} weft_loopback_stats_t;

typedef struct {
    weft_wcr1_region_t src;    ///< borrowed producer region
    weft_wcr1_region_t dst;    ///< borrowed consumer region
    uint32_t* fifo;            ///< delivered chunk indices (setup alloc)
    uint32_t  fifo_head, fifo_tail, fifo_cap;
    uint32_t  cluster_id, schema_id;
    weft_loopback_stats_t stats;
    char      err[128];
} weft_loopback_ctx_t;

/// Wire two regions: frames sent from `src` land in `dst` chunks. Both
/// regions are validated; geometry mismatch (chunk_size) is a named
/// refusal (the loopback is a SIMULATOR of the wire, not a re-coder).
weft_cluster_status_t weft_loopback_init(weft_loopback_ctx_t* ctx,
                                         const weft_wcr1_region_t* src,
                                         const weft_wcr1_region_t* dst,
                                         uint32_t cluster_id,
                                         uint32_t schema_id);

/// Deliver ONE frame: validates the WCF1 header at src chunk
/// `src_chunk` (refusals named + counted), memcpy's header+payload to
/// dst chunk `dst_chunk`, pushes the delivery FIFO. Zero allocations.
weft_cluster_status_t weft_loopback_send(weft_loopback_ctx_t* ctx,
                                         uint32_t src_chunk,
                                         uint32_t dst_chunk);

/// Harvest delivered frames (caller storage): each entry points INTO
/// the dst region (valid until the next send to that chunk).
weft_cluster_status_t weft_loopback_recv_batch(weft_loopback_ctx_t* ctx,
                                               weft_cluster_rx_ent_t* out,
                                               uint32_t cap,
                                               uint32_t* out_n);

void weft_loopback_shutdown(weft_loopback_ctx_t* ctx);

const weft_loopback_stats_t* weft_loopback_stats(
    const weft_loopback_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif // WEFT_LOOPBACK_H
