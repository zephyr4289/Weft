// weft_synth_net.c — Virtual RDMA/UDP Network Chaos Injector + WCR1-style
// lease consensus reference (Pillar 8, module C).
//
// Implementation notes (the WHY lives in the header):
//   * fate is decided ENTIRELY at send time (partition -> drop model ->
//     bit-flip -> jitter), then played out by the timing wheel — the
//     virtual wire is therefore a replay function of the PRNG stream.
//   * the timing wheel is an intrusive LIFO per 1-us slot over a fixed
//     packet pool; bucket link lives in the packet (next_idx), so there
//     is zero allocation and zero list metadata outside the pool.
//   * CRC runs only when the chaos layer is armed (fault.enabled) or the
//     caller validates explicitly — that is what keeps the disabled-hook
//     overhead at branch scale for the < 5% bench gate.
//   * the shared Gilbert-Elliott channel transitions per PACKET (not per
//     tick): burst length statistics then come out in packets, which is
//     what burst-drop models mean on a wire.
//   * consensus safety rests on two structural facts: one vote per epoch
//     (so majorities intersect and a granted epoch has a unique owner),
//     and election_min > lease_ttl (so any successor is elected only
//     after the old lease is provably dead). Both are asserted at init
//     and counted at runtime.

#include "weft_synth/weft_synth_net.h"

#include <math.h>
#include <string.h>

#define WEFT_SYNTH_NET_PI 3.14159265358979323846

/* ------------------------------------------------------------------ */
/* CRC-32 (slice-by-4, lazily built table, process-wide)                */
/* ------------------------------------------------------------------ */

static uint32_t weft_synth_net_crc_tbl[4][256];
static int weft_synth_net_crc_ready = 0;

static void weft_synth_net_crc_build(void) {
    for (uint32_t i = 0u; i < 256u; i++) {
        uint32_t c = i;
        for (uint32_t k = 0u; k < 8u; k++) {
            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
        }
        weft_synth_net_crc_tbl[0][i] = c;
    }
    for (uint32_t i = 0u; i < 256u; i++) {
        uint32_t c = weft_synth_net_crc_tbl[0][i];
        for (uint32_t j = 1u; j < 4u; j++) {
            c = weft_synth_net_crc_tbl[0][c & 0xFFu] ^ (c >> 8);
            weft_synth_net_crc_tbl[j][i] = c;
        }
    }
    weft_synth_net_crc_ready = 1;
}

static uint32_t weft_synth_net_crc_update(uint32_t crc,
                                          const uint8_t *buf, size_t len) {
    crc = ~crc;
    while (len >= 4u) {
        crc ^= (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
               ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
        crc = weft_synth_net_crc_tbl[3][crc & 0xFFu] ^
              weft_synth_net_crc_tbl[2][(crc >> 8) & 0xFFu] ^
              weft_synth_net_crc_tbl[1][(crc >> 16) & 0xFFu] ^
              weft_synth_net_crc_tbl[0][crc >> 24];
        buf += 4;
        len -= 4u;
    }
    while (len > 0u) {
        crc = (crc >> 8) ^
              weft_synth_net_crc_tbl[0][(crc ^ *buf++) & 0xFFu];
        len--;
    }
    return ~crc;
}

uint32_t weft_synth_net_crc32(const void *data, size_t len) {
    if (data == NULL && len != 0u) {
        return 0u;
    }
    if (!weft_synth_net_crc_ready) {
        weft_synth_net_crc_build();
    }
    return weft_synth_net_crc_update(0u, (const uint8_t *)data, len);
}

/* CRC over the packet's identity fields + payload (crc field excluded). */
static uint32_t weft_synth_net_pkt_crc(const weft_synth_net_pkt_t *pkt) {
    uint8_t hdr[24];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0, &pkt->src, sizeof(pkt->src));
    memcpy(hdr + 4, &pkt->dst, sizeof(pkt->dst));
    memcpy(hdr + 8, &pkt->seq, sizeof(pkt->seq));
    memcpy(hdr + 16, &pkt->kind, sizeof(pkt->kind));
    memcpy(hdr + 18, &pkt->payload_len, sizeof(pkt->payload_len));
    uint32_t crc = weft_synth_net_crc_update(0u, hdr, sizeof(hdr));
    return weft_synth_net_crc_update(crc, pkt->payload, pkt->payload_len);
}

