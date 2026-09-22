// weft_contention_profiler.c — lock-free telemetry over read-only views.
//
// Pass anatomy (one weft_prof_scrape):
//   1. inspector snapshot refresh (bracketed acquire reads)
//   2. per watched ring: frontier probe (torn tracker), stall machine,
//      drop ledger diff
//   3. per watched generic region: relaxed word loads
//   4. false-sharing evolution: co-mutated cross-owner same-line pairs
//
// Every store in this file lands in the CALLER-OWNED profiler context.
// No shared memory is written anywhere, ever.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "weft_inspector/weft_contention_profiler.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                        */
/* ------------------------------------------------------------------ */

static void ring_stats_fill(const weft_prof_ring_watch_t *w,
                            weft_prof_ring_stats_t *out) {
    memset(out, 0, sizeof *out);
    out->torn_attempts = w->torn_attempts;
    out->torn_events = w->torn_events;
    out->writer_churn = w->writer_churn;
    out->stalls = w->stalls;
    out->stall_ns_total = w->stall_ns_total;
    out->reliable_timeout_windows = w->reliable_timeout_windows;
    out->stall_open = w->stall_state == 2u ? 1u : 0u;
    out->stall_opened_ns = w->stall_start_ns;
    out->published_cum = w->published_cum;
    out->dropped_cum = w->dropped_cum;
    out->drop_bursts = w->drop_bursts;
    out->last_event_ns = w->last_event_ns;
    out->drop_rate = (w->published_cum == 0u)
                         ? 0.0
                         : (double)w->dropped_cum / (double)w->published_cum;
}

static void ev_push_fshare(weft_prof_ctx_t *p,
                           const weft_prof_fshare_event_t *e) {
    if (p->fshare_ev_count < WEFT_PROF_EVENT_RING) {
        unsigned slot = (p->fshare_ev_head + p->fshare_ev_count) %
                        WEFT_PROF_EVENT_RING;
        p->fshare_ev[slot] = *e;
        p->fshare_ev_count++;
    } else {
        /* overwrite-oldest: freshest contention wins, loss is counted */
        p->fshare_ev[p->fshare_ev_head] = *e;
        p->fshare_ev_head = (p->fshare_ev_head + 1u) % WEFT_PROF_EVENT_RING;
        p->fshare_ev_lost++;
    }
}

static void ev_push_stall(weft_prof_ctx_t *p,
                          const weft_prof_stall_event_t *e) {
    if (p->stall_ev_count < WEFT_PROF_EVENT_RING) {
        unsigned slot = (p->stall_ev_head + p->stall_ev_count) %
                        WEFT_PROF_EVENT_RING;
        p->stall_ev[slot] = *e;
        p->stall_ev_count++;
    } else {
        p->stall_ev[p->stall_ev_head] = *e;
        p->stall_ev_head = (p->stall_ev_head + 1u) % WEFT_PROF_EVENT_RING;
        p->stall_ev_lost++;
    }
}

