// weft_trace.c — RFC 0016: continuous lock-free flight recorder, C reference.
//
// See weft_trace.h for the design contract. Implementation notes that ARE
// the contract (do not "optimize" away):
//
// 1. PUBLISH IS ONE RELEASE STORE. The slot payload words are written with
//    relaxed atomics (formally race-free, zero fence cost on every real
//    target), then the slot's `stamp` word flips odd -> even with ONE
//    release store. There is no second inter-thread write the producer
//    performs. The producer never READS shared mutable state.
//
// 2. THE STAMP IS A SEQLOCK PARITY TICKET: even value 2*t = ticket t is
//    stable and readable; odd 2*t+1 = the producer is writing. A consumer
//    that validated a stamp re-reads it after copying the payload and
//    retries ONCE before declaring the line lost (lossy-skip, counted in
//    t_lap). The flight recorder never waits on a writer — Law 1 on the
//    consumer side, too.
//
// 3. OVERWRITE-OLDEST IS ACCOUNTED AT RESYNC. Slot index = (ticket-1) &
//    mask, so a slot that should hold ticket t but holds t' > t means the
//    producer lapped: tickets [t .. t'-cap] are gone. missed = t' - cap +
//    1 - t, the drainer resyncs to t'-cap+1 and counts. Tickets are u32
//    modular (stamp holds 2*t, so ticket space is 2^31); distances are
//    normalized into (-2^30, 2^30).

#include "weft_trace.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef WEFT_TRACE_DEBUG
#include <pthread.h>
#endif

// ---------------------------------------------------------------------------
// Slot layout (32 bytes — WEFT_TRACE_SLOT_SIZE is asserted in the test)
// ---------------------------------------------------------------------------

typedef struct {
    _Atomic uint16_t kind;
    _Atomic uint16_t aux;
    _Atomic uint32_t data;
    _Atomic uint32_t producer;
    _Atomic uint64_t t_ns;
    _Atomic uint32_t stamp;   // seqlock parity ticket (see note 2)
    _Atomic uint32_t pad;
} weft_trace_slot;

#define TICKET_SPACE ((uint64_t)1 << 31)

static inline uint32_t tmod(uint64_t t) { return (uint32_t)(t & (TICKET_SPACE - 1)); }

