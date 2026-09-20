// weft_flow.c — RFC 0017: zero-allocation declarative stream operators.
//
// Nothing in this file allocates, spins, or reads a clock. grep it: there
// is no malloc, no loop without a declared bound, no clock symbol. That is
// the deliverable — Law 2 as a reviewable property, not a promise.

#include "weft_flow.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// map
// ---------------------------------------------------------------------------

int weft_flow_map(const weft_flow_view* src, weft_flow_view* out,
                  uint8_t* dst, size_t dst_cap,
                  weft_flow_map_fn fn, void* ctx) {
    if (src == NULL || out == NULL || dst == NULL || fn == NULL) return -1;
    size_t n = fn(ctx, src, dst, dst_cap);
    if (n > dst_cap) return -1;  // refused whole (out untouched)
    out->ptr = dst;
    out->len = (uint32_t)n;
    out->seq = src->seq;      // identity metadata carries through — the
    out->t_ns = src->t_ns;    // mapped stream stays joinable by zip/sync
    return 0;
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------

void weft_flow_window_init(weft_flow_window_t* w, weft_flow_view* buf,
                           uint32_t cap, int sliding) {
    if (w == NULL) return;
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    w->sliding = (uint8_t)(sliding ? 1 : 0);
}

size_t weft_flow_window_step(weft_flow_window_t* w, const weft_flow_view* v,
                             weft_flow_view* out, size_t out_cap) {
    if (w == NULL || v == NULL || out == NULL || w->cap == 0) return 0;
    w->t_views++;
    size_t emitted = 0;
    if (w->sliding) {
        // ring: newest overwrites oldest when full (bounded state, Law 1)
        size_t n;
        uint32_t start;
        if (w->count < w->cap) {
            w->buf[w->count] = *v;
            w->count++;
            start = 0;
            n = w->count;
        } else {
            w->buf[w->head_idx] = *v;
            w->head_idx = (w->head_idx + 1) % w->cap;
            start = w->head_idx;  // post-increment: the new oldest
            n = w->cap;
        }
        if (out_cap < n) n = out_cap;  // declared: caller bug -> truncation
        for (size_t i = 0; i < n; i++) {
            out[i] = w->buf[(start + i) % w->cap];
        }
        w->t_emitted++;
        return n;
    }
    // tumbling
    if (w->count < w->cap) {
        w->buf[w->count++] = *v;
    }
    if (w->count == w->cap) {
        size_t n = (out_cap < w->cap) ? out_cap : w->cap;
        for (size_t i = 0; i < n; i++) out[i] = w->buf[i];
        w->count = 0;  // window closed and reset
        w->t_emitted++;
        emitted = n;
    }
    return emitted;
}

size_t weft_flow_window_flush(weft_flow_window_t* w, weft_flow_view* out,
                              size_t out_cap) {
    if (w == NULL || out == NULL) return 0;
    size_t n = (out_cap < w->count) ? out_cap : w->count;
    for (size_t i = 0; i < n; i++) out[i] = w->buf[i];
    w->count = 0;
    if (n) w->t_emitted++;
    return n;
}

// ---------------------------------------------------------------------------
// demux
// ---------------------------------------------------------------------------

int weft_flow_demux_step(weft_flow_sink_t* sinks, uint32_t n_sinks,
                         const weft_flow_view* v,
                         weft_flow_route_fn route, void* ctx) {
    if (sinks == NULL || v == NULL || n_sinks == 0 ||
        n_sinks > WEFT_FLOW_DEMUX_MAX) {
        return -1;
    }
    int r = route ? route(ctx, v) : 0;
    if (r < 0) return -1;              // dropped BY DECISION
    if ((uint32_t)r >= n_sinks) return -1;  // misdeclared route: refused
    weft_flow_sink_t* s = &sinks[r];
    if (s->count == s->cap) {
        s->t_overflow++;  // lossy, VISIBLE (Law 1: backpressure != stall)
        return r;
    }
    uint32_t idx = (s->head + s->count) % s->cap;
    s->buf[idx] = *v;
    s->count++;
    return r;
}

size_t weft_flow_sink_drain(weft_flow_sink_t* s, weft_flow_view* out,
                            size_t out_cap) {
    if (s == NULL || out == NULL) return 0;
    size_t n = (out_cap < s->count) ? out_cap : s->count;
    for (size_t i = 0; i < n; i++) {
        out[i] = s->buf[(s->head + i) % s->cap];
    }
    s->head = (s->head + (uint32_t)n) % (s->cap ? s->cap : 1);
    s->count -= (uint32_t)n;
    return n;
}

// ---------------------------------------------------------------------------
// zip
// ---------------------------------------------------------------------------

void weft_flow_zip_init(weft_flow_zip_t* z, uint64_t tolerance_ns) {
    if (z == NULL) return;
    memset(z, 0, sizeof(*z));
    z->tolerance_ns = tolerance_ns;
}

int weft_flow_zip_step(weft_flow_zip_t* z, uint32_t side,
                       const weft_flow_view* v, weft_flow_view out[2]) {
    if (z == NULL || v == NULL || out == NULL || side > 1) return 0;
    if (z->has[side]) {
        // latest-wins coalescing: the held view is stale by definition
        z->t_coalesce++;
        z->held[side] = *v;
        return 0;
    }
    z->held[side] = *v;
    z->has[side] = 1;
    if (!(z->has[0] && z->has[1])) return 0;

    uint64_t a = z->held[0].t_ns, b = z->held[1].t_ns;
    uint64_t spread = (a > b) ? (a - b) : (b - a);
    if (spread > z->tolerance_ns) {
        // the pair is refused WHOLE (rule: never emit a misaligned pair,
        // never hold a stale half hoping for a better match)
        z->has[0] = z->has[1] = 0;
        z->t_gap++;
        return -1;
    }
    out[0] = z->held[0];
    out[1] = z->held[1];
    z->has[0] = z->has[1] = 0;
    z->t_emit++;
    return 1;
}

// ---------------------------------------------------------------------------
// pipeline
// ---------------------------------------------------------------------------

size_t weft_flow_pipeline_step(weft_flow_pipeline_t* p,
                               const weft_flow_view* v,
                               weft_flow_view* out, size_t out_cap) {
    if (p == NULL || v == NULL || out == NULL) return 0;
    p->t_in++;
    if (p->filter_fn && !p->filter_fn(p->filter_ctx, v)) {
        p->t_filtered++;
        return 0;
    }
    weft_flow_view cur = *v;
    if (p->map_fn) {
        weft_flow_view mapped;
        size_t ring = p->map_ring ? p->map_ring : 1;
        uint8_t* d = p->map_dst + (p->t_in % ring) * p->map_cap;
        if (weft_flow_map(&cur, &mapped, d, p->map_cap,
                          p->map_fn, p->map_ctx) != 0) {
            p->t_filtered++;  // refused whole — same counter, same honesty
            return 0;
        }
        cur = mapped;
    }
    p->t_mapped++;
    if (p->window) {
        size_t n = weft_flow_window_step(p->window, &cur, out, out_cap);
        p->t_windowed += n;
        return n;
    }
    if (out_cap == 0) return 0;
    out[0] = cur;
    return 1;
}

#ifdef __cplusplus
}
#endif
