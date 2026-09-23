// weft_memory_stream.c — incremental shadow planes + zero-copy frame
// staging. Implementation notes:
//   * Every cell update goes through mark_cell(): shadow write + optional
//     delta record + counters — one funnel, one budget discipline.
//   * WFRM classification per pass (absolute s, physical k = s & (M-1)):
//       A) s in [tail_cursor, tail)      -> READ
//       B) s in [head_cursor, head)      -> version odd ? WRITING : COMMITTED
//       C) k(head & mask)                -> version odd ? WRITING : CLAIMED
//     cursors clamp to one physical window (M) behind the frontiers so a
//     saturated stream coalesces old intermediate states (lag counted).
//   * WFSH: B) s in [head_cursor, latest) -> slotSeq 0 ? WRITING : COMMITTED
//     (FREE while latest == 0); no A (no ack plane), C is folded into B.
//   * The initial bind sweep classifies the whole plane once (O(cells)).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "weft_inspector/weft_memory_stream.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static weft_mstream_binding_t *bind_find(weft_mstream_ctx_t *ctx,
                                         int seg_idx) {
    for (unsigned i = 0; i < ctx->count; i++) {
        if (ctx->binds[i].seg_idx == seg_idx) return &ctx->binds[i];
    }
    return NULL;
}

static void delta_push(weft_mstream_ctx_t *ctx, unsigned ring_slot,
                       uint32_t cell, uint8_t state) {
    if (ctx->dcount < ctx->dcap) {
        weft_mstream_blob_delta_t *d = &ctx->deltas[ctx->dcount++];
        d->ring = (uint16_t)ring_slot;
        d->state = state;
        d->pad = 0u;
        d->cell = cell;
    } else {
        ctx->delta_overflow = 1u;   /* keyframe recovery, never silent */
    }
}

static void mark_cell(weft_mstream_ctx_t *ctx, weft_mstream_binding_t *b,
                      unsigned ring_slot, uint32_t k, uint8_t state) {
    if (b->plane[k] != state) {
        b->plane[k] = state;
        delta_push(ctx, ring_slot, k, state);
    }
    ctx->cells_marked++;
}

/* version word of physical slot k (WFRM) */
static const _Atomic uint64_t *wfrm_version(const weft_inspect_segment_t *s,
                                             uint32_t k) {
    return (const _Atomic uint64_t *)(const void *)(s->base +
            RMW_WEFT_RING_SLOTS_OFFSET + (size_t)k * s->slot_stride);
}