/// Signed modular distance b - a in (-2^30, 2^30).
static inline int64_t tdist(uint32_t a, uint32_t b) {
    int64_t d = ((int64_t)b - (int64_t)a) & (TICKET_SPACE - 1);
    if (d >= (int64_t)(TICKET_SPACE / 2)) d -= (int64_t)TICKET_SPACE;
    return d;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static int shard_init(weft_trace_shard* s, uint32_t cap, int zeroed_already) {
    if (!zeroed_already) memset(s, 0, sizeof(*s));
    s->cap = cap;
    s->cap_mask = cap - 1;
    s->head = 0;
    s->c_ticket = 1;  // tickets are 1-based
    return 0;
}

int weft_trace_init(weft_trace_t* r, uint32_t cap) {
    if (r == NULL) return -1;
    if (cap < WEFT_TRACE_CAP_MIN) cap = WEFT_TRACE_CAP_MIN;
    if (cap > WEFT_TRACE_CAP_MAX ||
        (cap & (cap - 1)) != 0) return -1;  // power of two only
    memset(r, 0, sizeof(*r));
    for (uint32_t i = 0; i < WEFT_TRACE_SHARDS_MAX; i++) {
        weft_trace_slot* mem = NULL;
        int rc = posix_memalign((void**)&mem, 64,
                                (size_t)cap * WEFT_TRACE_SLOT_SIZE);
        if (rc != 0 || mem == NULL) {
            // unwind the shards allocated so far
            weft_trace_destroy(r);
            return -1;
        }
        memset(mem, 0, (size_t)cap * WEFT_TRACE_SLOT_SIZE);
        shard_init(&r->shard[i], cap, 1);
        r->shard[i].slots = (weft_trace_obs*)mem;  // opaque storage pointer
        r->shard_used[i] = 1;
        r->shard_owned[i] = 1;
    }
    return 0;
}

/// Placement init (mmap host path, RFC-0016 §6): the caller provides the
/// slot storage for every shard. Nothing is allocated.
int weft_trace_init_at(weft_trace_t* r, uint32_t cap, void* storage,
                       size_t storage_len) {
    if (r == NULL || storage == NULL) return -1;
    if (cap < WEFT_TRACE_CAP_MIN) cap = WEFT_TRACE_CAP_MIN;
    if (cap > WEFT_TRACE_CAP_MAX || (cap & (cap - 1)) != 0) return -1;
    size_t need = (size_t)WEFT_TRACE_SHARDS_MAX * cap * WEFT_TRACE_SLOT_SIZE;
    if (storage_len < need) return -1;
    memset(r, 0, sizeof(*r));
    uint8_t* p = (uint8_t*)storage;
    memset(p, 0, need);
    for (uint32_t i = 0; i < WEFT_TRACE_SHARDS_MAX; i++) {
        shard_init(&r->shard[i], cap, 1);
        r->shard[i].slots = (weft_trace_obs*)(p + (size_t)i * cap * WEFT_TRACE_SLOT_SIZE);
        r->shard_used[i] = 1;
        r->shard_owned[i] = 0;  // caller's memory — destroy must not free
    }
    return 0;
}

void weft_trace_destroy(weft_trace_t* r) {
    if (r == NULL) return;
    for (uint32_t i = 0; i < WEFT_TRACE_SHARDS_MAX; i++) {
        if (r->shard_used[i] && r->shard_owned[i] &&
            r->shard[i].slots != NULL) {
            free(r->shard[i].slots);
        }
        r->shard[i].slots = NULL;
        r->shard_used[i] = 0;
        r->shard_owned[i] = 0;
    }
}

// ---------------------------------------------------------------------------
// Emission (producer thread only)
// ---------------------------------------------------------------------------

#ifdef WEFT_TRACE_DEBUG
static _Atomic pthread_t shard_owner[WEFT_TRACE_SHARDS_MAX];
static _Atomic uint64_t binding_violations;
static void binding_check(uint32_t producer) {
    pthread_t self = pthread_self();
    pthread_t expected = atomic_load(&shard_owner[producer]);
    if (expected == 0) {
        atomic_compare_exchange_strong(&shard_owner[producer], &expected, self);
    }
    // A different thread on a bound shard is the §2 caller bug. The debug
    // build COUNTS the violation (test-observable, weft_trace_debug_binding_
    // violations) instead of trapping mid-flight — the release build is
    // zero-cost and defines the behavior as advisory telemetry (§2).
    if (expected != 0 && !pthread_equal(expected, self)) {
        atomic_fetch_add(&binding_violations, 1);
    }
}
uint64_t weft_trace_debug_binding_violations(void) {
    return atomic_load(&binding_violations);
}
#else
static inline void binding_check(uint32_t producer) { (void)producer; }
#endif

void weft_trace_emit(weft_trace_t* r, uint32_t producer, uint16_t kind,
                     uint16_t aux, uint32_t data, uint64_t t_ns) {
    if (r == NULL || producer >= WEFT_TRACE_SHARDS_MAX) return;
    binding_check(producer);
    weft_trace_shard* s = &r->shard[producer];
    weft_trace_slot* slots = (weft_trace_slot*)s->slots;
    uint64_t ticket = s->head + 1;  // 1-based
    weft_trace_slot* sl = &slots[(uint32_t)((ticket - 1) & s->cap_mask)];

    // begin: odd stamp (relaxed — ordering is provided by the publish store)
    atomic_store_explicit(&sl->stamp, (uint32_t)(2 * tmod(ticket)) | 1u,
                          memory_order_relaxed);
    atomic_store_explicit(&sl->kind, kind, memory_order_relaxed);
    atomic_store_explicit(&sl->aux, aux, memory_order_relaxed);
    atomic_store_explicit(&sl->data, data, memory_order_relaxed);
    atomic_store_explicit(&sl->producer, producer, memory_order_relaxed);
    atomic_store_explicit(&sl->t_ns, t_ns, memory_order_relaxed);
    // publish: even stamp, ONE release store (note 1)
    atomic_store_explicit(&sl->stamp, (uint32_t)(2 * tmod(ticket)),
                          memory_order_release);

    s->head = ticket;  // producer-private
    atomic_fetch_add_explicit(&r->t_emit, 1, memory_order_relaxed);
}

void weft_trace_emit_kernel(weft_trace_t* r, uint32_t producer,
                            const weft_trace_event* ev, uint64_t t_ns) {
    if (ev == NULL) return;
    weft_trace_emit(r, producer, ev->kind, ev->aux, ev->data, t_ns);
}

// ---------------------------------------------------------------------------
// Shard front: decode ticket c_ticket WITHOUT consuming (resync/lossy-skip
// may advance c_ticket — that is accounting, not consumption). Returns 1
// and fills *out when a stable event is available; 0 when caught up.
// ---------------------------------------------------------------------------

static int shard_front(weft_trace_shard* s, weft_trace_obs* out) {
    weft_trace_slot* slots = (weft_trace_slot*)s->slots;
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t idx = (uint32_t)((s->c_ticket - 1) & s->cap_mask);
        weft_trace_slot* sl = &slots[idx];
        uint32_t st1 = atomic_load_explicit(&sl->stamp, memory_order_acquire);
        if (st1 & 1u) {  // producer mid-write on our line
            if (attempt == 0) { s->t_torn++; continue; }
            // lossy-skip: the line is hot; declare this ticket unreadable.
            s->c_ticket++;
            s->t_lap++;
            return 0;
        }
        uint32_t ticket = st1 >> 1;
        uint32_t expected = tmod(s->c_ticket);
        int64_t d = tdist(expected, ticket);
        if (d == 0) {
            weft_trace_obs o;
            o.kind = atomic_load_explicit(&sl->kind, memory_order_relaxed);
            o.aux = atomic_load_explicit(&sl->aux, memory_order_relaxed);
            o.data = atomic_load_explicit(&sl->data, memory_order_relaxed);
            o.producer = atomic_load_explicit(&sl->producer, memory_order_relaxed);
            o.t_ns = atomic_load_explicit(&sl->t_ns, memory_order_relaxed);
            o.back_ref = WEFT_SIDECAR_NO_REF;  // export assigns
            uint32_t st2 = atomic_load_explicit(&sl->stamp, memory_order_acquire);
            if (st2 != st1) {  // torn during copy
                if (attempt == 0) { s->t_torn++; continue; }
                s->c_ticket++;
                s->t_lap++;
                return 0;
            }
            *out = o;
            return 1;
        }
        if (d > 0) {
            // Lap (note 3): same slot index ⇒ d is a positive multiple of
            // cap. This slot's ticket is NOT necessarily the newest — the
            // resync target must be derived from the shard's NEWEST ticket
            // (max over all slots, modular distance ahead of c_ticket),
            // otherwise the walk converges one slot at a time and the lap
            // accounting goes wrong. O(cap) once per lap — consumer side.
            uint32_t newest = tmod(s->c_ticket);  // d>0 ⇒ strictly ahead
            for (uint32_t i = 0; i < s->cap; i++) {
                uint32_t st = atomic_load_explicit(&slots[i].stamp,
                                                   memory_order_acquire);
                if (st & 1u) continue;  // in-flight; skip
                uint32_t t_i = st >> 1;
                if (tdist(tmod(s->c_ticket), t_i) >
                    tdist(tmod(s->c_ticket), newest)) {
                    newest = t_i;
                }
            }
            int64_t ahead = tdist(tmod(s->c_ticket), newest);
            if (ahead >= (int64_t)s->cap) {
                uint64_t missed = (uint64_t)ahead - s->cap + 1;
                // resync point = newest - cap + 1, reached by ADVANCING
                // c_ticket by `missed` (u64 epoch preserved).
                s->c_ticket += missed;
                s->t_lap += missed;
            }
            if (attempt == 0) continue;  // re-read at the resync point
            return 0;
        }
        return 0;  // d < 0: caught up (slot holds an older ticket)
    }
    return 0;
}