static void ev_push_drop(weft_prof_ctx_t *p, const weft_prof_drop_event_t *e) {
    if (p->drop_ev_count < WEFT_PROF_EVENT_RING) {
        unsigned slot = (p->drop_ev_head + p->drop_ev_count) %
                        WEFT_PROF_EVENT_RING;
        p->drop_ev[slot] = *e;
        p->drop_ev_count++;
    } else {
        p->drop_ev[p->drop_ev_head] = *e;
        p->drop_ev_head = (p->drop_ev_head + 1u) % WEFT_PROF_EVENT_RING;
        p->drop_ev_lost++;
    }
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                            */
/* ------------------------------------------------------------------ */

int weft_prof_init(weft_prof_ctx_t *p, weft_inspect_ctx_t *insp) {
    if (p == NULL || insp == NULL) return WEFT_INSPECT_ERR_INVALID;
    memset(p, 0, sizeof *p);
    p->insp = insp;
    return WEFT_INSPECT_OK;
}

static weft_prof_ring_watch_t *ring_slot(weft_prof_ctx_t *p, int seg_idx) {
    for (unsigned i = 0; i < p->ring_count; i++) {
        if (p->rings[i].seg_idx == seg_idx) return &p->rings[i];
    }
    return NULL;
}

int weft_prof_watch_ring(weft_prof_ctx_t *p, int seg_idx,
                         uint32_t pub_pid, uint32_t sub_pid) {
    if (p == NULL) return WEFT_INSPECT_ERR_INVALID;
    if (seg_idx < 0 || (unsigned)seg_idx >= p->insp->count) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    if (ring_slot(p, seg_idx) != NULL) return WEFT_INSPECT_OK; /* idempotent */
    if (p->ring_count >= WEFT_PROF_MAX_RINGS) return WEFT_INSPECT_ERR_FULL;

    const weft_inspect_segment_t *s = &p->insp->segs[seg_idx];
    if (s->family != WEFT_INSPECT_FAMILY_RMW_RING &&
        s->family != WEFT_INSPECT_FAMILY_CLUSTER_RING) {
        return WEFT_INSPECT_ERR_INVALID;
    }

    weft_prof_ring_watch_t *w = &p->rings[p->ring_count++];
    memset(w, 0, sizeof *w);
    w->seg_idx = seg_idx;
    w->pub_pid = pub_pid;
    w->sub_pid = sub_pid;
    if (sub_pid == 0u && s->family == WEFT_INSPECT_FAMILY_RMW_RING) {
        w->sub_pid = s->creator_pid;  /* ring creator IS the subscriber */
    }
    /* baseline the ledger at first watch so deltas start from zero */
    w->led_pub_base = s->published_total;
    w->led_drop_base = s->dropped_total;
    w->prev_head = s->head;
    w->prev_tail = s->tail_ack;
    w->prev_published = s->published_total;
    w->prev_dropped = s->dropped_total;
    return WEFT_INSPECT_OK;
}

int weft_prof_watch_region(weft_prof_ctx_t *p, const char *label,
                           const void *base, const uint32_t *offsets,
                           const uint8_t *owners, const uint32_t *owner_pids,
                           const char *const *labels, unsigned n) {
    if (p == NULL || label == NULL || base == NULL || offsets == NULL ||
        owners == NULL || n == 0u || n > 16u) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    int rid = -1;
    for (unsigned i = 0; i < WEFT_PROF_MAX_REGIONS; i++) {
        if (!p->regions[i].active) {
            rid = (int)i;
            break;
        }
    }
    if (rid < 0) return WEFT_INSPECT_ERR_FULL;
    if (p->word_count + n > WEFT_PROF_MAX_WORDS) return WEFT_INSPECT_ERR_FULL;

    weft_prof_region_t *r = &p->regions[rid];
    memset(r, 0, sizeof *r);
    snprintf(r->label, sizeof r->label, "%s", label);
    r->base = (uintptr_t)base;
    r->active = 1u;
    r->word_count = (uint8_t)n;

    for (unsigned i = 0; i < n; i++) {
        const void *addr = (const uint8_t *)base + offsets[i];
        if (((uintptr_t)addr & 7u) != 0u) return WEFT_INSPECT_ERR_INVALID;
        weft_prof_word_t *wd = &p->words[p->word_count++];
        memset(wd, 0, sizeof *wd);
        wd->addr = (const _Atomic uint64_t *)addr;
        wd->last = atomic_load_explicit(wd->addr, memory_order_relaxed);
        wd->off = offsets[i];
        wd->owner = owners[i];
        wd->owner_pid = (owner_pids != NULL) ? owner_pids[i] : 0u;
        wd->region = (uint8_t)rid;
        if (labels != NULL && labels[i] != NULL) {
            snprintf(wd->label, sizeof wd->label, "%s", labels[i]);
        }
    }
    return rid;
}

int weft_prof_unwatch_ring(weft_prof_ctx_t *p, int seg_idx) {
    if (p == NULL) return WEFT_INSPECT_ERR_INVALID;
    for (unsigned i = 0; i < p->ring_count; i++) {
        if (p->rings[i].seg_idx == seg_idx) {
            /* close any open stall into an honest (short) event first */
            if (p->rings[i].stall_state == 2u) {
                weft_prof_stall_event_t e;
                memset(&e, 0, sizeof e);
                e.start_ns = p->rings[i].stall_start_ns;
                e.end_ns = (uint64_t)weft_inspect_now_ns();
                e.duration_ns = e.end_ns - e.start_ns;
                e.seg_idx = (int32_t)seg_idx;
                e.kind = 1u;
                ev_push_stall(p, &e);
            }
            p->rings[i] = p->rings[p->ring_count - 1u];
            p->ring_count--;
            return WEFT_INSPECT_OK;
        }
    }
    return WEFT_INSPECT_ERR_NOENT;
}

/* ------------------------------------------------------------------ */
/* per-ring pass: frontier probe, stall machine, ledger                  */
/* ------------------------------------------------------------------ */

static void pass_ring(weft_prof_ctx_t *p, weft_prof_ring_watch_t *w,
                      unsigned widx) {
    const weft_inspect_segment_t *s = &p->insp->segs[w->seg_idx];
    uint64_t head = s->head, tail = s->tail_ack;
    uint64_t pub = s->published_total, drop = s->dropped_total;
    if (s->family == WEFT_INSPECT_FAMILY_CLUSTER_RING) {
        head = s->latest_seq;
        tail = s->latest_seq;   /* WFSH: no ack plane; in_flight = 0 */
        pub = s->publishes;
        drop = 0u;
    }
    uint64_t capacity = (s->slot_count >= 2u) ? (uint64_t)(s->slot_count - 2u)
                                              : 0u;
    uint64_t in_flight = (head >= tail) ? (head - tail) : 0u;
    int64_t now = weft_inspect_now_ns();

    /* capture this pass's mutation flags BEFORE prev_* refresh */
    w->mut_head = (head != w->prev_head) ? 1u : 0u;
    w->mut_tail = (tail != w->prev_tail) ? 1u : 0u;

    /* --- torn tracker: probe the write frontier ----------------------- */
    if (s->family == WEFT_INSPECT_FAMILY_RMW_RING && s->attached) {
        const rmw_ring_ctrl_t *c =
            (const rmw_ring_ctrl_t *)(const void *)(s->base +
                                                   RMW_WEFT_RING_HEADER_BYTES);
        uint64_t live_head = atomic_load_explicit(&c->head,
                                                  memory_order_acquire);
        if (live_head != 0u) {
            uint64_t k = live_head & (uint64_t)(s->slot_count - 1u);
            const rmw_ring_slot_t *slot =
                (const rmw_ring_slot_t *)(const void *)(s->base +
                        RMW_WEFT_RING_SLOTS_OFFSET + (size_t)k * s->slot_stride);
            w->torn_attempts++;
            uint64_t v1 = atomic_load_explicit(&slot->version,
                                               memory_order_acquire);
            if ((v1 & 1u) != 0u) {
                w->torn_events++;   /* writer mid-commit, observed live */
            } else {
                uint64_t seq = slot->seq_id;
#ifndef __SANITIZE_THREAD__
                /* hardware acquire fence; TSan's virtual machine tracks
                 * happens-before via its atomic interceptors and warns on
                 * inlined acquire fences — skip it under sanitization */
                atomic_thread_fence(memory_order_acquire);
#endif
                uint64_t v2 = atomic_load_explicit(&slot->version,
                                                   memory_order_acquire);
                if (v1 != v2 || seq == 0u) w->torn_events++;
            }
        }
    } else if (s->family == WEFT_INSPECT_FAMILY_CLUSTER_RING && s->attached) {
        const _Atomic uint64_t *lat =
            (const _Atomic uint64_t *)(const void *)(s->base + 64u);
        uint64_t live = atomic_load_explicit(lat, memory_order_acquire);
        if (live != 0u) {
            uint64_t k = live % (uint64_t)s->slot_count;
            /* RFC-0004: frame L publishes into slot (L-1) mod M; the
             * frontier being filled next is L mod M. slotSeq 0 = fill. */
            const _Atomic uint64_t *ss =
                (const _Atomic uint64_t *)(const void *)(s->base + 64u + 16u +
                                                        (size_t)k * 8u);
            w->torn_attempts++;
            uint64_t v = atomic_load_explicit(ss, memory_order_acquire);
            if (v == 0u) w->torn_events++;   /* invalidated: writer filling */
        }
    }

    /* --- stall machine (backpressure + producer ladder) --------------- */
    /* A STALL EPISODE is a contiguous window of frozen-head-over-full-ring:
     * the producer is pinned in its spin/sleep ladder. The episode ends
     * when the producer makes progress (head advanced — it finally pushed
     * a frame through the backpressure) OR the ring drains below full.
     * A ring that stays full while head keeps advancing is SATURATED BUT
     * FLOWING — pressure, not a stall (the pressure itself is visible as
     * in_flight_now == capacity in the ring stats). */
    int full = (in_flight >= capacity) && (capacity > 0u);
    int head_frozen = (head == w->prev_head);
    if (full && head_frozen) {
        w->frozen_passes++;
        if (w->stall_state < 2u && w->frozen_passes >= 2u) {
            /* open: the producer is pinned in its spin/sleep ladder */
            w->stall_state = 2u;
            w->stall_start_ns = (uint64_t)now;
            w->stall_head_at_start = head;
            w->stall_tail_at_start = tail;
            w->stall_max_in_flight = in_flight;
        } else if (w->stall_state == 2u) {
            if (in_flight > w->stall_max_in_flight) {
                w->stall_max_in_flight = in_flight;
            }
        }
    } else {
        if (w->stall_state == 2u) {
            /* recovery: head advanced or the consumer drained */
            weft_prof_stall_event_t e;
            memset(&e, 0, sizeof e);
            e.start_ns = w->stall_start_ns;
            e.end_ns = (uint64_t)now;
            e.duration_ns = e.end_ns - e.start_ns;
            e.head_at_start = w->stall_head_at_start;
            e.tail_at_start = w->stall_tail_at_start;
            e.max_in_flight = w->stall_max_in_flight;
            e.seg_idx = w->seg_idx;
            e.kind = 1u;
            ev_push_stall(p, &e);
            w->stalls++;
            w->stall_ns_total += e.duration_ns;
            /* the D-62 RELIABLE zero-progress budget default is 20 ms; a
             * stall at least that long is consistent with exactly one
             * publisher timeout window (declared estimate, not a count) */
            if (e.duration_ns >= 20000000ull) {
                w->reliable_timeout_windows++;
            }
            w->last_event_ns = (uint64_t)now;
        }
        /* not (full && frozen): flowing or drained — either way the
         * pinned-producer window, if open, has ended */
        w->stall_state = full ? 1u : 0u;
        w->frozen_passes = 0u;
    }

    /* --- drop ledger (exact interval accounting) ----------------------- */
    if (pub >= w->prev_published) {
        uint64_t pub_d = pub - w->prev_published;
        uint64_t drop_d = (drop >= w->prev_dropped) ? (drop - w->prev_dropped)
                                                    : 0u;
        w->published_cum += pub_d;
        w->dropped_cum += drop_d;
        w->writer_churn += pub_d;   /* commits observed this interval */
        if (drop_d > 0u) {
            w->drop_bursts++;
            weft_prof_drop_event_t e;
            memset(&e, 0, sizeof e);
            e.ts_ns = now;
            e.published_delta = pub_d;
            e.dropped_delta = drop_d;
            e.seg_idx = w->seg_idx;
            ev_push_drop(p, &e);
            w->last_event_ns = (uint64_t)now;
        }
    }

    /* --- false sharing: ctrl co-mutation (auto region) ----------------- */
    /* WFRM ctrl words: head@+0 (PUB), tail_ack@+8 (SUB), published@+16
     * (PUB), dropped@+24 (PUB). The PUB/SUB pair head x tail_ack shares
     * the ctrl block's first line by ring design — under live traffic
     * this MUST trip (the honest finding D-72 surfaces). */
    if (s->family == WEFT_INSPECT_FAMILY_RMW_RING) {
        if (w->mut_head && w->mut_tail) {
            /* the tripwire pair: different owners, adjacent offsets, one
             * line. Episode keyed by THIS watch index (auto region 0xFE,
             * wa = watch index — exact attribution, no pid matching). */
            p->fshare_co_mutations++;
            const rmw_ring_ctrl_t *c =
                (const rmw_ring_ctrl_t *)(const void *)(s->base +
                                                   RMW_WEFT_RING_HEADER_BYTES);
            uintptr_t lh = weft_inspect_line_base(&c->head);
            uintptr_t lt = weft_inspect_line_base(&c->tail_ack);
            if (lh == lt) {
                int matched = 0;
                for (unsigned i = 0; i < WEFT_PROF_MAX_FSHARE_EPISODES; i++) {
                    weft_prof_fshare_ep_t *ep = &p->fshare_eps[i];
                    if (!ep->active || ep->region != 0xFEu) continue;
                    if (ep->wa != (uint8_t)widx) continue;
                    ep->last_ns = (uint64_t)now;
                    ep->comutations++;
                    matched = 1;
                    break;
                }
                if (!matched) {
                    for (unsigned i = 0; i < WEFT_PROF_MAX_FSHARE_EPISODES;
                         i++) {
                        if (!p->fshare_eps[i].active) {
                            weft_prof_fshare_ep_t *ep = &p->fshare_eps[i];
                            memset(ep, 0, sizeof *ep);
                            ep->active = 1u;
                            ep->region = 0xFEu;   /* auto-ring marker */
                            ep->wa = (uint8_t)widx;  /* watch index */
                            ep->wb = 0u;
                            ep->start_ns = (uint64_t)now;
                            ep->last_ns = (uint64_t)now;
                            ep->comutations = 1u;
                            ep->line = lh;
                            ep->off_a = 0u;
                            ep->off_b = 8u;
                            ep->pid_a = w->pub_pid;
                            ep->pid_b = w->sub_pid;
                            break;
                        }
                    }
                }
            }
        }
    }

    w->prev_head = head;
    w->prev_tail = tail;
    w->prev_published = pub;
    w->prev_dropped = drop;
}

/* evolve generic-region false-sharing episodes */
static void pass_regions(weft_prof_ctx_t *p) {
    int64_t now = weft_inspect_now_ns();

    /* 1. load current values + collect mutated words per region */
    uint64_t cur[WEFT_PROF_MAX_WORDS];
    uint8_t mutated[WEFT_PROF_MAX_WORDS];
    for (unsigned i = 0; i < p->word_count; i++) {
        cur[i] = atomic_load_explicit(p->words[i].addr,
                                      memory_order_relaxed);
        mutated[i] = (cur[i] != p->words[i].last) ? 1u : 0u;
    }

    /* 2. pairwise co-mutation within each region: different owner, one
     *    line -> open/extend; absence -> close into an event */
    for (unsigned i = 0; i < p->word_count; i++) {
        if (!mutated[i]) continue;
        for (unsigned j = i + 1; j < p->word_count; j++) {
            if (!mutated[j]) continue;
            if (p->words[i].region != p->words[j].region) continue;
            if (p->words[i].owner == p->words[j].owner) continue;
            if (!weft_inspect_same_line(p->words[i].addr, p->words[j].addr)) {
                continue;
            }
            /* the tripwire pair (i, j): find or open its episode */
            weft_prof_fshare_ep_t *ep = NULL;
            for (unsigned k = 0; k < WEFT_PROF_MAX_FSHARE_EPISODES; k++) {
                weft_prof_fshare_ep_t *cand = &p->fshare_eps[k];
                if (!cand->active || cand->region != p->words[i].region) {
                    continue;
                }
                if ((cand->wa == (uint8_t)i && cand->wb == (uint8_t)j) ||
                    (cand->wa == (uint8_t)j && cand->wb == (uint8_t)i)) {
                    ep = cand;
                    break;
                }
            }
            if (ep == NULL) {
                for (unsigned k = 0; k < WEFT_PROF_MAX_FSHARE_EPISODES; k++) {
                    if (!p->fshare_eps[k].active) {
                        ep = &p->fshare_eps[k];
                        memset(ep, 0, sizeof *ep);
                        ep->active = 1u;
                        ep->region = p->words[i].region;
                        ep->wa = (uint8_t)i;
                        ep->wb = (uint8_t)j;
                        ep->start_ns = (uint64_t)now;
                        ep->comutations = 1u;
                        ep->line = weft_inspect_line_base(p->words[i].addr);
                        ep->off_a = p->words[i].off;
                        ep->off_b = p->words[j].off;
                        ep->pid_a = p->words[i].owner_pid;
                        ep->pid_b = p->words[j].owner_pid;
                        break;
                    }
                }
            } else {
                ep->last_ns = (uint64_t)now;
                ep->comutations++;
            }
            p->fshare_co_mutations++;
        }
    }

    /* 3. close episodes whose pair did NOT co-mutate this pass */
    for (unsigned k = 0; k < WEFT_PROF_MAX_FSHARE_EPISODES; k++) {
        weft_prof_fshare_ep_t *ep = &p->fshare_eps[k];
        if (!ep->active || ep->region == 0xFEu) continue;  /* auto handled */
        int alive = 0;
        for (unsigned i = 0; i < p->word_count && !alive; i++) {
            if (!mutated[i] || p->words[i].region != ep->region) continue;
            for (unsigned j = i + 1; j < p->word_count; j++) {
                if (!mutated[j] || p->words[j].region != ep->region) continue;
                if (p->words[i].owner == p->words[j].owner) continue;
                if (!weft_inspect_same_line(p->words[i].addr,
                                            p->words[j].addr)) continue;
                if ((ep->wa == (uint8_t)i && ep->wb == (uint8_t)j) ||
                    (ep->wa == (uint8_t)j && ep->wb == (uint8_t)i)) {
                    alive = 1;
                    break;
                }
            }
        }
        if (!alive) {
            weft_prof_fshare_event_t e;
            memset(&e, 0, sizeof e);
            e.start_ns = ep->start_ns;
            e.end_ns = (uint64_t)now;
            e.duration_ns = e.end_ns - e.start_ns;
            e.comutations = ep->comutations;
            e.line = ep->line;
            e.off_a = ep->off_a;
            e.off_b = ep->off_b;
            e.pid_a = ep->pid_a;
            e.pid_b = ep->pid_b;
            unsigned wi = ep->wa, wj = ep->wb;
            snprintf(e.label_a, sizeof e.label_a, "%s",
                     p->words[wi].label);
            snprintf(e.label_b, sizeof e.label_b, "%s",
                     p->words[wj].label);
            e.region = ep->region;
            ev_push_fshare(p, &e);
            ep->active = 0u;
        }
    }

    /* 4. auto-ring episodes (region 0xFE): close when the owning watch's
     *    pair stopped co-mutating THIS pass (flags captured in pass_ring
     *    before prev_* refresh — the exact same-pass view). */
    for (unsigned k = 0; k < WEFT_PROF_MAX_FSHARE_EPISODES; k++) {
        weft_prof_fshare_ep_t *ep = &p->fshare_eps[k];
        if (!ep->active || ep->region != 0xFEu) continue;
        if (ep->wa >= p->ring_count) {  /* watch removed: close now */
            ep->active = 0u;
            continue;
        }
        const weft_prof_ring_watch_t *w = &p->rings[ep->wa];
        if (!(w->mut_head && w->mut_tail)) {
            weft_prof_fshare_event_t e;
            memset(&e, 0, sizeof e);
            e.start_ns = ep->start_ns;
            e.end_ns = (uint64_t)now;
            e.duration_ns = e.end_ns - e.start_ns;
            e.comutations = ep->comutations;
            e.line = ep->line;
            e.off_a = ep->off_a;
            e.off_b = ep->off_b;
            e.pid_a = ep->pid_a;
            e.pid_b = ep->pid_b;
            snprintf(e.label_a, sizeof e.label_a, "head");
            snprintf(e.label_b, sizeof e.label_b, "tail_ack");
            e.region = 0xFEu;
            ev_push_fshare(p, &e);
            ep->active = 0u;
        }
    }

    /* 5. persist current values */
    for (unsigned i = 0; i < p->word_count; i++) {
        p->words[i].last = cur[i];
    }
}

int weft_prof_scrape(weft_prof_ctx_t *p) {
    if (p == NULL) return WEFT_INSPECT_ERR_INVALID;
    if (weft_inspect_scrape(p->insp) != WEFT_INSPECT_OK) {
        return WEFT_INSPECT_ERR_INVALID;
    }
    for (unsigned i = 0; i < p->ring_count; i++) {
        pass_ring(p, &p->rings[i], i);
    }
    pass_regions(p);
    p->passes++;
    return WEFT_INSPECT_OK;
}

/* ------------------------------------------------------------------ */
/* queries                                                              */
/* ------------------------------------------------------------------ */

int weft_prof_ring_stats(const weft_prof_ctx_t *p, int seg_idx,
                         weft_prof_ring_stats_t *out) {
    if (p == NULL || out == NULL) return WEFT_INSPECT_ERR_INVALID;
    const weft_prof_ring_watch_t *w = ring_slot((weft_prof_ctx_t *)p,
                                                seg_idx);
    if (w == NULL) return WEFT_INSPECT_ERR_NOENT;
    ring_stats_fill(w, out);
    const weft_inspect_segment_t *s = &p->insp->segs[w->seg_idx];
    if (s->family == WEFT_INSPECT_FAMILY_RMW_RING) {
        out->in_flight_now = (s->head >= s->tail_ack)
                                 ? (s->head - s->tail_ack) : 0u;
        out->capacity = (s->slot_count >= 2u) ? (uint64_t)(s->slot_count - 2u)
                                              : 0u;
    } else {
        out->in_flight_now = 0u;
        out->capacity = 0u;
    }
    return WEFT_INSPECT_OK;
}

unsigned weft_prof_fshare_active(const weft_prof_ctx_t *p) {
    if (p == NULL) return 0u;
    unsigned n = 0u;
    for (unsigned k = 0; k < WEFT_PROF_MAX_FSHARE_EPISODES; k++) {
        if (p->fshare_eps[k].active) n++;
    }
    return n;
}

unsigned weft_prof_drain_fshare(weft_prof_ctx_t *p,
                                weft_prof_fshare_event_t *out, unsigned max) {
    if (p == NULL || out == NULL) return 0u;
    unsigned n = (max < p->fshare_ev_count) ? max : p->fshare_ev_count;
    for (unsigned i = 0; i < n; i++) {
        out[i] = p->fshare_ev[p->fshare_ev_head];
        p->fshare_ev_head = (p->fshare_ev_head + 1u) % WEFT_PROF_EVENT_RING;
    }
    p->fshare_ev_count -= n;
    return n;
}

unsigned weft_prof_drain_stalls(weft_prof_ctx_t *p,
                                weft_prof_stall_event_t *out, unsigned max) {
    if (p == NULL || out == NULL) return 0u;
    unsigned n = (max < p->stall_ev_count) ? max : p->stall_ev_count;
    for (unsigned i = 0; i < n; i++) {
        out[i] = p->stall_ev[p->stall_ev_head];
        p->stall_ev_head = (p->stall_ev_head + 1u) % WEFT_PROF_EVENT_RING;
    }
    p->stall_ev_count -= n;
    return n;
}

unsigned weft_prof_drain_drops(weft_prof_ctx_t *p,
                               weft_prof_drop_event_t *out, unsigned max) {
    if (p == NULL || out == NULL) return 0u;
    unsigned n = (max < p->drop_ev_count) ? max : p->drop_ev_count;
    for (unsigned i = 0; i < n; i++) {
        out[i] = p->drop_ev[p->drop_ev_head];
        p->drop_ev_head = (p->drop_ev_head + 1u) % WEFT_PROF_EVENT_RING;
    }
    p->drop_ev_count -= n;
    return n;
}

uint64_t weft_prof_events_lost(const weft_prof_ctx_t *p) {
    if (p == NULL) return 0u;
    return p->fshare_ev_lost + p->stall_ev_lost + p->drop_ev_lost;
}
