// weft_sync.c — RFC 0018: multi-stream temporal synchronizer, C reference.
//
// offer() is one compare + one store + one spread check per stream. There
// is no queue, no lock, no allocator, no clock read (t_ns is injected;
// offsets are static per bind). The laggard drop is the entire
// backpressure story: the oldest held sample is released and counted.

#include "weft_sync.h"

#include <string.h>

static int64_t effective_tau(const weft_sync_t* sy, uint32_t stream,
                             uint64_t t_ns) {
    // saturating add — a wild offset must not wrap into a "valid" past
    int64_t t = (int64_t)t_ns;
    int64_t off = sy->offset_ns[stream];
    if (off > 0 && t > INT64_MAX - off) return INT64_MAX;
    if (off < 0 && t < INT64_MIN - off) return INT64_MIN;
    return t + off;
}

int weft_sync_init(weft_sync_t* sy, uint32_t n_streams,
                   uint64_t tolerance_ns, uint8_t pivot_policy,
                   uint8_t anchor_stream) {
    if (sy == NULL || n_streams == 0 || n_streams > WEFT_SYNC_MAX_STREAMS) {
        return -1;
    }
    if (pivot_policy > WEFT_SYNC_PIVOT_ANCHOR) return -1;
    if (pivot_policy == WEFT_SYNC_PIVOT_ANCHOR &&
        anchor_stream >= n_streams) return -1;
    memset(sy, 0, sizeof(*sy));
    sy->n_streams = n_streams;
    sy->tolerance_ns = tolerance_ns;
    sy->pivot_policy = pivot_policy;
    sy->anchor_stream = anchor_stream;
    return 0;
}

void weft_sync_set_offset(weft_sync_t* sy, uint32_t stream, int64_t offset_ns) {
    if (sy == NULL || stream >= sy->n_streams) return;
    sy->offset_ns[stream] = offset_ns;
    // calibration rebind: the HELD sample's effective time moves with the
    // new offset (otherwise a mid-stream recalibration would compare taus
    // from two different clock models)
    if (sy->lane[stream].has) {
        sy->lane[stream].tau = effective_tau(sy, stream, sy->lane[stream].held.t_ns);
    }
}

weft_sync_verdict_t weft_sync_offer(weft_sync_t* sy, uint32_t stream,
                                    const weft_flow_view* v,
                                    weft_flow_view* out) {
    if (sy == NULL || v == NULL || stream >= sy->n_streams) {
        return WEFT_SYNC_INVALID;
    }
    weft_sync_lane_t* lane = &sy->lane[stream];
    int64_t tau = effective_tau(sy, stream, v->t_ns);

    int replaced = 0;
    if (lane->has && tau < lane->tau) {
        sy->t_stale++;          // out-of-order: never regress a stream
        return WEFT_SYNC_STALE;
    }
    if (lane->has) {
        sy->t_coalesced++;      // replacing an unconsumed older sample
        replaced = 1;
    }
    lane->held = *v;
    lane->has = 1;
    lane->tau = tau;

    // ANCHOR policy: only an arrival on the anchor stream attempts a
    // tuple — non-anchor samples arm their lane and wait for the anchor's
    // next tick. MAX/MIN_TS attempt on every arrival.
    if (sy->pivot_policy == WEFT_SYNC_PIVOT_ANCHOR &&
        stream != sy->anchor_stream) {
        return replaced ? WEFT_SYNC_COALESCED : WEFT_SYNC_HELD;
    }

    // all streams armed?
    for (uint32_t i = 0; i < sy->n_streams; i++) {
        if (!sy->lane[i].has) return replaced ? WEFT_SYNC_COALESCED : WEFT_SYNC_HELD;
    }

    // pivot
    int64_t pivot = sy->lane[0].tau;
    if (sy->pivot_policy == WEFT_SYNC_PIVOT_MAX_TS) {
        for (uint32_t i = 1; i < sy->n_streams; i++) {
            if (sy->lane[i].tau > pivot) pivot = sy->lane[i].tau;
        }
    } else if (sy->pivot_policy == WEFT_SYNC_PIVOT_MIN_TS) {
        for (uint32_t i = 1; i < sy->n_streams; i++) {
            if (sy->lane[i].tau < pivot) pivot = sy->lane[i].tau;
        }
    } else {
        pivot = sy->lane[sy->anchor_stream].tau;
    }

    // spread check
    if (sy->pivot_policy == WEFT_SYNC_PIVOT_ANCHOR) {
        // every non-anchor stream must be within the anchor's window; the
        // ones outside are released (their tuple never existed) and the
        // anchor tick is refused — it retries on its NEXT sample
        int any_out = 0;
        for (uint32_t i = 0; i < sy->n_streams; i++) {
            if (i == sy->anchor_stream) continue;
            int64_t d = sy->lane[i].tau - pivot;
            if (d < 0) d = -d;
            if ((uint64_t)d > sy->tolerance_ns) {
                sy->lane[i].has = 0;
                sy->t_gap++;
                any_out = 1;
            }
        }
        if (any_out) return WEFT_SYNC_GAP;
    } else {
        // MAX/MIN_TS: every stream must be within the pivot's window
        for (uint32_t i = 0; i < sy->n_streams; i++) {
            int64_t d = sy->lane[i].tau - pivot;
            if (d < 0) d = -d;
            if ((uint64_t)d > sy->tolerance_ns) {
                // release the laggard (smallest tau) — the tuple it belonged
                // to never existed; holding it would only deepen the backlog
                uint32_t lag = 0;
                for (uint32_t j = 1; j < sy->n_streams; j++) {
                    if (sy->lane[j].tau < sy->lane[lag].tau) lag = j;
                }
                sy->lane[lag].has = 0;
                sy->t_gap++;
                return WEFT_SYNC_GAP;
            }
        }
    }

    // emit the tuple, release all holds
    for (uint32_t i = 0; i < sy->n_streams; i++) {
        out[i] = sy->lane[i].held;  // ORIGINAL t_ns — alignment != rewriting
        sy->lane[i].has = 0;
    }
    sy->t_tuples++;
    return WEFT_SYNC_TUPLE;
}
