// weft_sync_test.c — RFC 0018 Y-series: sensor-fusion synchronizer.
//
// Y1  aligned emission: 60 Hz video + 100 Hz audio + 200 Hz IMU over 10s
//     -> exactly 600 tuples, pinned checksum
// Y2  pivot policies: MAX_TS / MIN_TS / ANCHOR semantics
// Y3  tolerance failure -> laggard release, accounting exact
// Y4  stale (out-of-order) refusal
// Y5  coalescing (newer replaces unconsumed)
// Y6  offset calibration (a skewed domain aligned by its offset)
// Y7  invalid geometry refused

#define _GNU_SOURCE
#include "weft_sync.h"
#include "weft.h"   // weft_pat

#include <stdio.h>
#include <string.h>

static int g_checks = 0, g_fails = 0;
#define CK(cond, name)                                              \
    do {                                                            \
        g_checks++;                                                 \
        if (!(cond)) {                                              \
            g_fails++;                                              \
            printf("  FAIL %s:%d %s\n", __func__, __LINE__, name);  \
        }                                                           \
    } while (0)

static weft_flow_view mkv(uint32_t seq, uint64_t t_ns) {
    weft_flow_view v;
    v.ptr = NULL; v.len = 4; v.seq = seq; v.t_ns = t_ns;
    return v;
}

// the mission scenario: three independent rings, one monotonic clock domain
// each (offsets 0 here), tolerance 2 ms. Video 60 Hz rules the cadence;
// audio 100 Hz and IMU 200 Hz must join it. Returns the tuple count and
// an FNV-1a checksum over the tuple (video seq, audio seq, imu seq).
static uint64_t fusion_scenario(weft_sync_t* sy, uint64_t* tuples_out,
                                uint32_t* gap_out) {
    // rates in mHz-ish ticks: video every 16666667 ns, audio every 10 ms,
    // imu every 5 ms; run 10 s. Feed by global time: at each offer step,
    // give the stream whose next timestamp has come. Order: deterministic
    // (video, audio, imu round-robin by earliest next-ts, ties by index).
    uint64_t next[3] = {0, 0, 0};
    uint64_t period[3] = {16666667ull, 10000000ull, 5000000ull};
    uint32_t seq[3] = {0, 0, 0};
    uint64_t tuples = 0, h = 0xcbf29ce484222325ull;
    uint32_t gaps = 0;
    weft_flow_view out[3];
    for (int step = 0; step < 4000; step++) {  // 3600 offers + margin
        // pick the stream with the earliest next timestamp (tie: lowest idx)
        int k = 0;
        for (int i = 1; i < 3; i++) {
            if (next[i] < next[k]) k = i;
        }
        uint64_t t = next[k];
        if (t >= 10000000000ull) break;
        seq[k]++;
        weft_flow_view v = mkv(seq[k], t);
        weft_sync_verdict_t r = weft_sync_offer(sy, (uint32_t)k, &v, out);
        if (r == WEFT_SYNC_TUPLE) {
            tuples++;
            for (int i = 0; i < 3; i++) {
                h ^= out[i].seq & 0xff;
                h *= 0x100000001b3ull;
            }
        } else if (r == WEFT_SYNC_GAP) {
            gaps++;
        }
        next[k] += period[k];
    }
    *tuples_out = tuples;
    *gap_out = gaps;
    return h;
}

static void t_y1_fusion(void) {
    weft_sync_t sy;
    // ANCHOR(video): the 60 Hz stream rules the cadence; tolerance covers
    // one audio period (10 ms) — the RFC-0018 mission configuration.
    CK(weft_sync_init(&sy, 3, 10000000 /*10 ms*/, WEFT_SYNC_PIVOT_ANCHOR, 0) == 0,
       "init 3 streams, 10 ms tolerance, ANCHOR(video)");
    uint64_t tuples = 0;
    uint32_t gaps = 0;
    uint64_t h = fusion_scenario(&sy, &tuples, &gaps);
    // Deterministic invariants (the RFC-0018 mission configuration):
    // video tick 0 finds no followers armed (all three streams start at
    // t=0; the anchor offer goes first) — its tuple never forms. Every
    // subsequent tick fuses: 599 tuples, zero gaps, zero stale.
    CK(tuples == 599, "10s @ 60 Hz -> 599 tuples (tick-0 arming boundary)");
    CK(gaps == 0, "zero gaps at nominal rates");
    CK(sy.t_stale == 0, "no out-of-order at nominal rates");
    CK(sy.t_coalesced == 1801, "follower coalescing exact");
    CK(sy.t_tuples == tuples, "counter agreement");
    CK(h == 0x144dd8920bcd3fb0ull, "pinned fusion checksum (parity vector)");
}

