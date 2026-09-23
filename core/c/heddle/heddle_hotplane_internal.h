// heddle_hotplane_internal.h — engine-internal shared surface between
// heddle_hotplane.c and the WASM bridge test plane. NOT part of the
// public contract; do not consume outside core/c/heddle/ (+ core/wasm
// bridge, which re-derives offsets from the public header instead).
//
// Memory-ordering cheatsheet for this engine (normative, §4/§8):
//   producer begin : store begin_seq (relaxed)  ; fence release
//   producer commit: fence release              ; store commit_seq (relaxed)
//                    fetch_add epoch (acq_rel)  ; store heartbeat (relaxed)
//   reader open    : load commit_seq (acquire)  ; load begin_seq (acquire)
//                    -> equal = quiescent-consistent frame opened
//   reader close   : load begin_seq (acquire)   -> must still equal
//   All payload accesses are 8-aligned chunked __atomic relaxed
//   loads/stores (plain mov on x86-64; data-race-free, TSan-clean).
//   On x86-64/TSO every fence above compiles to a compiler-only barrier;
//   the sub-5ns budgets are met by the hardware for free.

#ifndef HEDDLE_HOTPLANE_INTERNAL_H
#define HEDDLE_HOTPLANE_INTERNAL_H

#include "heddle_hotplane.h"

/* ---- fixed-order atomic accessors (memory orders must be constants) --- */

static inline uint64_t hplane_ld_acq64(const volatile uint64_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline uint64_t hplane_ld_rel64(const volatile uint64_t *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}
static inline void hplane_st_rel64(volatile uint64_t *p, uint64_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}
static inline void hplane_fence_release(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}
static inline void hplane_fence_acquire(void)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}
static inline uint64_t hplane_fetch_or_acqrel64(volatile uint64_t *p,
                                                uint64_t bits)
{
    return __atomic_fetch_or(p, bits, __ATOMIC_ACQ_REL);
}
static inline uint64_t hplane_exchange_acqrel64(volatile uint64_t *p,
                                                uint64_t v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL);
}
static inline uint64_t hplane_fetch_add_acqrel64(volatile uint64_t *p,
                                                 uint64_t v)
{
    return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL);
}

/* ---- two-store register protocol (shared by plane, lane, slot pairs) ---
 * Writer is single-owner (enforced by session/role contracts).
 * Reader helpers: open returns the commit version when quiescent.      */

static inline int hplane_ts_open(const volatile uint64_t *begin,
                                 const volatile uint64_t *commit,
                                 uint64_t *commit_out)
{
    uint64_t c = hplane_ld_acq64(commit);
    uint64_t b = hplane_ld_acq64(begin);
    if (c != b) {
        return HEDDLE_E_SEQ_TORN;   /* session in flight — retry */
    }
    *commit_out = c;
    return HEDDLE_OK;
}

static inline int hplane_ts_close(const volatile uint64_t *begin,
                                  uint64_t commit_open)
{
    uint64_t b2 = hplane_ld_acq64(begin);
    return (b2 == commit_open) ? HEDDLE_OK : HEDDLE_E_SEQ_TORN;
}

/* ---- chunked payload copies (shared-side accesses are 8-aligned) ------- */

static inline void hplane_copy_from_shared(void *dst, const void *shared,
                                           uint32_t len)
{
    const uint8_t *s = (const uint8_t *)shared;
    uint8_t *d = (uint8_t *)dst;
    uint32_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t w = __atomic_load_n((const uint64_t *)(const void *)(s + i),
                                     __ATOMIC_RELAXED);
        __builtin_memcpy(d + i, &w, 8);
    }
    for (; i < len; i++) {
        d[i] = __atomic_load_n(s + i, __ATOMIC_RELAXED);
    }
}

static inline void hplane_copy_to_shared(void *shared, const void *src,
                                         uint32_t len)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)shared;
    uint32_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t w;
        __builtin_memcpy(&w, s + i, 8);
        __atomic_store_n((uint64_t *)(void *)(d + i), w, __ATOMIC_RELAXED);
    }
    for (; i < len; i++) {
        __atomic_store_n(d + i, s[i], __ATOMIC_RELAXED);
    }
}

/* ---- layout math (pure arithmetic; the bridge mirrors these) ---------- */

static inline hplane_header_t *hplane_hdr(const hplane_ctx_t *ctx)
{
    return (hplane_header_t *)(void *)ctx->mem;
}
static inline hplane_lane_desc_t *hplane_lane_desc(const hplane_ctx_t *ctx,
                                                   uint32_t lane)
{
    return (hplane_lane_desc_t *)(void *)(ctx->mem + WHP2_HEADER_SIZE +
                                          (size_t)lane * WHP2_LANE_DESC_SIZE);
}
static inline uint8_t *hplane_lane_data(const hplane_ctx_t *ctx,
                                        uint32_t lane)
{
    return ctx->mem + WHP2_HEADER_SIZE +
           (size_t)ctx->lane_count * WHP2_LANE_DESC_SIZE +
           (size_t)lane * ctx->lane_stride;
}
static inline uint8_t *hplane_slot_at(const hplane_ctx_t *ctx, uint32_t lane,
                                      uint32_t slot_idx)
{
    uint32_t stride = (ctx->mode == WHP2_MODE_RING)
                          ? hplane_slot_stride(ctx->sample_size)
                          : hplane_cell_stride(ctx->sample_size);
    return hplane_lane_data(ctx, lane) + (size_t)slot_idx * stride;
}
static inline uint64_t hplane_lane_bit(uint32_t lane)
{
    return 1ull << lane;
}
static inline uint64_t hplane_lane_count_mask(const hplane_ctx_t *ctx)
{
    return (ctx->lane_count >= 64) ? ~0ull
                                   : ((1ull << ctx->lane_count) - 1ull);
}

/* ---- stat keys (total-order mapping per kind; single-owner updates) --- */

static inline uint64_t hplane_stat_key(uint32_t kind, uint64_t raw)
{
    switch (kind) {
    case WHP2_STAT_I64:
        return raw ^ 0x8000000000000000ull;              /* signed order */
    case WHP2_STAT_F64:
        return (raw >> 63) ? ~raw : (raw | 0x8000000000000000ull);
    default:
        return raw;                                       /* WHP2_STAT_U64 */
    }
}

/* first min(8, len) bytes of a sample, LE-decoded, zero-padded */
static inline uint64_t hplane_stat_raw(const void *sample, uint32_t len)
{
    uint8_t tmp[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t n = (len < 8) ? len : 8;
    __builtin_memcpy(tmp, sample, n);
    return hplane_le64_get(tmp);
}

/* ---- internal shared validation ladder (used by create/attach) -------- */

int hplane_validate_ladder(void *mem, size_t len, hplane_cfg_t *cfg_out);

#endif /* HEDDLE_HOTPLANE_INTERNAL_H */