static void shard_take(weft_trace_shard* s) { s->c_ticket++; }

// ---------------------------------------------------------------------------
// K-way merge drain (consumer; RFC-0016 §5)
// ---------------------------------------------------------------------------

typedef struct {
    weft_trace_shard* sh;
    weft_trace_obs front;
    int valid;
} merge_cursor;

static int cursor_refill(merge_cursor* c) {
    c->valid = shard_front(c->sh, &c->front);
    return c->valid;
}

size_t weft_trace_drain(weft_trace_t* r, weft_trace_obs* out, size_t out_cap) {
    if (r == NULL || out == NULL || out_cap == 0) return 0;
    merge_cursor cur[WEFT_TRACE_SHARDS_MAX];
    int k = 0;
    for (uint32_t i = 0; i < WEFT_TRACE_SHARDS_MAX; i++) {
        if (!r->shard_used[i]) continue;
        cur[k].sh = &r->shard[i];
        cursor_refill(&cur[k]);
        k++;
    }
    size_t n = 0;
    while (n < out_cap) {
        int best = -1;
        for (int i = 0; i < k; i++) {
            if (!cur[i].valid) continue;
            if (best < 0) { best = i; continue; }
            // order: (t_ns, producer); stable in-shard arrival by
            // construction (one front per shard, taken in ticket order)
            if (cur[i].front.t_ns < cur[best].front.t_ns ||
                (cur[i].front.t_ns == cur[best].front.t_ns &&
                 cur[i].front.producer < cur[best].front.producer)) {
                best = i;
            }
        }
        if (best < 0) break;
        out[n++] = cur[best].front;
        shard_take(cur[best].sh);
        cursor_refill(&cur[best]);
    }
    atomic_fetch_add_explicit(&r->t_drain, 1, memory_order_relaxed);
    return n;
}

// ---------------------------------------------------------------------------
// Export (RFC-0016 §6)
// ---------------------------------------------------------------------------

static int is_kernel_kind(uint16_t kind) {
    return kind >= WEFT_TRACE_PUBLISH && kind <= WEFT_TRACE_CANARY_FAIL;
}

