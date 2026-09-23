// heddle_hotplane.c — the WHP2 hot-plane engine (Pillar 4, heddle-2.0).
//
// Implementation pillars (HOTPLANE-LAYOUT-V2 normative):
//   * zero allocation anywhere (Law 1) — the engine owns no heap;
//   * two-store seqlock sessions on plane / lane / slot granularity;
//   * signal-driven dirty mask + per-lane bounding boxes;
//   * dual-mode data regions (ring slots / state cells);
//   * strict little-endian formatted fields, canary-gated hosts;
//   * bounded retries everywhere, unique named refusals (Law 4).

#include "heddle_hotplane_internal.h"

#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

const char *hplane_err_name(int err)
{
    switch (err) {
    case HEDDLE_OK:              return "HEDDLE_OK";
    case HEDDLE_E_ARG:           return "HEDDLE_E_ARG";
    case HEDDLE_E_MAGIC:         return "HEDDLE_E_MAGIC";
    case HEDDLE_E_VERSION:       return "HEDDLE_E_VERSION";
    case HEDDLE_E_LAYOUT:        return "HEDDLE_E_LAYOUT";
    case HEDDLE_E_ENDIAN:        return "HEDDLE_E_ENDIAN";
    case HEDDLE_E_CFG_CRC:       return "HEDDLE_E_CFG_CRC";
    case HEDDLE_E_REGION_SIZE:   return "HEDDLE_E_REGION_SIZE";
    case HEDDLE_E_CAPACITY:      return "HEDDLE_E_CAPACITY";
    case HEDDLE_E_LANE_OVERFLOW: return "HEDDLE_E_LANE_OVERFLOW";
    case HEDDLE_E_PAYLOAD:       return "HEDDLE_E_PAYLOAD";
    case HEDDLE_E_MODE:          return "HEDDLE_E_MODE";
    case HEDDLE_E_STATE:         return "HEDDLE_E_STATE";
    case HEDDLE_E_SEQ_TORN:      return "HEDDLE_E_SEQ_TORN";
    case HEDDLE_E_OVERRUN:       return "HEDDLE_E_OVERRUN";
    case HEDDLE_E_NOT_PUBLISHED: return "HEDDLE_E_NOT_PUBLISHED";
    case HEDDLE_E_UNSUPPORTED:   return "HEDDLE_E_UNSUPPORTED";
    default:                     return "HEDDLE_E_UNKNOWN";
    }
}

const char *hplane_mode_name(uint32_t mode)
{
    switch (mode) {
    case WHP2_MODE_RING:  return "ring";
    case WHP2_MODE_STATE: return "state";
    default:              return "invalid";
    }
}

const char *hplane_stat_kind_name(uint32_t kind)
{
    switch (kind) {
    case WHP2_STAT_U64: return "u64";
    case WHP2_STAT_I64: return "i64";
    case WHP2_STAT_F64: return "f64";
    default:            return "invalid";
    }
}

// ---------------------------------------------------------------------------
// CRC-32/IEEE (poly 0xEDB88320 reflected, init/xorout 0xFFFFFFFF).
// Check value: crc32("123456789") == 0xCBF43926 (unit-gated).
// Create/attach-time only — never on the hot path.
// ---------------------------------------------------------------------------

uint32_t hplane_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

