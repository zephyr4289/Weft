// weft_sync.h — RFC 0018: multi-stream temporal synchronization (sensor
// fusion), C reference.
//
// N independent rings -> coherent timestamp-aligned tuples, with O(N)
// latest-wins state and ZERO allocation. See the RFC for the modeling
// contract (pivot policies, laggard drops, late/stale refusal).

#ifndef WEFT_SYNC_H
#define WEFT_SYNC_H

#include <stddef.h>
#include <stdint.h>

#include "weft_flow.h"  // weft_flow_view

#define WEFT_SYNC_MAX_STREAMS 8u

/// Pivot policies (closed set — a fourth is a new RFC).
typedef enum {
    WEFT_SYNC_PIVOT_MAX_TS  = 0,  // wait for the slowest domain (default)
    WEFT_SYNC_PIVOT_MIN_TS  = 1,  // emit at the fastest domain's cadence
    WEFT_SYNC_PIVOT_ANCHOR  = 2,  // one stream rules (video rules)
} weft_sync_pivot_t;

/// Verdicts from weft_sync_offer.
typedef enum {
    WEFT_SYNC_TUPLE   = 1,   // tuple emitted (out holds n_streams views)
    WEFT_SYNC_HELD    = 0,   // sample held, waiting for the other streams
    WEFT_SYNC_STALE   = -1,  // out-of-order arrival, refused (t_stale_drops)
    WEFT_SYNC_COALESCED = -2, // newer sample replaced an unconsumed older
                              // (t_coalesced) — caller may treat as HELD
    WEFT_SYNC_GAP     = -3,  // tolerance failed; laggard released (t_gap)
    WEFT_SYNC_INVALID = -4,  // bad stream index / args
} weft_sync_verdict_t;

typedef struct {
    weft_flow_view held;      // the ONE unconsumed sample (latest-wins)
    uint8_t        has;
    int64_t        tau;       // effective time of the held sample
} weft_sync_lane_t;

typedef struct {
    uint32_t         n_streams;
    uint64_t         tolerance_ns;
    int64_t          offset_ns[WEFT_SYNC_MAX_STREAMS];  // static calibration
    uint8_t          pivot_policy;
    uint8_t          anchor_stream;
    weft_sync_lane_t lane[WEFT_SYNC_MAX_STREAMS];
    uint64_t         t_tuples;
    uint64_t         t_gap;        // laggard releases
    uint64_t         t_stale;      // out-of-order refusals
    uint64_t         t_coalesced;  // unconsumed samples replaced
} weft_sync_t;

/// Init. n_streams in [1, 8]; pivot policy per the enum; anchor_stream
/// only meaningful under WEFT_SYNC_PIVOT_ANCHOR. Offsets default 0.
int weft_sync_init(weft_sync_t* sy, uint32_t n_streams,
                   uint64_t tolerance_ns, uint8_t pivot_policy,
                   uint8_t anchor_stream);

/// Set a stream's clock offset (calibration; τ = t_ns + offset).
void weft_sync_set_offset(weft_sync_t* sy, uint32_t stream, int64_t offset_ns);

/// Offer one sample. On WEFT_SYNC_TUPLE, out[0..n_streams) receives the
/// aligned views in stream order (ORIGINAL t_ns preserved — offsets
/// affect alignment, never the data). Zero allocation, O(N) work.
weft_sync_verdict_t weft_sync_offer(weft_sync_t* sy, uint32_t stream,
                                    const weft_flow_view* v,
                                    weft_flow_view* out);

#endif  // WEFT_SYNC_H