int weft_synth_net_pkt_valid(const weft_synth_net_pkt_t *pkt) {
    if (pkt == NULL) {
        return 0;
    }
    if (pkt->crc == 0u) {
        return 1;  /* chaos-off wire: valid by construction */
    }
    if (pkt->payload_len > WEFT_SYNTH_NET_PKT_PAYLOAD) {
        return 0;
    }
    return weft_synth_net_pkt_crc(pkt) == pkt->crc ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* configuration                                                        */
/* ------------------------------------------------------------------ */

void weft_synth_net_fault_defaults(weft_synth_net_fault_cfg_t *f) {
    if (f == NULL) {
        return;
    }
    f->enabled = 0;
    f->model = WEFT_SYNTH_NET_FAULT_NONE;
    f->drop_p = 0.0;
    f->ge_p_g2b = 0.0;
    f->ge_p_b2g = 0.0;
    f->ge_drop_good = 0.0;
    f->ge_drop_bad = 0.0;
    f->jitter_mean_ns = 0.0;
    f->jitter_std_ns = 0.0;
    f->jitter_max_ns = 0.0;
    f->reorder_p = 0.0;
    f->reorder_extra_mean_ns = 0.0;
    f->bitflip_p = 0.0;
    f->bitflip_bits_min = 1u;
    f->bitflip_bits_max = 1u;
}

int weft_synth_net_defaults(weft_synth_net_cfg_t *cfg) {
    if (cfg == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    cfg->n_nodes = 5u;
    cfg->seed = 1u;
    cfg->rx_capacity = WEFT_SYNTH_NET_RX_CAP;
    cfg->pool_capacity = WEFT_SYNTH_NET_POOL_CAP;
    weft_synth_net_fault_defaults(&cfg->fault);
    return WEFT_SYNTH_OK;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                            */
/* ------------------------------------------------------------------ */

static int weft_synth_net_fault_validate(const weft_synth_net_fault_cfg_t *f) {
    if (f->drop_p < 0.0 || f->drop_p > 1.0 ||
        f->ge_p_g2b < 0.0 || f->ge_p_g2b > 1.0 ||
        f->ge_p_b2g < 0.0 || f->ge_p_b2g > 1.0 ||
        f->ge_drop_good < 0.0 || f->ge_drop_good > 1.0 ||
        f->ge_drop_bad < 0.0 || f->ge_drop_bad > 1.0 ||
        f->reorder_p < 0.0 || f->reorder_p > 1.0 ||
        f->bitflip_p < 0.0 || f->bitflip_p > 1.0) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    if (f->jitter_mean_ns < 0.0 || f->jitter_std_ns < 0.0 ||
        f->jitter_max_ns < 0.0 || f->reorder_extra_mean_ns < 0.0) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    /* wheel horizon: worst delay must land strictly inside the span */
    double max_delay_ns = f->jitter_max_ns;
    if (f->reorder_extra_mean_ns > 0.0) {
        max_delay_ns = f->jitter_max_ns;  /* extra is clamped to max too */
    }
    if (max_delay_ns > (double)((WEFT_SYNTH_NET_WHEEL_SPAN - 4u) * 1000u)) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    if (f->bitflip_bits_min < 1u ||
        f->bitflip_bits_max < f->bitflip_bits_min ||
        f->bitflip_bits_max > 8u) {  /* distinct-bit redraw buffer bound */
        return WEFT_SYNTH_ERR_RANGE;
    }
    return WEFT_SYNTH_OK;
}

int weft_synth_net_init(weft_synth_net_t *net,
                        const weft_synth_net_cfg_t *cfg) {
    if (net == NULL || cfg == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    if (cfg->n_nodes < 2u || cfg->n_nodes > WEFT_SYNTH_NET_MAX_NODES) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    if (cfg->rx_capacity == 0u ||
        cfg->rx_capacity > WEFT_SYNTH_NET_RX_CAP) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    if (cfg->pool_capacity == 0u ||
        cfg->pool_capacity > WEFT_SYNTH_NET_POOL_CAP) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    int rc = weft_synth_net_fault_validate(&cfg->fault);
    if (rc != WEFT_SYNTH_OK) {
        return rc;
    }

    memset(net, 0, sizeof(*net));
    net->cfg = *cfg;
    net->rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)cfg->seed;
    (void)weft_synth_splitmix64(&net->rng);
    net->trace_hash = 0xCBF29CE484222325ull;
    net->tick_now = 0ull;
    net->ge_bad = 0;
    net->groups_nonzero = 0u;
    net->interceptor_armed = (cfg->fault.enabled != 0) ? 1 : 0;
    for (uint32_t i = 0u; i < WEFT_SYNTH_NET_WHEEL_SPAN; i++) {
        net->wheel_head[i] = WEFT_SYNTH_NET_NO_IDX;
        net->wheel_tail[i] = WEFT_SYNTH_NET_NO_IDX;
    }
    /* free stack: index order is deterministic; pops come off the top */
    for (uint32_t i = 0u; i < cfg->pool_capacity; i++) {
        net->pool_free[i] = i;
    }
    net->pool_free_top = cfg->pool_capacity;
    /* force CRC table build now: honors the single-threaded init contract
     * even if the first armed send happens on another thread later */
    (void)weft_synth_net_crc32("", 0u);
    return WEFT_SYNTH_OK;
}

static void weft_synth_net_trace(weft_synth_net_t *net, uint32_t code,
                                 uint64_t v) {
    net->trace_hash =
        weft_synth_fnv1a(weft_synth_fnv1a(net->trace_hash, code), v);
}

/* ------------------------------------------------------------------ */
/* gaussian / exponential jitter draws (fixed draw ORDER = Law 2)        */
/* ------------------------------------------------------------------ */

#ifndef WEFT_SYNTH_NET_NO_INTERCEPTOR
/* gaussian draw for the jitter model (unused in the bench twin build) */
static double weft_synth_net_gauss(uint64_t *rng) {
    double u1 = weft_synth_u01(rng);
    double u2 = weft_synth_u01(rng);
    if (u1 < 1e-300) {
        u1 = 1e-300;
    }
    return sqrt(-2.0 * log(u1)) * cos(2.0 * WEFT_SYNTH_NET_PI * u2);
}
#endif

/* ------------------------------------------------------------------ */
/* send: decide the packet's fate, then schedule it                     */
/* ------------------------------------------------------------------ */

/* intercept(): the fate decisions only — partition, drop model, bit-flip
 * draw, jitter draw. Returns 1 when the packet is dropped (fate decided).
 * Out-of-line so the armed path never pollutes the fast path's layout. */
#ifndef WEFT_SYNTH_NET_NO_INTERCEPTOR
__attribute__((noinline)) static int weft_synth_net_intercept(
    weft_synth_net_t *net, uint32_t from, uint32_t to, uint32_t len,
    uint64_t seq, const weft_synth_net_fault_cfg_t *f, int *do_flip,
    uint32_t *flip_bits, uint64_t *delay_ticks) {

    /* 1) split-brain partition: bidirectional group isolation */
    if (net->groups[from] != 0u && net->groups[to] != 0u &&
        net->groups[from] != net->groups[to]) {
        net->stats.dropped_partition++;
        weft_synth_net_trace(net, 3u, seq);
        return 1;
    }

    /* 2) drop model (one draw for Bernoulli, state+drop draws for GE) */
    if (f->enabled && f->model == WEFT_SYNTH_NET_FAULT_BERNOULLI &&
        f->drop_p > 0.0) {
        if (weft_synth_u01(&net->rng) < f->drop_p) {
            net->stats.dropped_bernoulli++;
            weft_synth_net_trace(net, 1u, seq);
            return 1;
        }
    } else if (f->enabled &&
               f->model == WEFT_SYNTH_NET_FAULT_GILBERT_ELLIOTT) {
        if (net->ge_bad) {
            if (weft_synth_u01(&net->rng) < f->ge_p_b2g) {
                net->ge_bad = 0;
            }
        } else {
            if (weft_synth_u01(&net->rng) < f->ge_p_g2b) {
                net->ge_bad = 1;
            }
        }
        const double pdrop =
            net->ge_bad ? f->ge_drop_bad : f->ge_drop_good;
        if (pdrop > 0.0 && weft_synth_u01(&net->rng) < pdrop) {
            net->stats.dropped_ge++;
            weft_synth_net_trace(net, 2u, seq);
            return 1;
        }
    }

    /* 3) bit-flip decision (applied to the copy AFTER the CRC) */
    if (f->enabled && f->bitflip_p > 0.0 && len > 0u) {
        if (weft_synth_u01(&net->rng) < f->bitflip_p) {
            *do_flip = 1;
            uint32_t span =
                f->bitflip_bits_max - f->bitflip_bits_min + 1u;
            *flip_bits = f->bitflip_bits_min +
                         (uint32_t)(weft_synth_xorshift64(&net->rng) %
                                    span);
            net->stats.corrupted_injected++;
        }
    }

    /* 4) jitter: gaussian delay + exponential reorder extra, clamped */
    if (f->enabled &&
        (f->jitter_mean_ns > 0.0 || f->jitter_std_ns > 0.0 ||
         f->reorder_p > 0.0)) {
        double d = f->jitter_mean_ns;
        if (f->jitter_std_ns > 0.0) {
            d += f->jitter_std_ns * weft_synth_net_gauss(&net->rng);
        }
        if (f->reorder_p > 0.0 &&
            weft_synth_u01(&net->rng) < f->reorder_p) {
            double u = weft_synth_u01(&net->rng);
            if (u < 1e-300) {
                u = 1e-300;
            }
            d += f->reorder_extra_mean_ns * (-log(u));
        }
        if (d < 0.0) {
            d = 0.0;
        }
        if (d > f->jitter_max_ns) {
            d = f->jitter_max_ns;
        }
        *delay_ticks = (uint64_t)(d + 999.0) / 1000ull;
        if (d > 0.5) {
            net->stats.jittered++;
        }
        if (*delay_ticks > 0ull) {
            weft_synth_net_trace(net, 5u, *delay_ticks);
        }
    }
    return 0;
}
#endif  /* WEFT_SYNTH_NET_NO_INTERCEPTOR */

#ifndef WEFT_SYNTH_NET_NO_INTERCEPTOR
/* The ARMED send path — fate decisions plus (delayed, CRC-stamped,
 * possibly corrupted) scheduling, fully out-of-line: the canonical
 * zero-overhead-interceptor pattern. The public send() keeps an inline
 * fast path whose shape matches the interceptor-free recompile, so the
 * disabled-interceptor overhead is two cached loads and one predictable
 * branch (the G1 < 5% mandate; a single-body send with the interceptor
 * merely out-of-line measured ~4.5% through code-layout effects alone). */
__attribute__((noinline)) static int weft_synth_net_send_armed(
    weft_synth_net_t *net, uint32_t from, uint32_t to, uint8_t kind,
    const void *payload, uint32_t len, uint64_t seq,
    const weft_synth_net_fault_cfg_t *f) {
    int do_flip = 0;
    uint32_t flip_bits = 0u;
    uint64_t delay_ticks = 0ull;
    if (weft_synth_net_intercept(net, from, to, len, seq, f, &do_flip,
                                 &flip_bits, &delay_ticks)) {
        return WEFT_SYNTH_OK;  /* fate decided: dropped by chaos */
    }

    if (net->pool_free_top == 0u) {
        net->stats.dropped_pool++;
        weft_synth_net_trace(net, 6u, seq);
        return WEFT_SYNTH_OK;
    }
    const uint32_t idx = net->pool_free[--net->pool_free_top];
    weft_synth_net_pkt_t *pkt = &net->pool[idx];

    pkt->src = from;
    pkt->dst = to;
    pkt->seq = seq;
    pkt->kind = kind;
    pkt->payload_len = (uint16_t)len;
    pkt->corrupted = 0u;
    pkt->deliver_at_tick = net->tick_now + 1ull + delay_ticks;
    if (len > 0u) {
        memcpy(pkt->payload, payload, len);
    }
    pkt->crc = weft_synth_net_pkt_crc(pkt);  /* armed: protect the wire */
    if (do_flip) {
        /* DISTINCT bit positions: flipping the same bit twice would
         * cancel out and hand the receiver a CRC-valid "corrupted"
         * packet — the injector's ledger would disagree with the wire.
         * Redraw-on-collision keeps every corrupted packet detectable
         * (bounded: bits <= 8, payload >= 1 byte). */
        uint32_t picked[8];
        for (uint32_t b = 0u; b < flip_bits; b++) {
            uint64_t bit;
            int dup;
            do {
                dup = 0;
                bit = weft_synth_xorshift64(&net->rng) %
                      ((uint64_t)len * 8ull);
                for (uint32_t j = 0u; j < b; j++) {
                    if (picked[j] == (uint32_t)bit) {
                        dup = 1;
                        break;
                    }
                }
            } while (dup);
            picked[b] = (uint32_t)bit;
            pkt->payload[bit / 8ull] ^= (uint8_t)(1u << (bit % 8ull));
        }
        pkt->corrupted = 1u;  /* injector flag: cross-check, not proof */
        weft_synth_net_trace(net, 4u, seq);
    }

    const uint32_t slot =
        (uint32_t)(pkt->deliver_at_tick % WEFT_SYNTH_NET_WHEEL_SPAN);
    pkt->next_idx = WEFT_SYNTH_NET_NO_IDX;
    if (net->wheel_head[slot] == WEFT_SYNTH_NET_NO_IDX) {
        net->wheel_head[slot] = idx;
    } else {
        net->pool[net->wheel_tail[slot]].next_idx = idx;
    }
    net->wheel_tail[slot] = idx;
    return WEFT_SYNTH_OK;
}
#endif  /* WEFT_SYNTH_NET_NO_INTERCEPTOR */

int weft_synth_net_send(weft_synth_net_t *net, uint32_t from, uint32_t to,
                        uint8_t kind, const void *payload, uint32_t len) {
    if (net == NULL || from >= net->cfg.n_nodes || to >= net->cfg.n_nodes ||
        len > WEFT_SYNTH_NET_PKT_PAYLOAD ||
        (payload == NULL && len != 0u)) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    net->stats.sent++;
    /* The send-attempt sequence number is assigned at ENTRY: a dropped
     * packet consumes its seq, so drop positions are reconstructible from
     * the delivered stream (the burst-loss battery depends on that). */
    const uint64_t seq = ++net->seq_ctr[from];

#ifndef WEFT_SYNTH_NET_NO_INTERCEPTOR
    /* THE interceptor gate: two cached loads and one predictable branch
     * price the entire chaos layer when the lab is disabled and the
     * fabric is unpartitioned — the mandate's "< 5% when disabled". */
    if (net->interceptor_armed || net->groups_nonzero != 0u) {
        return weft_synth_net_send_armed(net, from, to, kind, payload, len,
                                         seq, &net->cfg.fault);
    }
#endif  /* WEFT_SYNTH_NET_NO_INTERCEPTOR */

    /* FAST PATH (no chaos): byte-for-byte the shape of the
     * interceptor-free recompile — clean wire, min transit, no CRC. */
    if (net->pool_free_top == 0u) {
        net->stats.dropped_pool++;
        return WEFT_SYNTH_OK;
    }
    const uint32_t idx = net->pool_free[--net->pool_free_top];
    weft_synth_net_pkt_t *pkt = &net->pool[idx];

    pkt->src = from;
    pkt->dst = to;
    pkt->seq = seq;
    pkt->kind = kind;
    pkt->payload_len = (uint16_t)len;
    pkt->corrupted = 0u;
    pkt->deliver_at_tick = net->tick_now + 1ull;
    if (len > 0u) {
        memcpy(pkt->payload, payload, len);
    }
    pkt->crc = 0u;  /* chaos-off wire: valid by construction */

    const uint32_t slot =
        (uint32_t)(pkt->deliver_at_tick % WEFT_SYNTH_NET_WHEEL_SPAN);
    pkt->next_idx = WEFT_SYNTH_NET_NO_IDX;
    if (net->wheel_head[slot] == WEFT_SYNTH_NET_NO_IDX) {
        net->wheel_head[slot] = idx;
    } else {
        net->pool[net->wheel_tail[slot]].next_idx = idx;
    }
    net->wheel_tail[slot] = idx;
    return WEFT_SYNTH_OK;
}

/* ------------------------------------------------------------------ */
/* tick: advance the virtual clock, deliver what is due                 */
/* ------------------------------------------------------------------ */

static void weft_synth_net_deliver(weft_synth_net_t *net,
                                   weft_synth_net_pkt_t *p) {
    /* reorder ledger: this delivery is behind a newer one on its link */
    const uint64_t ls = net->last_seq_seen[p->dst][p->src];
    if (p->seq < ls) {
        net->stats.reordered_events++;
    }
    if (p->seq > ls) {
        net->last_seq_seen[p->dst][p->src] = p->seq;
    }

    if (net->rx[p->dst].count >= net->cfg.rx_capacity) {
        net->stats.dropped_rx_full++;
        weft_synth_net_trace(net, 7u, p->seq);
        return;
    }
    net->rx[p->dst].ring[net->rx[p->dst].tail] = *p;  /* one struct copy */
    net->rx[p->dst].tail =
        (net->rx[p->dst].tail + 1u) % net->cfg.rx_capacity;
    net->rx[p->dst].count++;
    net->stats.delivered++;
    if (p->corrupted) {
        net->stats.delivered_corrupt++;
    }
}

int weft_synth_net_tick(weft_synth_net_t *net) {
    if (net == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    net->tick_now++;
    net->stats.ticks++;
    const uint32_t slot =
        (uint32_t)(net->tick_now % WEFT_SYNTH_NET_WHEEL_SPAN);
    uint32_t idx = net->wheel_head[slot];
    net->wheel_head[slot] = WEFT_SYNTH_NET_NO_IDX;
    net->wheel_tail[slot] = WEFT_SYNTH_NET_NO_IDX;
    while (idx != WEFT_SYNTH_NET_NO_IDX) {
        weft_synth_net_pkt_t *p = &net->pool[idx];
        const uint32_t nxt = p->next_idx;
        if (p->deliver_at_tick == net->tick_now) {
            weft_synth_net_deliver(net, p);
        } else {
            /* stale bucket entry: horizon math makes this unreachable;
             * counted and dropped rather than wedged (fail-closed) */
            net->stats.dropped_pool++;
        }
        net->pool_free[net->pool_free_top++] = idx;  /* recycle */
        idx = nxt;
    }
    return WEFT_SYNTH_OK;
}

/* ------------------------------------------------------------------ */
/* recv                                                                 */
/* ------------------------------------------------------------------ */

int weft_synth_net_recv(weft_synth_net_t *net, uint32_t node,
                        weft_synth_net_pkt_t *out_pkt) {
    if (net == NULL || node >= net->cfg.n_nodes || out_pkt == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    if (net->rx[node].count == 0u) {
        return 0;
    }
    *out_pkt = net->rx[node].ring[net->rx[node].head];
    net->rx[node].head = (net->rx[node].head + 1u) % net->cfg.rx_capacity;
    net->rx[node].count--;
    if (out_pkt->crc != 0u && !weft_synth_net_pkt_valid(out_pkt)) {
        net->stats.rejected_crc++;
        return 2;  /* delivered, but the receiver's CRC gate rejects it */
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* partitions                                                           */
/* ------------------------------------------------------------------ */

void weft_synth_net_partition_set(weft_synth_net_t *net,
                                  const uint32_t groups[
                                      WEFT_SYNTH_NET_MAX_NODES]) {
    if (net == NULL || groups == NULL) {
        return;
    }
    uint64_t g = 0xCBF29CE484222325ull;
    uint32_t nonzero = 0u;
    for (uint32_t i = 0u; i < net->cfg.n_nodes; i++) {
        net->groups[i] = groups[i];
        if (groups[i] != 0u) {
            nonzero++;
        }
        g = weft_synth_fnv1a(g, groups[i]);
    }
    net->groups_nonzero = nonzero;
    weft_synth_net_trace(net, 8u, g);
}

void weft_synth_net_partition_heal(weft_synth_net_t *net) {
    if (net == NULL) {
        return;
    }
    for (uint32_t i = 0u; i < net->cfg.n_nodes; i++) {
        net->groups[i] = 0u;
    }
    net->groups_nonzero = 0u;
    weft_synth_net_trace(net, 9u, net->tick_now);
}

uint64_t weft_synth_net_trace_hash(const weft_synth_net_t *net) {
    return (net == NULL) ? 0ull : net->trace_hash;
}

/* ------------------------------------------------------------------ */
/* WCR1-style lease consensus reference                                  */
/* ------------------------------------------------------------------ */

int weft_synth_consensus_defaults(weft_synth_consensus_cfg_t *cfg) {
    if (cfg == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    cfg->n_nodes = 5u;
    cfg->seed = 1u;
    /* liveness-tuned lease geometry: 7 heartbeat rounds per lease window
     * keep majority-renewal alive under ~40% loss (measured: see the
     * C2/C3 battery scenarios); election_min still exceeds lease_ttl by
     * 50 us + hb period, so the structural no-dual-primary argument
     * is untouched. */
    cfg->lease_ttl_ticks = 175ull;   /* us */
    cfg->hb_period_ticks = 25ull;    /* us — 7 rounds per lease window */
    cfg->election_min_ticks = 250ull;
    cfg->election_max_ticks = 500ull;
    cfg->crash_mask = 0u;
    return WEFT_SYNTH_OK;
}

/* randomized election timeout as an ABSOLUTE virtual tick: the campaign
 * and stepdown call sites store it directly, so the tick offset must be
 * folded in HERE (the original version returned a bare offset, which
 * post-init call sites stored as absolute — every timeout landed in the
 * past and the cluster hyper-campaigned at lockstep epochs forever). */
static uint64_t weft_synth_cs_rand_timeout(weft_synth_consensus_t *cs) {
    const uint64_t lo = cs->cfg.election_min_ticks;
    const uint64_t hi = cs->cfg.election_max_ticks;
    const uint64_t span = (hi > lo) ? (hi - lo + 1ull) : 1ull;
    return cs->tick + lo +
           (weft_synth_xorshift64(&cs->rng) % span);
}

static void weft_synth_cs_trace(weft_synth_consensus_t *cs, uint32_t code,
                                uint32_t node, uint64_t v) {
    cs->trace_hash =
        weft_synth_fnv1a(weft_synth_fnv1a(cs->trace_hash, code),
                         ((uint64_t)node << 32) ^ v);
}

int weft_synth_consensus_init(weft_synth_consensus_t *cs,
                              const weft_synth_consensus_cfg_t *cfg) {
    if (cs == NULL || cfg == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    if (cfg->n_nodes < 2u || cfg->n_nodes > WEFT_SYNTH_NET_MAX_NODES) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    /* structural no-dual-primary argument: any successor is elected only
     * after the old lease is provably dead (emin > ttl), and heartbeats
     * must fit inside a lease window (hb < ttl) */
    if (cfg->election_min_ticks <= cfg->lease_ttl_ticks ||
        cfg->election_max_ticks < cfg->election_min_ticks ||
        cfg->hb_period_ticks == 0ull ||
        cfg->hb_period_ticks >= cfg->lease_ttl_ticks) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    if (cfg->crash_mask >> cfg->n_nodes != 0u) {
        return WEFT_SYNTH_ERR_RANGE;
    }

    memset(cs, 0, sizeof(*cs));
    cs->cfg = *cfg;
    cs->rng = 0x9E3779B97F4A7C15ull ^ (0xD1B54A32D192ED03ull /
                                       (uint64_t)(cfg->seed + 1u));
    (void)weft_synth_splitmix64(&cs->rng);
    cs->trace_hash = 0xCBF29CE484222325ull;
    cs->tick = 0ull;
    for (uint32_t i = 0u; i < cfg->n_nodes; i++) {
        weft_synth_cnode_t *n = &cs->nodes[i];
        n->role = WEFT_SYNTH_CONSENSUS_FOLLOWER;
        n->epoch = 0ull;
        n->election_timeout_at = weft_synth_cs_rand_timeout(cs);
        n->prev_epoch = 0ull;
    }
    return WEFT_SYNTH_OK;
}

uint32_t weft_synth_consensus_set_crash_mask(weft_synth_consensus_t *cs,
                                             uint32_t mask) {
    if (cs == NULL) {
        return 0u;
    }
    uint32_t old = cs->cfg.crash_mask;
    cs->cfg.crash_mask = mask;
    return old;
}

/* one campaign: fresh epoch, self-vote, vote requests to everyone */
static void weft_synth_cs_campaign(weft_synth_consensus_t *cs,
                                   weft_synth_net_t *net, uint32_t i) {
    weft_synth_cnode_t *n = &cs->nodes[i];
    n->epoch += 1ull;                 /* strictly monotonic per node */
    n->campaign_epoch = n->epoch;
    n->voted_epoch = n->epoch;        /* self vote: one vote per epoch */
    n->vote_mask = (1u << i);
    n->role = WEFT_SYNTH_CONSENSUS_CANDIDATE;
    n->election_timeout_at = weft_synth_cs_rand_timeout(cs);
    for (uint32_t m = 0u; m < cs->cfg.n_nodes; m++) {
        if (m == i) {
            continue;
        }
        (void)weft_synth_net_send(net, i, m, WEFT_SYNTH_NET_KIND_VOTE_REQ,
                                  &n->epoch, sizeof(n->epoch));
        cs->votes_sent++;
    }
    weft_synth_cs_trace(cs, 2u, i, n->epoch);
}

int weft_synth_consensus_tick(weft_synth_consensus_t *cs,
                              weft_synth_net_t *net) {
    if (cs == NULL || net == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    const weft_synth_consensus_cfg_t *cfg = &cs->cfg;
    const uint32_t n_nodes = cfg->n_nodes;
    const uint32_t majority = n_nodes / 2u + 1u;
    const uint64_t reset_timeout =
        cfg->election_min_ticks + (cfg->election_max_ticks -
                                   cfg->election_min_ticks) / 2ull;
    cs->tick++;
    cs->ticks_elapsed++;

    /* ---- phase A: drain inboxes (fixed node order = determinism) ---- */
    for (uint32_t i = 0u; i < n_nodes; i++) {
        weft_synth_net_pkt_t pkt;
        if ((cfg->crash_mask >> i) & 1u) {
            /* crashed node: drain-and-discard so the wire stays healthy */
            while (weft_synth_net_recv(net, i, &pkt) > 0) {
                /* deliberate discard */
            }
            continue;
        }
        for (;;) {
            int rc = weft_synth_net_recv(net, i, &pkt);
            if (rc <= 0) {
                break;  /* empty or invalid args */
            }
            if (rc == 2) {
                cs->invalid_msgs++;  /* CRC garbage: dropped at the gate */
                continue;
            }
            cs->msgs_received++;
            uint64_t epoch = 0ull;
            memcpy(&epoch, pkt.payload, sizeof(epoch));
            weft_synth_cnode_t *n = &cs->nodes[i];
            switch (pkt.kind) {
            case WEFT_SYNTH_NET_KIND_VOTE_REQ:
                if (epoch > n->epoch) {
                    if (n->role == WEFT_SYNTH_CONSENSUS_PRIMARY) {
                        n->role = WEFT_SYNTH_CONSENSUS_FOLLOWER;
                        cs->stepdowns++;  /* a higher epoch exists */
                    } else if (n->role == WEFT_SYNTH_CONSENSUS_CANDIDATE) {
                        n->role = WEFT_SYNTH_CONSENSUS_FOLLOWER;
                    }
                    n->epoch = epoch;
                    n->voted_epoch = epoch;
                    n->election_timeout_at = cs->tick + reset_timeout;
                    (void)weft_synth_net_send(net, i, pkt.src,
                                              WEFT_SYNTH_NET_KIND_VOTE_GRANT,
                                              &epoch, sizeof(epoch));
                    cs->votes_sent++;
                }
                break;
            case WEFT_SYNTH_NET_KIND_VOTE_GRANT:
                if (n->role == WEFT_SYNTH_CONSENSUS_CANDIDATE &&
                    epoch == n->campaign_epoch) {
                    n->vote_mask |= (1u << pkt.src);
                }
                break;
            case WEFT_SYNTH_NET_KIND_HEARTBEAT:
                if (epoch >= n->epoch) {
                    if (n->role != WEFT_SYNTH_CONSENSUS_FOLLOWER) {
                        if (n->role == WEFT_SYNTH_CONSENSUS_PRIMARY) {
                            cs->stepdowns++;  /* equal/higher epoch HB */
                        }
                        n->role = WEFT_SYNTH_CONSENSUS_FOLLOWER;
                    }
                    n->epoch = epoch;
                    n->election_timeout_at = cs->tick + reset_timeout;
                    (void)weft_synth_net_send(net, i, pkt.src,
                                              WEFT_SYNTH_NET_KIND_HB_ACK,
                                              &epoch, sizeof(epoch));
                }
                break;
            case WEFT_SYNTH_NET_KIND_HB_ACK:
                if (n->role == WEFT_SYNTH_CONSENSUS_PRIMARY &&
                    epoch == n->lease_epoch) {
                    n->ack_mask |= (1u << pkt.src);
                }
                break;
            default:
                break;  /* DATA rides the same wire; consensus ignores it */
            }
        }
    }

    /* ---- phase B: elections, leases, heartbeats (crashed skip) ---- */
    for (uint32_t i = 0u; i < n_nodes; i++) {
        if ((cfg->crash_mask >> i) & 1u) {
            continue;
        }
        weft_synth_cnode_t *n = &cs->nodes[i];
        if (n->role == WEFT_SYNTH_CONSENSUS_FOLLOWER) {
            if (cs->tick >= n->election_timeout_at) {
                weft_synth_cs_campaign(cs, net, i);
            }
        } else if (n->role == WEFT_SYNTH_CONSENSUS_CANDIDATE) {
            if (__builtin_popcount(n->vote_mask) >= (int)majority) {
                n->role = WEFT_SYNTH_CONSENSUS_PRIMARY;
                n->lease_epoch = n->campaign_epoch;
                n->lease_expires = cs->tick + cfg->lease_ttl_ticks;
                n->ack_mask = (1u << i);
                cs->grants++;
                /* ledger: granted epochs strictly increase, one owner */
                if (cs->has_grant && n->lease_epoch <= cs->last_granted_epoch) {
                    cs->epoch_reuse_violations++;
                }
                cs->last_granted_epoch = n->lease_epoch;
                cs->has_grant = 1ull;
                cs->last_granted_node = i;
                weft_synth_cs_trace(cs, 3u, i, n->lease_epoch);
                /* immediate first heartbeat: lease clock starts now */
                for (uint32_t m = 0u; m < n_nodes; m++) {
                    if (m == i) {
                        continue;
                    }
                    (void)weft_synth_net_send(net, i, m,
                                              WEFT_SYNTH_NET_KIND_HEARTBEAT,
                                              &n->lease_epoch,
                                              sizeof(n->lease_epoch));
                    cs->hbs_sent++;
                }
                n->next_hb_at = cs->tick + cfg->hb_period_ticks;
            } else if (cs->tick >= n->election_timeout_at) {
                weft_synth_cs_campaign(cs, net, i);  /* split vote: retry */
            }
        } else {  /* PRIMARY */
            if (cs->tick >= n->lease_expires) {
                n->role = WEFT_SYNTH_CONSENSUS_FOLLOWER;
                cs->stepdowns++;
                n->election_timeout_at = weft_synth_cs_rand_timeout(cs);
                weft_synth_cs_trace(cs, 4u, i, n->lease_epoch);
            } else {
                if (__builtin_popcount(n->ack_mask) >= (int)majority) {
                    n->lease_expires = cs->tick + cfg->lease_ttl_ticks;
                    n->ack_mask = (1u << i);  /* renewal: fresh ack window */
                }
                if (cs->tick >= n->next_hb_at) {
                    for (uint32_t m = 0u; m < n_nodes; m++) {
                        if (m == i) {
                            continue;
                        }
                        (void)weft_synth_net_send(net, i, m,
                                                  WEFT_SYNTH_NET_KIND_HEARTBEAT,
                                                  &n->lease_epoch,
                                                  sizeof(n->lease_epoch));
                        cs->hbs_sent++;
                    }
                    n->next_hb_at = cs->tick + cfg->hb_period_ticks;
                }
            }
        }
    }

    /* ---- phase C: safety + liveness ledger ---- */
    uint32_t valid_prim = 0u;
    for (uint32_t i = 0u; i < n_nodes; i++) {
        weft_synth_cnode_t *n = &cs->nodes[i];
        if (n->role == WEFT_SYNTH_CONSENSUS_PRIMARY &&
            cs->tick < n->lease_expires) {
            valid_prim++;
        }
        if (n->epoch < n->prev_epoch) {
            cs->node_epoch_regressions++;
        }
        n->prev_epoch = n->epoch;
        if (n->epoch > cs->max_epoch) {
            cs->max_epoch = n->epoch;
        }
    }
    if (valid_prim > 1u) {
        cs->dual_primary_ticks++;
    }
    if (valid_prim >= 1u) {
        cs->ticks_with_primary++;
    }
    return WEFT_SYNTH_OK;
}

int weft_synth_consensus_primary_node(const weft_synth_consensus_t *cs) {
    if (cs == NULL) {
        return -1;
    }
    for (uint32_t i = 0u; i < cs->cfg.n_nodes; i++) {
        const weft_synth_cnode_t *n = &cs->nodes[i];
        if (n->role == WEFT_SYNTH_CONSENSUS_PRIMARY &&
            cs->tick < n->lease_expires) {
            return (int)i;
        }
    }
    return -1;
}

uint64_t weft_synth_consensus_trace_hash(const weft_synth_consensus_t *cs) {
    return (cs == NULL) ? 0ull : cs->trace_hash;
}