/* slotSeq word of physical slot k (WFSH) */
static const _Atomic uint64_t *wfsh_slotseq(const weft_inspect_segment_t *s,
                                            uint32_t k) {
    return (const _Atomic uint64_t *)(const void *)(s->base + 64u + 16u +
                                                    (size_t)k * 8u);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                            */
/* ------------------------------------------------------------------ */

int weft_mstream_init(weft_mstream_ctx_t *ctx, weft_inspect_ctx_t *insp,
                      weft_mstream_binding_t *binds, unsigned cap,
                      weft_mstream_blob_delta_t *deltas, unsigned dcap) {
    if (ctx == NULL || insp == NULL || binds == NULL || cap == 0u ||
        deltas == NULL || dcap == 0u) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->insp = insp;
    ctx->binds = binds;
    ctx->cap = cap;
    ctx->count = 0u;
    ctx->deltas = deltas;
    ctx->dcap = dcap;
    ctx->dcount = 0u;
    ctx->target_fps = 240.0;
    ctx->last_frame_ns = weft_inspect_now_ns();
    return WEFT_INSPECT_OK;
}

int weft_mstream_bind(weft_mstream_ctx_t *ctx, int seg_idx,
                      uint32_t rows, uint32_t cols,
                      weft_inspect_arena_t *plane_arena) {
    if (ctx == NULL || plane_arena == NULL) return WEFT_INSPECT_ERR_INVALID;
    if (seg_idx < 0 || (unsigned)seg_idx >= ctx->insp->count) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    if (bind_find(ctx, seg_idx) != NULL) return WEFT_INSPECT_OK;
    if (ctx->count >= ctx->cap) return WEFT_INSPECT_ERR_FULL;

    const weft_inspect_segment_t *s = &ctx->insp->segs[seg_idx];
    if (s->family != WEFT_INSPECT_FAMILY_RMW_RING &&
        s->family != WEFT_INSPECT_FAMILY_CLUSTER_RING) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    if (s->slot_count == 0u) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    uint32_t cells = s->slot_count;
    if (rows == 0u || cols == 0u) {
        rows = 1u;
        cols = cells;
    }
    if ((uint64_t)rows * (uint64_t)cols < (uint64_t)cells) {
        return WEFT_INSPECT_ERR_INVALID;
    }

    uint8_t *plane = (uint8_t *)weft_inspect_arena_alloc(plane_arena, cells);
    if (plane == NULL) return WEFT_INSPECT_ERR_FULL;

    weft_mstream_binding_t *b = &ctx->binds[ctx->count++];
    memset(b, 0, sizeof *b);
    b->seg_idx = seg_idx;
    b->plane = plane;
    b->cells = cells;
    b->rows = rows;
    b->cols = cols;
    b->family = s->family;
    memset(plane, WEFT_MSTREAM_CELL_FREE, cells);

    /* one-time full classification sweep: establishes the plane truth the
     * incremental passes then maintain. Cursor baselines follow. */
    if (s->family == WEFT_INSPECT_FAMILY_RMW_RING && s->attached) {
        uint64_t head = s->head, tail = s->tail_ack;
        for (uint32_t k = 0; k < cells; k++) {
            uint64_t v = atomic_load_explicit(wfrm_version(s, k),
                                              memory_order_acquire);
            uint8_t state;
            if ((v & 1u) != 0u) {
                state = WEFT_MSTREAM_CELL_WRITING;
            } else if (v == 0u) {
                state = WEFT_MSTREAM_CELL_FREE;
            } else {
                /* the cell's last-written absolute index (mod indexing:
                 * WFRM is power-of-two, WFSH may be any depth >= 2) */
                uint64_t sw = head - ((head - (uint64_t)k) % cells);
                if (k == (uint32_t)(head % cells)) {
                    state = WEFT_MSTREAM_CELL_CLAIMED;
                } else if (sw < tail) {
                    state = WEFT_MSTREAM_CELL_READ;
                } else {
                    state = WEFT_MSTREAM_CELL_COMMITTED;
                }
            }
            plane[k] = state;
        }
        b->tail_cursor = tail;
        b->head_cursor = head;
    } else if (s->family == WEFT_INSPECT_FAMILY_CLUSTER_RING && s->attached) {
        uint64_t latest = s->latest_seq;
        for (uint32_t k = 0; k < cells; k++) {
            uint64_t v = atomic_load_explicit(wfsh_slotseq(s, k),
                                              memory_order_acquire);
            plane[k] = (v == 0u)
                           ? ((latest == 0u) ? WEFT_MSTREAM_CELL_FREE
                                             : WEFT_MSTREAM_CELL_WRITING)
                           : WEFT_MSTREAM_CELL_COMMITTED;
        }
        b->tail_cursor = latest;
        b->head_cursor = latest;
    }
    b->last_dropped = s->dropped_total;
    ctx->cells_marked += cells;
    return WEFT_INSPECT_OK;
}

int weft_mstream_unbind(weft_mstream_ctx_t *ctx, int seg_idx) {
    if (ctx == NULL) return WEFT_INSPECT_ERR_INVALID;
    for (unsigned i = 0; i < ctx->count; i++) {
        if (ctx->binds[i].seg_idx == seg_idx) {
            ctx->binds[i] = ctx->binds[ctx->count - 1u];
            ctx->count--;
            return WEFT_INSPECT_OK;
        }
    }
    return WEFT_INSPECT_ERR_NOENT;
}

/* ------------------------------------------------------------------ */
/* scrape pass                                                          */
/* ------------------------------------------------------------------ */

int weft_mstream_scrape(weft_mstream_ctx_t *ctx, uint32_t max_cells) {
    if (ctx == NULL) return WEFT_INSPECT_ERR_INVALID;
    if (weft_inspect_scrape(ctx->insp) != WEFT_INSPECT_OK) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    uint32_t budget = (max_cells == 0u) ? 1024u : max_cells;

    for (unsigned bi = 0; bi < ctx->count; bi++) {
        weft_mstream_binding_t *b = &ctx->binds[bi];
        if (b->seg_idx < 0 || (unsigned)b->seg_idx >= ctx->insp->count) {
            continue;   /* segment detached underneath: next bind/unbind
                         * cycle repairs; scrape stays safe */
        }
        const weft_inspect_segment_t *s = &ctx->insp->segs[b->seg_idx];
        if (!s->attached || s->slot_count != b->cells) continue;

        if (b->family == WEFT_INSPECT_FAMILY_RMW_RING) {
            uint64_t head = s->head, tail = s->tail_ack;
            /* clamp cursors into the live physical window */
            if (head - b->head_cursor > (uint64_t)b->cells) {
                b->lag_cells += (head - b->head_cursor) - b->cells;
                b->head_cursor = head - b->cells;
            }
            if (tail > b->tail_cursor &&
                tail - b->tail_cursor > (uint64_t)b->cells) {
                b->tail_cursor = tail - b->cells;
            }
            if (b->tail_cursor > b->head_cursor) {
                b->head_cursor = b->tail_cursor;
            }

            /* A) READ marking: [tail_cursor, tail) */
            while (b->tail_cursor < tail && budget > 0u) {
                uint64_t sc = b->tail_cursor;
                mark_cell(ctx, b, bi, (uint32_t)(sc % b->cells),
                          WEFT_MSTREAM_CELL_READ);
                b->tail_cursor++;
                budget--;
            }
            /* B) COMMIT/WRITING marking: [head_cursor, head) */
            while (b->head_cursor < head && budget > 0u) {
                uint64_t sc = b->head_cursor;
                uint32_t k = (uint32_t)(sc % b->cells);
                uint64_t v = atomic_load_explicit(wfrm_version(s, k),
                                                  memory_order_acquire);
                mark_cell(ctx, b, bi, k,
                          ((v & 1u) != 0u) ? WEFT_MSTREAM_CELL_WRITING
                                           : WEFT_MSTREAM_CELL_COMMITTED);
                b->head_cursor++;
                budget--;
            }
            /* C) frontier cell: the next write target — CLAIMED by the
             * documented legend (a borrow leaves no shared trace until
             * the version flips; the frontier IS the claim), WRITING the
             * moment the seqlock opens. An odd version here means the
             * writer is mid-commit right now. */
            if (budget > 0u) {
                uint32_t k = (uint32_t)(head % b->cells);
                uint64_t v = atomic_load_explicit(wfrm_version(s, k),
                                                  memory_order_acquire);
                mark_cell(ctx, b, bi, k,
                          ((v & 1u) != 0u) ? WEFT_MSTREAM_CELL_WRITING
                                           : WEFT_MSTREAM_CELL_CLAIMED);
                budget--;
            }
        } else if (b->family == WEFT_INSPECT_FAMILY_CLUSTER_RING) {
            uint64_t latest = s->latest_seq;
            if (latest > b->head_cursor &&
                latest - b->head_cursor > (uint64_t)b->cells) {
                b->lag_cells += (latest - b->head_cursor) - b->cells;
                b->head_cursor = latest - b->cells;
            }
            /* B) frame commits: [head_cursor, latest) */
            while (b->head_cursor < latest && budget > 0u) {
                uint64_t sc = b->head_cursor;
                uint32_t k = (uint32_t)(sc % b->cells);
                uint64_t v = atomic_load_explicit(wfsh_slotseq(s, k),
                                                  memory_order_acquire);
                mark_cell(ctx, b, bi, k,
                          (v == 0u) ? WEFT_MSTREAM_CELL_WRITING
                                    : WEFT_MSTREAM_CELL_COMMITTED);
                b->head_cursor++;
                budget--;
            }
            /* frontier: the fill in progress (slotSeq 0 at latest&mask) */
            if (latest > 0u && budget > 0u) {
                uint32_t k = (uint32_t)(latest % b->cells);
                uint64_t v = atomic_load_explicit(wfsh_slotseq(s, k),
                                                  memory_order_acquire);
                if (v == 0u) {
                    mark_cell(ctx, b, bi, k, WEFT_MSTREAM_CELL_WRITING);
                }
                budget--;
            }
            b->tail_cursor = latest;
        }
    }
    ctx->scrape_passes++;
    return WEFT_INSPECT_OK;
}

/* ------------------------------------------------------------------ */
/* frame assembly                                                       */
/* ------------------------------------------------------------------ */

int weft_mstream_frame(weft_mstream_ctx_t *ctx,
                       weft_inspect_arena_t *blob_arena,
                       weft_mstream_frame_t *out) {
    if (ctx == NULL || blob_arena == NULL || out == NULL) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    int64_t now = weft_inspect_now_ns();
    unsigned rings = ctx->count;
    unsigned deltas = ctx->dcount;
    uint32_t kind = 0u;
    uint32_t flags = 0u;
    if (ctx->delta_overflow) {
        kind = 1u;                        /* keyframe recovery */
        flags |= WEFT_MSTREAM_FLAG_DELTA_OVERFLOW;
        deltas = 0u;                      /* planes are authoritative */
    }

    size_t blob_bytes = sizeof(weft_mstream_blob_hdr_t) +
                        (size_t)rings * sizeof(weft_mstream_blob_ring_t) +
                        (size_t)deltas * sizeof(weft_mstream_blob_delta_t);
    uint8_t *blob = (uint8_t *)weft_inspect_arena_alloc(blob_arena, blob_bytes);
    if (blob == NULL) return WEFT_INSPECT_ERR_FULL;

    uint32_t total_cells = 0u;
    for (unsigned i = 0; i < rings; i++) {
        const weft_mstream_binding_t *b = &ctx->binds[i];
        const weft_inspect_segment_t *s =
            (b->seg_idx >= 0 && (unsigned)b->seg_idx < ctx->insp->count)
                ? &ctx->insp->segs[b->seg_idx] : NULL;
        if (s != NULL && s->soft_snapshot) {
            flags |= WEFT_MSTREAM_FLAG_SOFT_SNAPSHOT;
        }

        weft_mstream_blob_ring_t *rb =
            (weft_mstream_blob_ring_t *)(blob +
                sizeof(weft_mstream_blob_hdr_t) +
                (size_t)i * sizeof(weft_mstream_blob_ring_t));
        memset(rb, 0, sizeof *rb);
        snprintf(rb->name, sizeof rb->name, "%.47s",
                 (s != NULL) ? s->name : "(detached)");
        rb->cells = b->cells;
        rb->rows = b->rows;
        rb->cols = b->cols;
        if (s != NULL && b->family == WEFT_INSPECT_FAMILY_RMW_RING) {
            rb->head = s->head;
            rb->tail = s->tail_ack;
            rb->published = s->published_total;
            rb->dropped = s->dropped_total;
            rb->in_flight = (s->head >= s->tail_ack)
                                ? (s->head - s->tail_ack) : 0u;
        } else if (s != NULL) {
            rb->head = s->latest_seq;
            rb->tail = s->latest_seq;
            rb->published = s->publishes;
            rb->dropped = 0u;
            rb->in_flight = 0u;
        }
        rb->scrape_seq = ctx->insp->scrape_passes;
        total_cells += b->cells;
    }

    if (deltas > 0u) {
        memcpy(blob + sizeof(weft_mstream_blob_hdr_t) +
                       (size_t)rings * sizeof(weft_mstream_blob_ring_t),
               ctx->deltas,
               (size_t)deltas * sizeof(weft_mstream_blob_delta_t));
    }

    weft_mstream_blob_hdr_t *hdr = (weft_mstream_blob_hdr_t *)(void *)blob;
    memset(hdr, 0, sizeof *hdr);
    hdr->magic = WEFT_MSTREAM_BLOB_MAGIC;
    hdr->version = WEFT_MSTREAM_BLOB_VERSION;
    hdr->kind = (uint16_t)kind;
    hdr->ts_ns = (uint64_t)now;
    hdr->frame_seq = ctx->frames_emitted + 1u;
    hdr->ring_count = rings;
    hdr->delta_count = deltas;
    hdr->total_cells = total_cells;
    hdr->flags = flags;

    memset(out, 0, sizeof *out);
    out->blob = blob;
    out->blob_bytes = blob_bytes;
    for (unsigned i = 0; i < rings && i < WEFT_MSTREAM_MAX_BINDS; i++) {
        out->planes[i] = ctx->binds[i].plane;      /* zero-copy export */
        out->plane_cells[i] = ctx->binds[i].cells;
        out->plane_rows[i] = ctx->binds[i].rows;
        out->plane_cols[i] = ctx->binds[i].cols;
    }
    out->ts_ns = (uint64_t)now;
    out->kind = kind;
    out->ring_count = rings;
    out->delta_count = deltas;
    out->total_cells = total_cells;
    out->flags = flags;
    out->next_deadline_ns = weft_mstream_next_deadline_ns(ctx);

    ctx->dcount = 0u;              /* delta list consumed atomically */
    ctx->delta_overflow = 0u;
    ctx->frames_emitted++;
    ctx->last_frame_ns = now;
    return WEFT_INSPECT_OK;
}

/* ------------------------------------------------------------------ */
/* pacer                                                                */
/* ------------------------------------------------------------------ */

int weft_mstream_set_fps(weft_mstream_ctx_t *ctx, double fps) {
    if (ctx == NULL || fps <= 0.0 || fps > 1000.0) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    ctx->target_fps = fps;
    return WEFT_INSPECT_OK;
}

uint64_t weft_mstream_next_deadline_ns(const weft_mstream_ctx_t *ctx) {
    if (ctx == NULL || ctx->target_fps <= 0.0) return 0u;
    double interval_ns = 1e9 / ctx->target_fps;
    uint64_t interval = (uint64_t)(interval_ns + 0.5);
    if (interval == 0u) interval = 1u;
    return (uint64_t)ctx->last_frame_ns + interval;
}