/// Non-destructive peek-walk: counts (total, kernel) observations available
/// from each shard's c_ticket, bounded by the shard capacity. Exact when
/// quiesced; advisory under live emission (declared).
static void shard_count(weft_trace_shard* s, size_t* total, size_t* kernel) {
    weft_trace_slot* slots = (weft_trace_slot*)s->slots;
    uint64_t t = s->c_ticket;
    for (uint32_t i = 0; i < s->cap; i++) {
        uint32_t idx = (uint32_t)((t - 1) & s->cap_mask);
        weft_trace_slot* sl = &slots[idx];
        uint32_t st1 = atomic_load_explicit(&sl->stamp, memory_order_acquire);
        if (st1 & 1u) break;  // in-flight: stop (conservative)
        int64_t d = tdist(tmod(t), st1 >> 1);
        if (d != 0) break;  // lap or caught up: stop (conservative)
        uint16_t kind = atomic_load_explicit(&sl->kind, memory_order_relaxed);
        (*total)++;
        if (is_kernel_kind(kind)) (*kernel)++;
        t++;
    }
}

size_t weft_trace_pending(weft_trace_t* r) {
    size_t total = 0, kernel = 0;
    if (r == NULL) return 0;
    for (uint32_t i = 0; i < WEFT_TRACE_SHARDS_MAX; i++) {
        if (!r->shard_used[i]) continue;
        shard_count(&r->shard[i], &total, &kernel);
    }
    return total;
}

size_t weft_trace_needed_v4(size_t kernel_events) {
    return WEFT_TRACE_HDR_SIZE + 12u * kernel_events;
}

size_t weft_trace_needed_sidecar(size_t obs_n) {
    return WEFT_SIDECAR_HDR_SIZE + 20u * obs_n;
}

size_t weft_trace_export_v4(weft_trace_t* r, uint8_t* buf, size_t cap,
                            weft_trace_obs* obs_out, size_t obs_cap,
                            size_t* obs_n) {
    if (obs_n) *obs_n = 0;
    if (r == NULL || buf == NULL || obs_out == NULL) return 0;

    // Phase A: non-destructive counts (exact when quiesced).
    size_t total = 0, kernel = 0;
    for (uint32_t i = 0; i < WEFT_TRACE_SHARDS_MAX; i++) {
        if (!r->shard_used[i]) continue;
        shard_count(&r->shard[i], &total, &kernel);
    }
    // Capacity gate BEFORE any consumption — nothing is ever half-taken.
    if (cap < weft_trace_needed_v4(kernel) || obs_cap < total) return 0;

    // Phase B: consume + write. Events that arrive after Phase A simply
    // stay pending (we stop at the verified capacities — never lost).
    weft_trace_obs* obs = obs_out;
    size_t n = weft_trace_drain(r, obs, obs_cap < total ? obs_cap : total);
    weft_trace_writer w;
    weft_trace_writer_open(&w, buf, cap);
    uint32_t v4_index = 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_kernel_kind(obs[i].kind)) continue;
        weft_trace_event ev = {obs[i].kind, obs[i].aux, obs[i].data};
        weft_trace_writer_event(&w, &ev);
        obs[i].back_ref = v4_index++;
    }
    weft_trace_writer_close(&w);
    if (obs_n) *obs_n = n;
    return w.off;
}

size_t weft_trace_export_sidecar(const weft_trace_obs* obs, size_t obs_n,
                                 uint8_t* buf, size_t cap, uint64_t t_epoch_ns) {
    if (buf == NULL || obs == NULL) return 0;
    size_t need = weft_trace_needed_sidecar(obs_n);
    if (cap < need) return 0;
    memset(buf, 0, WEFT_SIDECAR_HDR_SIZE);
    uint32_t magic = WEFT_SIDECAR_MAGIC, ver = WEFT_SIDECAR_VERSION;
    memcpy(buf + 0, &magic, 4);
    memcpy(buf + 4, &ver, 4);
    // flags(8..12) = 0, reserved(12..16) = 0
    uint32_t count = (uint32_t)obs_n;
    memcpy(buf + 16, &count, 4);
    uint32_t crc = weft_trace_crc32(buf, 20);
    memcpy(buf + 20, &crc, 4);
    uint64_t epoch = t_epoch_ns;
    memcpy(buf + 24, &epoch, 8);
    uint8_t* p = buf + WEFT_SIDECAR_HDR_SIZE;
    for (size_t i = 0; i < obs_n; i++) {
        memcpy(p + 0, &obs[i].t_ns, 8);
        uint32_t ref = obs[i].back_ref;
        memcpy(p + 8, &ref, 4);
        memcpy(p + 12, &obs[i].producer, 2);
        memcpy(p + 14, &obs[i].kind, 2);
        memcpy(p + 16, &obs[i].data, 4);
        p += WEFT_SIDECAR_REC_SIZE;
    }
    return need;
}