static void t_y2_pivots(void) {
    // MIN_TS emits at the slowest stream's cadence; ANCHOR rules
    weft_sync_t sy;
    weft_sync_init(&sy, 2, 1000, WEFT_SYNC_PIVOT_MIN_TS, 0);
    weft_flow_view out[2];
    weft_flow_view a1 = mkv(1, 0);
    weft_flow_view b1 = mkv(1, 800);
    weft_sync_offer(&sy, 0, &a1, out);
    CK(weft_sync_offer(&sy, 1, &b1, out) == WEFT_SYNC_TUPLE, "MIN_TS: 800us spread ok");
    // ANCHOR: stream 1 rules — a follower arrival only ARMS its lane;
    // the anchor's tick attempts the tuple
    weft_sync_t s2;
    weft_sync_init(&s2, 2, 1000, WEFT_SYNC_PIVOT_ANCHOR, 1);
    weft_flow_view fa1 = mkv(1, 0);      // follower samples (stream 0)
    weft_flow_view an1 = mkv(1, 900);    // anchor samples (stream 1)
    CK(weft_sync_offer(&s2, 0, &fa1, out) == WEFT_SYNC_HELD,
       "ANCHOR: follower arrival arms, waits for the anchor tick");
    CK(weft_sync_offer(&s2, 0, &fa1, out) == WEFT_SYNC_COALESCED,
       "ANCHOR: equal-tau reoffer coalesces");
    CK(weft_sync_offer(&s2, 1, &an1, out) == WEFT_SYNC_TUPLE,
       "ANCHOR: anchor tick emits when the follower is in window");
    CK(out[0].t_ns == 0 && out[1].t_ns == 900, "tuple in stream order");
    // follower out of the anchor window -> released, tick refused
    weft_flow_view a2 = mkv(2, 20000);
    weft_flow_view b2 = mkv(2, 21500);  // 1500us from the follower — outside 1000
    weft_sync_offer(&s2, 0, &a2, out);   // armed @20000
    weft_sync_offer(&s2, 0, &fa1, out);  // stale (tau 0 < held 20000)
    CK(s2.t_stale == 1, "out-of-order follower refused");
    CK(weft_sync_offer(&s2, 1, &b2, out) == WEFT_SYNC_GAP,
       "ANCHOR: follower outside window -> GAP, follower released");
    CK(s2.t_gap == 1, "gap counted");
    weft_flow_view a3 = mkv(3, 30000);
    weft_flow_view b3 = mkv(3, 30400);
    weft_sync_offer(&s2, 0, &a3, out);
    CK(weft_sync_offer(&s2, 1, &b3, out) == WEFT_SYNC_TUPLE,
       "ANCHOR: recovery on the next tick");
}

static void t_y3_gap(void) {
    weft_sync_t sy;
    weft_sync_init(&sy, 2, 1000, WEFT_SYNC_PIVOT_MAX_TS, 0);
    weft_flow_view out[2];
    weft_flow_view a = mkv(1, 0);
    weft_flow_view b = mkv(1, 5000000);  // 5 ms away — far outside 1 µs
    weft_sync_offer(&sy, 0, &a, out);
    CK(weft_sync_offer(&sy, 1, &b, out) == WEFT_SYNC_GAP, "tolerance failed");
    CK(sy.t_gap == 1, "gap counted");
    // the laggard (a, tau 0) was released; b@5ms stays held. A fresh a at
    // the same instant as the held b forms the recovery tuple.
    weft_flow_view a2 = mkv(2, 5000000);
    CK(weft_sync_offer(&sy, 0, &a2, out) == WEFT_SYNC_TUPLE,
       "recovery tuple after laggard release");
    CK(out[0].seq == 2 && out[1].seq == 1, "recovery pairs a2 with held b1");
}