uint32_t hplane_crc32(const void *data, size_t len)
{
    return hplane_crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Sizing
// ---------------------------------------------------------------------------

static int hplane_cfg_check(const hplane_cfg_t *cfg)
{
    if (!cfg) {
        return HEDDLE_E_ARG;
    }
    if (cfg->mode != WHP2_MODE_RING && cfg->mode != WHP2_MODE_STATE) {
        return HEDDLE_E_MODE;
    }
    if (cfg->lane_count == 0 || cfg->lane_count > WHP2_MAX_LANES) {
        return HEDDLE_E_CAPACITY;
    }
    if (cfg->sample_size == 0 || cfg->sample_size > WHP2_MAX_SAMPLE_SIZE) {
        return HEDDLE_E_PAYLOAD;
    }
    uint32_t min_cap =
        (cfg->mode == WHP2_MODE_RING) ? WHP2_MIN_CAPACITY : 1u;
    if (cfg->slot_capacity < min_cap ||
        cfg->slot_capacity > WHP2_MAX_CAPACITY) {
        return HEDDLE_E_CAPACITY;
    }
    if (cfg->stat_kind > WHP2_STAT_F64) {
        return HEDDLE_E_ARG;
    }
    if (cfg->flags & ~(uint32_t)(WHP2_F_MULTI_PRODUCER | WHP2_F_BBOX |
                                 WHP2_F_STATS)) {
        return HEDDLE_E_UNSUPPORTED;
    }
    return HEDDLE_OK;
}

uint64_t hplane_plane_size(const hplane_cfg_t *cfg)
{
    if (!cfg) {
        return 0;
    }
    uint32_t stride = (cfg->mode == WHP2_MODE_RING)
                          ? hplane_slot_stride(cfg->sample_size)
                          : hplane_cell_stride(cfg->sample_size);
    uint64_t lane_stride = (uint64_t)cfg->slot_capacity * stride;
    return (uint64_t)WHP2_HEADER_SIZE +
           (uint64_t)cfg->lane_count * WHP2_LANE_DESC_SIZE +
           (uint64_t)cfg->lane_count * lane_stride;
}

// ---------------------------------------------------------------------------
// Create
// ---------------------------------------------------------------------------

int hplane_plane_create(void *mem, size_t len, const hplane_cfg_t *cfg,
                        uint64_t create_stamp_ns)
{
    int rc = hplane_cfg_check(cfg);
    if (rc != HEDDLE_OK) {
        return rc;
    }
    if (!mem || len == 0) {
        return HEDDLE_E_ARG;
    }
    if (((uintptr_t)mem & 63u) != 0) {
        return HEDDLE_E_ARG;                 /* 64B-aligned region (Law 2) */
    }

    uint32_t stride = (cfg->mode == WHP2_MODE_RING)
                          ? hplane_slot_stride(cfg->sample_size)
                          : hplane_cell_stride(cfg->sample_size);
    uint64_t lane_stride = (uint64_t)cfg->slot_capacity * stride;
    if (lane_stride > WHP2_MAX_LANE_STRIDE) {
        return HEDDLE_E_CAPACITY;
    }
    uint64_t psize = hplane_plane_size(cfg);
    if ((uint64_t)len < psize) {
        return HEDDLE_E_REGION_SIZE;
    }

    /* Deterministic zeroing — create-time only, never the hot path. */
    memset(mem, 0, (size_t)psize);

    hplane_header_t *h = (hplane_header_t *)mem;

    /* Identity + static geometry — explicit LE formatting (Law 2). */
    hplane_le32_put(&h->magic, WHP2_MAGIC);
    hplane_le16_put(&h->ver_major, WHP2_LAYOUT_MAJOR);
    hplane_le16_put(&h->ver_minor, WHP2_LAYOUT_MINOR);
    hplane_le32_put(&h->header_size, WHP2_HEADER_SIZE);
    hplane_le32_put(&h->static_crc32, 0);            /* filled below */
    hplane_le32_put(&h->layout_rev, WHP2_LAYOUT_REV);
    hplane_le32_put(&h->mode, cfg->mode);
    hplane_le32_put(&h->lane_count, cfg->lane_count);
    hplane_le32_put(&h->lane_stride, (uint32_t)lane_stride);
    hplane_le32_put(&h->sample_size, cfg->sample_size);
    hplane_le32_put(&h->slot_capacity, cfg->slot_capacity);
    hplane_le64_put(&h->plane_size, psize);
    hplane_le64_put(&h->create_stamp_ns, create_stamp_ns);
    hplane_le32_put(&h->stat_kind, cfg->stat_kind);
    hplane_le32_put(&h->flags, cfg->flags);

    /* Synchronization plane — atomic registers start at zero (the LE
     * byte image of 0 is 0 on every host; the canary gates the rest). */
    hplane_st_rel64(&h->epoch, 0);
    hplane_st_rel64(&h->begin_seq, 0);
    hplane_st_rel64(&h->commit_seq, 0);
    hplane_st_rel64(&h->dirty_mask, 0);
    hplane_st_rel64(&h->render_frame_id, 0);
    hplane_st_rel64(&h->heartbeat_ns, 0);
    hplane_st_rel64(&h->dirty_transitions, 0);

    hplane_le32_put(&h->endian_canary, WHP2_ENDIAN_CANARY);
    hplane_le32_put(&h->reserved0, 0);

    /* Static config CRC over [0x10,0x40). */
    uint32_t crc = hplane_crc32((const uint8_t *)mem + WHP2_STATIC_CRC_COVER_OFF,
                                WHP2_STATIC_CRC_COVER_LEN);
    hplane_le32_put(&h->static_crc32, crc);

    /* Lane descriptors: no-data sentinel state (§3.2). */
    for (uint32_t l = 0; l < cfg->lane_count; l++) {
        hplane_lane_desc_t *d =
            (hplane_lane_desc_t *)(void *)((uint8_t *)mem + WHP2_HEADER_SIZE +
                                           (size_t)l * WHP2_LANE_DESC_SIZE);
        hplane_st_rel64(&d->lane_min, 0xFFFFFFFFFFFFFFFFull);
        hplane_st_rel64(&d->lane_max, 0);
        hplane_st_rel64(&d->lane_current, 0);
        hplane_st_rel64(&d->lane_begin_seq, 0);
        hplane_st_rel64(&d->lane_commit_seq, 0);
        hplane_st_rel64(&d->lane_commit_cnt, 0);
        hplane_st_rel64(&d->bbox_packed, WHP2_BBOX_EMPTY);
        __atomic_store_n(&d->lane_flags, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&d->lane_misc, 0u, __ATOMIC_RELAXED);
    }

    return HEDDLE_OK;
}

// ---------------------------------------------------------------------------
// Validation ladder (shared by attach and validate)
// ---------------------------------------------------------------------------

int hplane_validate_ladder(void *mem, size_t len, hplane_cfg_t *cfg_out)
{
    if (!mem || len < WHP2_HEADER_SIZE) {
        return HEDDLE_E_ARG;
    }
    if (((uintptr_t)mem & 63u) != 0) {
        return HEDDLE_E_ARG;
    }

    hplane_header_t *h = (hplane_header_t *)mem;

    if (hplane_le32_get(&h->magic) != WHP2_MAGIC) {
        return HEDDLE_E_MAGIC;
    }
    if (hplane_le16_get(&h->ver_major) != WHP2_LAYOUT_MAJOR) {
        return HEDDLE_E_VERSION;
    }
    if (hplane_le32_get(&h->header_size) != WHP2_HEADER_SIZE ||
        hplane_le32_get(&h->layout_rev) != WHP2_LAYOUT_REV) {
        return HEDDLE_E_LAYOUT;
    }
    if (hplane_le32_get(&h->endian_canary) != WHP2_ENDIAN_CANARY) {
        return HEDDLE_E_ENDIAN;
    }

    uint32_t crc = hplane_crc32((const uint8_t *)mem + WHP2_STATIC_CRC_COVER_OFF,
                                WHP2_STATIC_CRC_COVER_LEN);
    if (crc != hplane_le32_get(&h->static_crc32)) {
        return HEDDLE_E_CFG_CRC;
    }

    hplane_cfg_t cfg;
    cfg.mode          = hplane_le32_get(&h->mode);
    cfg.lane_count    = hplane_le32_get(&h->lane_count);
    cfg.sample_size   = hplane_le32_get(&h->sample_size);
    cfg.slot_capacity = hplane_le32_get(&h->slot_capacity);
    cfg.stat_kind     = hplane_le32_get(&h->stat_kind);
    cfg.flags         = hplane_le32_get(&h->flags);

    int rc = hplane_cfg_check(&cfg);
    if (rc != HEDDLE_OK) {
        return rc;
    }

    /* Geometry must be exactly the deterministic derivation. */
    uint32_t stride = (cfg.mode == WHP2_MODE_RING)
                          ? hplane_slot_stride(cfg.sample_size)
                          : hplane_cell_stride(cfg.sample_size);
    uint64_t lane_stride = (uint64_t)cfg.slot_capacity * stride;
    uint32_t lane_stride_hdr = hplane_le32_get(&h->lane_stride);
    if (lane_stride > WHP2_MAX_LANE_STRIDE ||
        (uint64_t)lane_stride_hdr != lane_stride ||
        (lane_stride_hdr & 63u) != 0) {
        return HEDDLE_E_LAYOUT;
    }
    uint64_t psize = hplane_plane_size(&cfg);
    if (hplane_le64_get(&h->plane_size) != psize) {
        return HEDDLE_E_LAYOUT;
    }
    if ((uint64_t)len < psize) {
        return HEDDLE_E_REGION_SIZE;
    }

    if (cfg_out) {
        *cfg_out = cfg;
    }
    return HEDDLE_OK;
}

int hplane_validate(void *mem, size_t len)
{
    return hplane_validate_ladder(mem, len, NULL);
}

int hplane_attach(void *mem, size_t len, hplane_ctx_t *ctx, uint32_t role)
{
    if (!ctx) {
        return HEDDLE_E_ARG;
    }
    if (role != WHP2_ROLE_PRODUCER && role != WHP2_ROLE_CONSUMER &&
        role != WHP2_ROLE_BOTH) {
        return HEDDLE_E_ARG;
    }

    hplane_cfg_t cfg;
    int rc = hplane_validate_ladder(mem, len, &cfg);
    if (rc != HEDDLE_OK) {
        return rc;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->mem            = (uint8_t *)mem;
    ctx->mem_size       = (uint64_t)len;
    ctx->mode           = cfg.mode;
    ctx->lane_count     = cfg.lane_count;
    ctx->lane_stride    = hplane_le32_get(&((hplane_header_t *)mem)->lane_stride);
    ctx->sample_size    = cfg.sample_size;
    ctx->slot_capacity  = cfg.slot_capacity;
    ctx->stat_kind      = cfg.stat_kind;
    ctx->flags          = cfg.flags;
    ctx->role           = role;
    ctx->last_commit    = hplane_ld_acq64(&((hplane_header_t *)mem)->commit_seq);
    return HEDDLE_OK;
}

// ---------------------------------------------------------------------------
// Internal: session bookkeeping on lane descriptors
// ---------------------------------------------------------------------------

/* Open the lane's two-store pair with session id v (owner thread only). */
static inline void hplane_lane_pair_open(hplane_lane_desc_t *d, uint64_t v)
{
    hplane_st_rel64(&d->lane_begin_seq, v);
    hplane_fence_release();
}

/* Close the lane's two-store pair (owner thread only). The commit
 * counter is incremented BEFORE the COMMIT store so any reader that
 * brackets a stable pair also observes the post-session counter. */
static inline void hplane_lane_pair_close(hplane_lane_desc_t *d, uint64_t v)
{
    hplane_fetch_add_acqrel64(&d->lane_commit_cnt, 1);
    hplane_fence_release();
    hplane_st_rel64(&d->lane_commit_seq, v);
}

/* Owner-side stats maintenance (raw bit keys, §5). Seeds on the
 * no-data sentinel signature (min==UINT64_MAX && max==0), which no
 * post-seed state can reproduce because min<=max is an invariant. */
static void hplane_stats_update(hplane_lane_desc_t *d, uint32_t kind,
                                uint64_t raw)
{
    uint64_t min_r = hplane_ld_rel64(&d->lane_min);
    uint64_t max_r = hplane_ld_rel64(&d->lane_max);
    if (min_r == 0xFFFFFFFFFFFFFFFFull && max_r == 0) {
        hplane_st_rel64(&d->lane_min, raw);
        hplane_st_rel64(&d->lane_max, raw);
        return;
    }
    uint64_t k = hplane_stat_key(kind, raw);
    if (k < hplane_stat_key(kind, min_r)) {
        hplane_st_rel64(&d->lane_min, raw);
    }
    if (k > hplane_stat_key(kind, max_r)) {
        hplane_st_rel64(&d->lane_max, raw);
    }
}

/* Owner-side bounding-box expansion (idx < 2^32, §6). */
static void hplane_bbox_expand(hplane_lane_desc_t *d, uint32_t idx)
{
    uint64_t cur = hplane_ld_rel64(&d->bbox_packed);
    if (cur == WHP2_BBOX_EMPTY) {
        hplane_st_rel64(&d->bbox_packed,
                        ((uint64_t)idx << 32) | (uint64_t)idx);
        return;
    }
    uint32_t mn = (uint32_t)(cur >> 32);
    uint32_t mx = (uint32_t)cur;
    if (idx < mn) {
        mn = idx;
    }
    if (idx > mx) {
        mx = idx;
    }
    hplane_st_rel64(&d->bbox_packed, ((uint64_t)mn << 32) | mx);
}

/* Session-start bbox reset (§6): the bbox register is PRODUCER-ONLY —
 * the consumer never writes it. At each session start the owner checks
 * whether the consumer's frame has advanced since the last reset; if
 * so, the delivered range is cleared and this session's writes re-seed
 * it. Called INSIDE the open session bracket, so consumers bracketing
 * on the lane pair never observe the reset window. */
static void hplane_bbox_reset_if_framed(hplane_ctx_t *p,
                                        hplane_lane_desc_t *d)
{
    hplane_header_t *h = hplane_hdr(p);
    uint64_t f = hplane_ld_acq64(&h->render_frame_id);
    if (f != p->frame_seen) {
        hplane_st_rel64(&d->bbox_packed, WHP2_BBOX_EMPTY);
        p->frame_seen = f;
    }
}

/* Flush a batched dirty mask set into the active mask, counting the
 * 0->1 transitions into dirty_transitions (the lossless-harvest
 * ground truth: transitions == harvested popcount + final popcount). */
static inline void hplane_dirty_flush(hplane_header_t *h, uint64_t bits)
{
    if (!bits) {
        return;
    }
    uint64_t old = hplane_fetch_or_acqrel64(&h->dirty_mask, bits);
    uint64_t risen = bits & ~old;
    if (risen) {
        hplane_fetch_add_acqrel64(&h->dirty_transitions,
                                  (uint64_t)__builtin_popcountll(risen));
    }
}

/* Resolve the governing session for a mutation on `lane`.
 * Single-producer mode auto-opens the lane pair inside the global
 * session (touched-lane bookkeeping); multi mode requires the lane
 * session to be the open one. Returns the session id or an error. */
static int hplane_mutation_session(hplane_ctx_t *p, uint32_t lane,
                                   uint64_t *v_out,
                                   hplane_lane_desc_t **d_out)
{
    if (!(p->role & WHP2_ROLE_PRODUCER)) {
        return HEDDLE_E_STATE;
    }
    if (p->flags & WHP2_F_MULTI_PRODUCER) {
        if (p->lane_session == 0 || p->lane_open != lane) {
            return HEDDLE_E_STATE;     /* open the lane session first */
        }
        *v_out = p->lane_session;
        *d_out = hplane_lane_desc(p, lane);
        return HEDDLE_OK;
    }
    if (p->session == 0) {
        return HEDDLE_E_STATE;         /* open a global session first */
    }
    uint64_t bit = hplane_lane_bit(lane);
    if (!(p->touched_lanes & bit)) {
        hplane_lane_desc_t *d = hplane_lane_desc(p, lane);
        hplane_lane_pair_open(d, p->session);
        hplane_bbox_reset_if_framed(p, d);
        p->touched_lanes |= bit;
    }
    *v_out = p->session;
    *d_out = hplane_lane_desc(p, lane);
    return HEDDLE_OK;
}

// ---------------------------------------------------------------------------
// Producer: single-producer global sessions
// ---------------------------------------------------------------------------

int hplane_commit_begin(hplane_ctx_t *p)
{
    if (!p) {
        return HEDDLE_E_ARG;
    }
    if (p->flags & WHP2_F_MULTI_PRODUCER) {
        return HEDDLE_E_MODE;          /* multi planes use lane sessions */
    }
    if (!(p->role & WHP2_ROLE_PRODUCER)) {
        return HEDDLE_E_STATE;
    }
    if (p->session != 0) {
        return HEDDLE_E_STATE;         /* double open */
    }
    hplane_header_t *h = hplane_hdr(p);
    uint64_t v = p->last_commit + 1;   /* owner cache: zero shared loads */
    hplane_st_rel64(&h->begin_seq, v); /* store #1 — session enters flight */
    hplane_fence_release();
    p->session       = v;
    p->dirty_pending = 0;
    p->touched_lanes = 0;
    return HEDDLE_OK;
}

int hplane_commit_end(hplane_ctx_t *p)
{
    if (!p) {
        return HEDDLE_E_ARG;
    }
    if (p->flags & WHP2_F_MULTI_PRODUCER) {
        return HEDDLE_E_MODE;
    }
    if (p->session == 0) {
        return HEDDLE_E_STATE;         /* no open session */
    }
    hplane_header_t *h = hplane_hdr(p);
    uint64_t v = p->session;

    /* Close every touched lane pair: lane data becomes read-consistent
     * BEFORE the plane-level signals below reference it. */
    uint64_t t = p->touched_lanes;
    while (t) {
        uint32_t lane = (uint32_t)__builtin_ctzll(t);
        t &= t - 1;
        hplane_lane_pair_close(hplane_lane_desc(p, lane), v);
    }

    /* Flush the batched dirty mask: a harvester that observes a bit is
     * guaranteed the lane's pair is already closed (payload committed). */
    hplane_dirty_flush(h, p->dirty_pending);

    /* The plane COMMIT — final store of the session. No RMW on this
     * path: the epoch is derived from commit_seq by readers (the two
     * coincide for single-producer planes); multi-producer planes
     * fetch_add the epoch register in lane_end instead. */
    hplane_fence_release();
    hplane_st_rel64(&h->commit_seq, v);

    p->session       = 0;
    p->last_commit   = v;
    p->dirty_pending = 0;
    p->touched_lanes = 0;
    return HEDDLE_OK;
}

// ---------------------------------------------------------------------------
// Producer: multi-producer per-lane sessions
// ---------------------------------------------------------------------------

int hplane_lane_begin(hplane_ctx_t *p, uint32_t lane)
{
    if (!p) {
        return HEDDLE_E_ARG;
    }
    if (!(p->flags & WHP2_F_MULTI_PRODUCER)) {
        return HEDDLE_E_MODE;          /* single planes use global sessions */
    }
    if (!(p->role & WHP2_ROLE_PRODUCER)) {
        return HEDDLE_E_STATE;
    }
    if (p->lane_session != 0) {
        return HEDDLE_E_STATE;         /* close the open lane session first */
    }
    if (lane >= p->lane_count) {
        return HEDDLE_E_LANE_OVERFLOW;
    }
    hplane_lane_desc_t *d = hplane_lane_desc(p, lane);
    uint64_t c = hplane_ld_acq64(&d->lane_commit_seq);
    uint64_t b = hplane_ld_acq64(&d->lane_begin_seq);
    if (c != b) {
        return HEDDLE_E_STATE;         /* foreign producer in flight:
                                          lane-ownership contract breach */
    }
    uint64_t v = c + 1;
    hplane_lane_pair_open(d, v);
    hplane_bbox_reset_if_framed(p, d);
    __atomic_or_fetch(&d->lane_flags, WHP2_LANE_F_ACTIVE, __ATOMIC_RELAXED);
    p->lane_session   = v;
    p->lane_open      = lane;
    p->dirty_pending  = 0;
    return HEDDLE_OK;
}

int hplane_lane_end(hplane_ctx_t *p, uint32_t lane)
{
    if (!p) {
        return HEDDLE_E_ARG;
    }
    if (!(p->flags & WHP2_F_MULTI_PRODUCER)) {
        return HEDDLE_E_MODE;
    }
    if (p->lane_session == 0 || p->lane_open != lane) {
        return HEDDLE_E_STATE;
    }
    hplane_header_t *h = hplane_hdr(p);
    hplane_lane_desc_t *d = hplane_lane_desc(p, lane);
    uint64_t v = p->lane_session;

    hplane_dirty_flush(h, p->dirty_pending);
    hplane_lane_pair_close(d, v);
    hplane_fetch_add_acqrel64(&h->epoch, 1);

    p->lane_session  = 0;
    p->lane_open     = 0;
    p->dirty_pending = 0;
    return HEDDLE_OK;
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

int hplane_state_write(hplane_ctx_t *p, uint32_t lane, uint32_t cell,
                       const void *val, uint32_t len)
{
    if (!p || !val) {
        return HEDDLE_E_ARG;
    }
    if (p->mode != WHP2_MODE_STATE) {
        return HEDDLE_E_MODE;
    }
    if (lane >= p->lane_count) {
        return HEDDLE_E_LANE_OVERFLOW;
    }
    if (len == 0 || len > p->sample_size) {
        return HEDDLE_E_PAYLOAD;
    }
    if (cell >= p->slot_capacity) {
        return HEDDLE_E_CAPACITY;
    }

    uint64_t v;
    hplane_lane_desc_t *d;
    int rc = hplane_mutation_session(p, lane, &v, &d);
    if (rc != HEDDLE_OK) {
        return rc;
    }

    uint8_t *cellp = hplane_slot_at(p, lane, cell);   /* state: cell_stride */
    hplane_copy_to_shared(cellp, val, len);

    uint64_t raw = hplane_stat_raw(val, len);
    hplane_stats_update(d, p->stat_kind, raw);
    hplane_st_rel64(&d->lane_current, raw);           /* latest sample bits */
    hplane_bbox_expand(d, cell);
    p->dirty_pending |= hplane_lane_bit(lane);
    return HEDDLE_OK;
}

int hplane_ring_push(hplane_ctx_t *p, uint32_t lane,
                     const void *sample, uint32_t len)
{
    if (!p || !sample) {
        return HEDDLE_E_ARG;
    }
    if (p->mode != WHP2_MODE_RING) {
        return HEDDLE_E_MODE;
    }
    if (lane >= p->lane_count) {
        return HEDDLE_E_LANE_OVERFLOW;
    }
    if (len == 0 || len > p->sample_size) {
        return HEDDLE_E_PAYLOAD;
    }

    uint64_t v;
    hplane_lane_desc_t *d;
    int rc = hplane_mutation_session(p, lane, &v, &d);
    if (rc != HEDDLE_OK) {
        return rc;
    }

    uint64_t n = hplane_ld_rel64(&d->lane_current) + 1;   /* push ordinal */
    uint32_t slot_idx = (uint32_t)((n - 1) % p->slot_capacity);
    uint8_t *slot = hplane_slot_at(p, lane, slot_idx);
    volatile uint64_t *s_begin = (volatile uint64_t *)(void *)slot;
    volatile uint64_t *s_commit =
        (volatile uint64_t *)(void *)(slot + WHP2_SLOT_HEADER_SIZE - 8);

    /* Slot two-store: ordinal enters flight, payload, ordinal COMMIT. */
    hplane_st_rel64(s_begin, n);
    hplane_fence_release();
    hplane_copy_to_shared(slot + WHP2_SLOT_HEADER_SIZE, sample, len);
    hplane_fence_release();
    hplane_st_rel64(s_commit, n);

    /* Head advance = the ring gate (release: every ordinal < n is
     * committed payload; readers acquire-load the head then slots). */
    __atomic_store_n(&d->lane_current, n, __ATOMIC_RELEASE);

    /* Owner-side signals (fenced by the session pair at close). */
    uint64_t raw = hplane_stat_raw(sample, len);
    hplane_stats_update(d, p->stat_kind, raw);
    hplane_bbox_expand(d, slot_idx);
    p->dirty_pending |= hplane_lane_bit(lane);

    /* In-band backpressure: once the producer has completed at least
     * one full cycle it is overwriting unconsumed ordinals — mark the
     * lane and count completed cycles. The producer NEVER blocks. */
    if (n > (uint64_t)p->slot_capacity) {
        __atomic_or_fetch(&d->lane_flags,
                          WHP2_LANE_F_BP_MARK | WHP2_LANE_F_OVERRUN,
                          __ATOMIC_RELAXED);
        if (slot_idx == 0) {                    /* cycle boundary */
            uint32_t misc = __atomic_load_n(&d->lane_misc, __ATOMIC_RELAXED);
            uint32_t cnt = (misc & 0xFFFFu) + 1u;
            __atomic_store_n(&d->lane_misc,
                             (misc & 0xFFFF0000u) | (cnt & 0xFFFFu),
                             __ATOMIC_RELAXED);
        }
    }
    return HEDDLE_OK;
}

// ---------------------------------------------------------------------------
// Consumers (lock-free reads)
// ---------------------------------------------------------------------------

int hplane_cell_read(hplane_ctx_t *c, uint32_t lane, uint32_t cell,
                     void *out, uint32_t len, uint32_t max_retries)
{
    if (!c || !out) {
        return HEDDLE_E_ARG;
    }
    if (c->mode != WHP2_MODE_STATE) {
        return HEDDLE_E_MODE;
    }
    if (lane >= c->lane_count) {
        return HEDDLE_E_LANE_OVERFLOW;
    }
    if (len == 0 || len > c->sample_size) {
        return HEDDLE_E_PAYLOAD;
    }
    if (cell >= c->slot_capacity) {
        return HEDDLE_E_CAPACITY;
    }
    hplane_lane_desc_t *d = hplane_lane_desc(c, lane);
    uint8_t *cellp = hplane_slot_at(c, lane, cell);

    for (uint32_t r = 0; r <= max_retries; r++) {
        uint64_t open;
        if (hplane_ts_open(&d->lane_begin_seq, &d->lane_commit_seq, &open) !=
            HEDDLE_OK) {
            continue;                          /* session in flight */
        }
        hplane_copy_from_shared(out, cellp, len);
        if (hplane_ts_close(&d->lane_begin_seq, open) == HEDDLE_OK) {
            return HEDDLE_OK;
        }
    }
    return HEDDLE_E_SEQ_TORN;
}

int hplane_slot_read(hplane_ctx_t *c, uint32_t lane, uint64_t seq,
                     void *out, uint32_t len, uint32_t max_retries)
{
    if (!c || !out) {
        return HEDDLE_E_ARG;
    }
    if (c->mode != WHP2_MODE_RING) {
        return HEDDLE_E_MODE;
    }
    if (lane >= c->lane_count) {
        return HEDDLE_E_LANE_OVERFLOW;
    }
    if (len == 0 || len > c->sample_size) {
        return HEDDLE_E_PAYLOAD;
    }
    if (seq == 0) {
        return HEDDLE_E_ARG;                    /* ordinals are 1-based */
    }

    uint32_t slot_idx = (uint32_t)((seq - 1) % c->slot_capacity);
    uint8_t *slot = hplane_slot_at(c, lane, slot_idx);
    volatile uint64_t *s_begin = (volatile uint64_t *)(void *)slot;
    volatile uint64_t *s_commit =
        (volatile uint64_t *)(void *)(slot + WHP2_SLOT_HEADER_SIZE - 8);

    for (uint32_t r = 0; r <= max_retries; r++) {
        uint64_t cm = hplane_ld_acq64(s_commit);
        uint64_t bn = hplane_ld_acq64(s_begin);
        if (cm != bn) {
            continue;                           /* slot being (re)written */
        }
        if (cm == 0) {
            return HEDDLE_E_STATE;              /* slot never written */
        }
        if (cm != seq) {
            return (cm > seq) ? HEDDLE_E_OVERRUN : HEDDLE_E_NOT_PUBLISHED;
        }
        hplane_copy_from_shared(out, slot + WHP2_SLOT_HEADER_SIZE, len);
        if (hplane_ld_acq64(s_begin) == cm) {
            return HEDDLE_OK;
        }
    }
    return HEDDLE_E_SEQ_TORN;
}

int hplane_ring_head(const hplane_ctx_t *c, uint32_t lane, uint64_t *head_out)
{
    if (!c || !head_out) {
        return HEDDLE_E_ARG;
    }
    if (lane >= c->lane_count) {
        return HEDDLE_E_LANE_OVERFLOW;
    }
    if (c->mode != WHP2_MODE_RING) {
        return HEDDLE_E_MODE;
    }
    *head_out = hplane_ld_acq64(&hplane_lane_desc(c, lane)->lane_current);
    return HEDDLE_OK;
}

int hplane_lane_snapshot(hplane_ctx_t *c, uint32_t lane,
                         void *buf, size_t buf_len, uint32_t max_retries)
{
    if (!c || !buf) {
        return HEDDLE_E_ARG;
    }
    if (c->mode != WHP2_MODE_STATE) {
        return HEDDLE_E_MODE;
    }
    if (lane >= c->lane_count) {
        return HEDDLE_E_LANE_OVERFLOW;
    }
    size_t need = (size_t)c->slot_capacity * hplane_cell_stride(c->sample_size);
    if (buf_len < need) {
        return HEDDLE_E_REGION_SIZE;
    }
    hplane_lane_desc_t *d = hplane_lane_desc(c, lane);
    uint8_t *data = hplane_lane_data(c, lane);

    for (uint32_t r = 0; r <= max_retries; r++) {
        uint64_t open;
        if (hplane_ts_open(&d->lane_begin_seq, &d->lane_commit_seq, &open) !=
            HEDDLE_OK) {
            continue;
        }
        hplane_copy_from_shared(buf, data, (uint32_t)need);
        if (hplane_ts_close(&d->lane_begin_seq, open) == HEDDLE_OK) {
            return HEDDLE_OK;
        }
    }
    return HEDDLE_E_SEQ_TORN;
}

int hplane_lane_stats_get(hplane_ctx_t *c, uint32_t lane,
                          hplane_lane_stats_t *out, uint32_t max_retries)
{
    if (!c || !out) {
        return HEDDLE_E_ARG;
    }
    if (lane >= c->lane_count) {
        return HEDDLE_E_LANE_OVERFLOW;
    }
    hplane_lane_desc_t *d = hplane_lane_desc(c, lane);

    for (uint32_t r = 0; r <= max_retries; r++) {
        uint64_t open;
        if (hplane_ts_open(&d->lane_begin_seq, &d->lane_commit_seq, &open) !=
            HEDDLE_OK) {
            continue;
        }
        out->min_raw      = hplane_ld_rel64(&d->lane_min);
        out->max_raw      = hplane_ld_rel64(&d->lane_max);
        out->current_raw  = hplane_ld_rel64(&d->lane_current);
        out->commit_count = hplane_ld_rel64(&d->lane_commit_cnt);
        if (hplane_ts_close(&d->lane_begin_seq, open) == HEDDLE_OK) {
            return HEDDLE_OK;
        }
    }
    return HEDDLE_E_SEQ_TORN;
}

// ---------------------------------------------------------------------------
// Dirty plane / frame protocol
// ---------------------------------------------------------------------------

int hplane_dirty_harvest(hplane_ctx_t *c, uint64_t *mask_out)
{
    if (!c || !mask_out) {
        return HEDDLE_E_ARG;
    }
    if (!(c->role & WHP2_ROLE_CONSUMER)) {
        return HEDDLE_E_STATE;
    }
    *mask_out = hplane_exchange_acqrel64(&hplane_hdr(c)->dirty_mask, 0);
    return HEDDLE_OK;
}

int hplane_dirty_remark(hplane_ctx_t *c, uint64_t bits)
{
    if (!c) {
        return HEDDLE_E_ARG;
    }
    if (!(c->role & WHP2_ROLE_CONSUMER)) {
        return HEDDLE_E_STATE;
    }
    if (bits & ~hplane_lane_count_mask(c)) {
        return HEDDLE_E_LANE_OVERFLOW;
    }
    hplane_dirty_flush(hplane_hdr(c), bits);
    return HEDDLE_OK;
}

uint64_t hplane_dirty_transitions_get(const hplane_ctx_t *c)
{
    if (!c) {
        return 0;
    }
    return hplane_ld_acq64(&hplane_hdr(c)->dirty_transitions);
}

int hplane_bbox_harvest(hplane_ctx_t *c, uint32_t lane, uint64_t *bbox_out)
{
    if (!c || !bbox_out) {
        return HEDDLE_E_ARG;
    }
    if (lane >= c->lane_count) {
        return HEDDLE_E_LANE_OVERFLOW;
    }
    if (!(c->role & WHP2_ROLE_CONSUMER)) {
        return HEDDLE_E_STATE;
    }
    hplane_lane_desc_t *d = hplane_lane_desc(c, lane);
    for (uint32_t r = 0; r <= WHP2_DEFAULT_READ_RETRIES; r++) {
        uint64_t open;
        if (hplane_ts_open(&d->lane_begin_seq, &d->lane_commit_seq, &open) !=
            HEDDLE_OK) {
            continue;
        }
        uint64_t bbox = hplane_ld_rel64(&d->bbox_packed);
        if (hplane_ts_close(&d->lane_begin_seq, open) == HEDDLE_OK) {
            *bbox_out = bbox;
            return HEDDLE_OK;
        }
    }
    return HEDDLE_E_SEQ_TORN;
}

int hplane_frame_commit(hplane_ctx_t *c, uint64_t *frame_id_out)
{
    if (!c || !frame_id_out) {
        return HEDDLE_E_ARG;
    }
    if (!(c->role & WHP2_ROLE_CONSUMER)) {
        return HEDDLE_E_STATE;
    }
    *frame_id_out =
        hplane_fetch_add_acqrel64(&hplane_hdr(c)->render_frame_id, 1) + 1;
    return HEDDLE_OK;
}

uint64_t hplane_epoch_get(const hplane_ctx_t *c)
{
    if (!c) {
        return 0;
    }
    /* Single-producer planes: the epoch IS the committed session id
     * (kept RMW-free on the commit path). Multi-producer planes
     * maintain the aggregate register via fetch-add. */
    if (c->flags & WHP2_F_MULTI_PRODUCER) {
        return hplane_ld_acq64(&hplane_hdr(c)->epoch);
    }
    return hplane_ld_acq64(&hplane_hdr(c)->commit_seq);
}

uint64_t hplane_frame_get(const hplane_ctx_t *c)
{
    if (!c) {
        return 0;
    }
    return hplane_ld_acq64(&hplane_hdr(c)->render_frame_id);
}

uint64_t hplane_heartbeat_get(const hplane_ctx_t *c)
{
    if (!c) {
        return 0;
    }
    return hplane_ld_rel64(&hplane_hdr(c)->heartbeat_ns);
}

int hplane_heartbeat_touch(hplane_ctx_t *p)
{
    if (!p) {
        return HEDDLE_E_ARG;
    }
    if (!(p->role & WHP2_ROLE_PRODUCER)) {
        return HEDDLE_E_STATE;
    }
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return HEDDLE_E_STATE;
    }
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    hplane_st_rel64(&hplane_hdr(p)->heartbeat_ns, ns);
    return HEDDLE_OK;
}
