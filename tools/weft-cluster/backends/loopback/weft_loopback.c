// weft_loopback.c — the ground-truth road (see weft_loopback.h).

#include "weft_loopback.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// weft_xdp_rx_ent_t is shared as the cluster "received frame" record
// (the loopback and XDP roads produce the same shape — the bench treats
// them identically).

weft_cluster_status_t weft_loopback_init(weft_loopback_ctx_t* ctx,
                                         const weft_wcr1_region_t* src,
                                         const weft_wcr1_region_t* dst,
                                         uint32_t cluster_id,
                                         uint32_t schema_id) {
    if (!ctx || !src || !dst) return WEFT_CLUSTER_E_INVALID_ARG;
    memset(ctx, 0, sizeof(*ctx));
    char why[128];
    if (weft_wcr1_validate(src, why, sizeof(why)) != WEFT_WCR1_OK ||
        weft_wcr1_validate(dst, why, sizeof(why)) != WEFT_WCR1_OK) {
        snprintf(ctx->err, sizeof(ctx->err), "loopback: %.110s", why);
        return WEFT_CLUSTER_E_INVALID_ARG;
    }
    if (src->chunk_size != dst->chunk_size) {
        snprintf(ctx->err, sizeof(ctx->err),
                 "loopback: chunk_size mismatch %u vs %u (the loopback "
                 "simulates the wire, it does not re-code it)",
                 src->chunk_size, dst->chunk_size);
        return WEFT_CLUSTER_E_INVALID_ARG;
    }
    ctx->src = *src;
    ctx->dst = *dst;
    ctx->cluster_id = cluster_id;
    ctx->schema_id = schema_id;
    ctx->fifo_cap = dst->chunk_count;
    ctx->fifo = (uint32_t*)malloc(sizeof(uint32_t) * ctx->fifo_cap);
    if (!ctx->fifo) return WEFT_CLUSTER_E_NO_MEMORY;
    ctx->stats.copy_label = 1;  // [FALLBACK-COPY] by construction
    return WEFT_CLUSTER_OK;
}

weft_cluster_status_t weft_loopback_send(weft_loopback_ctx_t* ctx,
                                         uint32_t src_chunk,
                                         uint32_t dst_chunk) {
    if (!ctx || !ctx->fifo) return WEFT_CLUSTER_E_STATE;
    if (src_chunk >= ctx->src.chunk_count ||
        dst_chunk >= ctx->dst.chunk_count) {
        snprintf(ctx->err, sizeof(ctx->err),
                 "loopback: chunk out of range (%u/%u)", src_chunk,
                 dst_chunk);
        ctx->stats.refusals++;
        return WEFT_CLUSTER_E_INVALID_ARG;
    }

    // validate exactly like the wire roads (the simulator must refuse
    // what the NIC road would refuse)
    const weft_wcf1_t* hdr = (const weft_wcf1_t*)weft_wcr1_chunk(
        &ctx->src, src_chunk);
    const weft_wcf1_refusal_t rr = weft_wcf1_validate(
        hdr, ctx->src.chunk_size, ctx->cluster_id, ctx->schema_id,
        ctx->src.chunk_size, NULL, NULL);
    if (rr != WEFT_WCF1_OK) {
        snprintf(ctx->err, sizeof(ctx->err),
                 "loopback: WCF1 refused (%s) at src chunk %u",
                 weft_wcf1_refusal_str(rr), src_chunk);
        ctx->stats.refusals++;
        return WEFT_CLUSTER_E_INVALID_ARG;
    }

    if (ctx->fifo_tail - ctx->fifo_head >= ctx->fifo_cap) {
        // bounded FIFO: the consumer did not keep up — BUSY, not a drop
        ctx->stats.overflow++;
        return WEFT_CLUSTER_E_BUSY;
    }

    uint8_t* d = weft_wcr1_chunk(&ctx->dst, dst_chunk);
    const uint8_t* s = (const uint8_t*)hdr;
    const uint32_t total = WEFT_WCF1_HDR_BYTES + hdr->payload_len;
    memcpy(d, s, total);  // the [FALLBACK-COPY] line — labeled, counted
    ctx->fifo[ctx->fifo_tail % ctx->fifo_cap] = dst_chunk;
    ctx->fifo_tail++;
    ctx->stats.frames++;
    ctx->stats.bytes += total;
    return WEFT_CLUSTER_OK;
}

weft_cluster_status_t weft_loopback_recv_batch(weft_loopback_ctx_t* ctx,
                                               weft_cluster_rx_ent_t* out,
                                               uint32_t cap,
                                               uint32_t* out_n) {
    if (!ctx || !out || !out_n) return WEFT_CLUSTER_E_INVALID_ARG;
    if (!ctx->fifo) return WEFT_CLUSTER_E_STATE;
    *out_n = 0;
    while (ctx->fifo_head != ctx->fifo_tail && *out_n < cap) {
        const uint32_t chunk = ctx->fifo[ctx->fifo_head % ctx->fifo_cap];
        ctx->fifo_head++;
        const weft_wcf1_t* hdr =
            (const weft_wcf1_t*)weft_wcr1_chunk(&ctx->dst, chunk);
        out[*out_n].chunk_idx = chunk;
        out[*out_n].hdr = hdr;
        out[*out_n].payload = (const uint8_t*)hdr + WEFT_WCF1_HDR_BYTES;
        out[*out_n].len = hdr->payload_len;
        (*out_n)++;
    }
    return WEFT_CLUSTER_OK;
}

void weft_loopback_shutdown(weft_loopback_ctx_t* ctx) {
    if (!ctx) return;
    free(ctx->fifo);
    memset(ctx, 0, sizeof(*ctx));
}

const weft_loopback_stats_t* weft_loopback_stats(
    const weft_loopback_ctx_t* ctx) {
    return ctx ? &ctx->stats : NULL;
}
