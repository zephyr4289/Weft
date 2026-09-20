// weft_flow.h — RFC 0017: zero-allocation declarative stream operators.
//
// map / filter / window / demux / zip over BORROWED views of ring payloads.
// Every buffer is caller-provided at setup; the hot path allocates nothing,
// spins on nothing, and reads no clock (t_ns is injected — Law 4
// determinism). Callbacks must be PURE (no allocator, no I/O, no retained
// state across calls beyond their own ctx) — that is the caller contract
// that keeps the pipeline Law-2 clean end to end.

#ifndef WEFT_FLOW_H
#define WEFT_FLOW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// The view (24 bytes on LP64 — pass by value, never owned)
// ---------------------------------------------------------------------------

typedef struct {
    const uint8_t* ptr;   // borrowed; valid until the source's next publish
    uint32_t len;
    uint32_t seq;
    uint64_t t_ns;
} weft_flow_view;

// ---------------------------------------------------------------------------
// map — transform into CALLER scratch; output refused WHOLE on overflow
// ---------------------------------------------------------------------------

/// Transform fn: write the transformed payload into dst (<= dst_cap) and
/// return its length. Return > dst_cap to signal overflow — the step is
/// refused whole (no partial frames, the TIER4 refusal precedent).
typedef size_t (*weft_flow_map_fn)(void* ctx, const weft_flow_view* src,
                                   uint8_t* dst, size_t dst_cap);

/// Returns 0 on success (out = mapped view, identity metadata carried),
/// -1 on overflow (refused whole), -1 with out untouched. Zero alloc.
int weft_flow_map(const weft_flow_view* src, weft_flow_view* out,
                  uint8_t* dst, size_t dst_cap,
                  weft_flow_map_fn fn, void* ctx);

// ---------------------------------------------------------------------------
// filter — pure predicate
// ---------------------------------------------------------------------------

typedef int (*weft_flow_filter_fn)(void* ctx, const weft_flow_view* v);

static inline int weft_flow_filter(weft_flow_filter_fn fn, void* ctx,
                                   const weft_flow_view* v) {
    return fn ? fn(ctx, v) : 1;  // NULL fn = pass-through
}

// ---------------------------------------------------------------------------
// window — tumbling or sliding over a caller-provided view ring
// ---------------------------------------------------------------------------

typedef struct {
    weft_flow_view* buf;    // caller storage, cap entries
    uint32_t cap;
    uint32_t count;         // views currently held
    uint32_t head_idx;      // sliding: oldest entry (ring)
    uint8_t  sliding;       // 0 = tumbling, 1 = sliding
    uint64_t t_emitted;     // windows emitted
    uint64_t t_views;       // views accepted
} weft_flow_window_t;

void weft_flow_window_init(weft_flow_window_t* w, weft_flow_view* buf,
                           uint32_t cap, int sliding);

/// Accept one view. Returns the number of views written to `out`
/// (tumbling: cap when the window closes, else 0; sliding:
/// min(count, cap) every step, oldest-first order). out_cap smaller than
/// the emission truncates the OLDEST entries (counted in t_overflow? no —
/// declared: callers size out_cap >= cap; a smaller out_cap is a caller
/// bug detected by assertion in tests, defined as truncation in release).
size_t weft_flow_window_step(weft_flow_window_t* w, const weft_flow_view* v,
                             weft_flow_view* out, size_t out_cap);

/// Drain a partial tumbling window (sliding: emits current contents).
/// Returns views written.
size_t weft_flow_window_flush(weft_flow_window_t* w, weft_flow_view* out,
                              size_t out_cap);

// ---------------------------------------------------------------------------
// demux — route to K caller sinks; full sink = lossy, counted (Law 1)
// ---------------------------------------------------------------------------

#define WEFT_FLOW_DEMUX_MAX 8u

typedef struct {
    weft_flow_view* buf;    // caller storage, cap entries
    uint32_t cap;
    uint32_t head;          // next write index (ring)
    uint32_t count;
    uint64_t t_overflow;    // views lost to a full sink — VISIBLE loss
} weft_flow_sink_t;

/// Route fn: return the sink index 0..K-1, or -1 to drop (a DECISION,
/// distinct from overflow — the same accounting split the governor draws).
typedef int (*weft_flow_route_fn)(void* ctx, const weft_flow_view* v);

/// Returns the sink index the view landed in, -1 if dropped by route.
/// (Overflow is lossy at the SINK level: counted in its t_overflow.)
int weft_flow_demux_step(weft_flow_sink_t* sinks, uint32_t n_sinks,
                         const weft_flow_view* v,
                         weft_flow_route_fn route, void* ctx);

/// Read up to out_cap views from a sink (FIFO), removing them.
/// Returns views read.
size_t weft_flow_sink_drain(weft_flow_sink_t* s, weft_flow_view* out,
                            size_t out_cap);

// ---------------------------------------------------------------------------
// zip — two streams, arrival-wise, timestamp tolerance
// ---------------------------------------------------------------------------

typedef struct {
    weft_flow_view held[2];   // at most ONE unpaired view per side
    uint8_t       has[2];     // [0] = A, [1] = B
    uint64_t      tolerance_ns;
    uint64_t      t_emit;
    uint64_t      t_coalesce; // stale unpaired views replaced (latest-wins)
    uint64_t      t_gap;      // tolerance violations (pair refused)
} weft_flow_zip_t;

void weft_flow_zip_init(weft_flow_zip_t* z, uint64_t tolerance_ns);

/// Offer a view from side `side` (0 = A, 1 = B). When a pair is formed
/// (both present within tolerance) writes both views to `out` (out[0]=A,
/// out[1]=B) and returns 1; returns 0 when the view is held; returns -1
/// when the tolerance fails (both views are DROPPED and t_gap++ — the
/// pair is refused whole; holding a stale half is the bug this refuses).
int weft_flow_zip_step(weft_flow_zip_t* z, uint32_t side,
                       const weft_flow_view* v, weft_flow_view out[2]);

// ---------------------------------------------------------------------------
// pipeline — filter -> map -> window, one step, per-stage telemetry
// ---------------------------------------------------------------------------

typedef struct {
    weft_flow_filter_fn  filter_fn;  void* filter_ctx;  // NULL = pass all
    weft_flow_map_fn     map_fn;     void* map_ctx;     // NULL = identity view
    uint8_t*             map_dst;    size_t map_cap;    // caller scratch
    size_t               map_ring;   // scratch RING size (>=1): step k maps
                                         // into buffer k % map_ring, so a
                                         // window following a map does not
                                         // alias the scratch it holds. 1 =
                                         // single buffer (fine without a
                                         // window; ALIASED with one).
    weft_flow_window_t*  window;                        // NULL = no windowing
    uint64_t t_in, t_filtered, t_mapped, t_windowed;
} weft_flow_pipeline_t;

/// One step: view in → optional filter (drop returns 0 views) → optional
/// map (into the pipeline's scratch; overflow refuses whole = 0 views,
/// counted) → optional window (emission written to `out`). Returns the
/// number of views written to `out` (0 or window emission size).
size_t weft_flow_pipeline_step(weft_flow_pipeline_t* p,
                               const weft_flow_view* v,
                               weft_flow_view* out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif  // WEFT_FLOW_H