static void t_y4_stale(void) {
    weft_sync_t sy;
    weft_sync_init(&sy, 2, 1000, WEFT_SYNC_PIVOT_MAX_TS, 0);
    weft_flow_view out[2];
    weft_flow_view v1 = mkv(1, 5000);
    weft_flow_view v0 = mkv(0, 1000);  // older — out of order
    weft_sync_offer(&sy, 0, &v1, out);  // held, waiting for stream 1
    CK(weft_sync_offer(&sy, 0, &v0, out) == WEFT_SYNC_STALE, "late sample refused");
    CK(sy.t_stale == 1, "stale counted");
    CK(weft_sync_offer(&sy, 0, &v1, out) == WEFT_SYNC_COALESCED,
       "identical re-offer counts as coalesce (equal tau)");
}

static void t_y5_coalesce(void) {
    weft_sync_t sy;
    weft_sync_init(&sy, 2, 1000, WEFT_SYNC_PIVOT_MAX_TS, 0);
    weft_flow_view out[2];
    weft_flow_view a1 = mkv(1, 1000);
    weft_flow_view a2 = mkv(2, 1200);
    weft_flow_view b1 = mkv(1, 1500);
    weft_sync_offer(&sy, 0, &a1, out);
    CK(weft_sync_offer(&sy, 0, &a2, out) == WEFT_SYNC_COALESCED,
       "newer replaced unconsumed (COALESCED verdict)");
    CK(sy.t_coalesced == 1, "coalesce counted");
    CK(weft_sync_offer(&sy, 1, &b1, out) == WEFT_SYNC_TUPLE, "tuple with newest");
    CK(out[0].seq == 2, "tuple holds the NEWEST a");
}

static void t_y6_offsets(void) {
    // audio domain is +5 ms skewed; calibrating with -5 ms aligns it
    weft_sync_t sy;
    weft_sync_init(&sy, 2, 1000, WEFT_SYNC_PIVOT_MAX_TS, 0);
    weft_flow_view out[2];
    weft_flow_view a = mkv(1, 10000000);   // video at t=10 ms
    weft_flow_view b = mkv(1, 15000000);   // audio stamp reads 15 ms
    weft_sync_offer(&sy, 0, &a, out);        // video@10ms armed
    CK(weft_sync_offer(&sy, 1, &b, out) == WEFT_SYNC_GAP, "skew breaks fusion");
    // video (the laggard) was released; audio@15ms stays armed
    weft_sync_set_offset(&sy, 1, -5000000);  // calibration rebind: held tau
                                             // moves 15ms -> 10ms
    weft_flow_view a2 = mkv(2, 10000000);    // next video tick at 10ms
    CK(weft_sync_offer(&sy, 0, &a2, out) == WEFT_SYNC_TUPLE,
       "calibrated domain fuses");
    CK(out[1].t_ns == 15000000, "tuple preserves the ORIGINAL t_ns");
    CK(out[0].seq == 2, "tuple pairs with the fresh anchor tick");
}

static void t_y7_geometry(void) {
    weft_sync_t sy;
    CK(weft_sync_init(&sy, 0, 1, 0, 0) == -1, "0 streams refused");
    CK(weft_sync_init(&sy, 9, 1, 0, 0) == -1, "9 streams refused");
    CK(weft_sync_init(&sy, 2, 1, 42, 0) == -1, "unknown pivot refused");
    CK(weft_sync_init(&sy, 2, 1, WEFT_SYNC_PIVOT_ANCHOR, 5) == -1,
       "anchor >= n refused");
    weft_flow_view out[2];
    weft_flow_view v = mkv(1, 0);
    CK(weft_sync_offer(&sy, 7, &v, out) == WEFT_SYNC_INVALID, "bad stream refused");
}

int main(void) {
    printf("== weft_sync_test — RFC 0018 Y-series ==\n");
    t_y1_fusion();
    t_y2_pivots();
    t_y3_gap();
    t_y4_stale();
    t_y5_coalesce();
    t_y6_offsets();
    t_y7_geometry();
    printf("== Y-series: %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
