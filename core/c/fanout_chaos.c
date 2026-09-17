// fanout_chaos.c — RFC 0011 deterministic chaos engine, C reference (oracle).
//
// Every port (TS, JVM/Kotlin, Dart, Swift, Rust) of this engine MUST emit
// the byte-identical stepped verdict JSON for the same config. The normative
// contract lives in fanout_chaos.h — read it first. Any edit here that
// changes a PRNG draw order, a freeze rule, an SM transition, or the JSON
// field order is a BREAKING CONTRACT CHANGE: bump "v", update every port,
// and regenerate the golden fixtures in tools/guardian/fixtures/.
//
// "chaos contract" is the cross-port grep anchor (see run_chaos_parity.sh).

#include "fanout_chaos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef WEFT_FANOUT_CHAOS_FREE_MODE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#endif

#include "fanout.h"
#include "weft.h"

// ---------------------------------------------------------------------------
// The chaos contract: PRNG + pattern (normative — mirror exactly)
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t a, b, c, d;
} chaos_rng_t;

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/// Marsaglia xorshift128 — the chaos contract PRNG (u32 wrap, logical
/// right shifts). One stream per deterministic agent; see the derivations.
static uint32_t chaos_next(chaos_rng_t* r) {
    uint32_t t = r->d;
    uint32_t s = r->a;
    r->d = r->c;
    r->c = r->b;
    r->b = s;
    t ^= t << 11;
    t ^= t >> 8;
    r->a = t ^ s ^ (s >> 10);
    return r->a;
}

static void chaos_seed(chaos_rng_t* r, uint32_t seed) {
    r->a = mix32(seed ^ 0xA341316Cu);
    r->b = mix32(seed ^ 0xC8013EA4u);
    r->c = r->a ^ 0x9E3779B9u;
    r->d = r->b ^ 0x85EBCA6Bu;
}

/// Deterministic payload word (04-LITMUS §0.1 mixer family — the same
/// generator the torture runner and the interop fixtures use).
static uint32_t tword(uint32_t seq, uint32_t w) {
    return mix32(seq * 2654435761u + w);
}

static int cfg_validate(const weft_chaos_config_t* c) {
    if (c->slots < 2 || c->slots > WEFT_FANOUT_MAX_SLOTS) return -1;
    if (c->words < 1 || c->words > 64) return -1;
    if (c->readers < 1 || c->readers > 4) return -1;
    if (c->chaos_rate > 1000) return -1;
    return 0;
}

// ---------------------------------------------------------------------------
// STEPPED engine — the deterministic scheduler
// ---------------------------------------------------------------------------

typedef enum { W_IDLE = 0, W_FILL, W_STAMP, W_PUBLISH, W_DONE } w_state_t;

typedef enum {
    R_IDLE = 0, R_STAMP_B, R_COPY, R_STAMP_A,
    R_ACCEPT, R_TICK_END, R_SKIP_TICK, R_EXHAUSTED_TICK, R_DONE
} r_state_t;

typedef struct {
    // Model ring (u64 stamps mirror the real ctrl layout semantics)
    uint64_t latest;
    uint64_t publishes;
    uint64_t slot_seq[WEFT_FANOUT_MAX_SLOTS];
    uint32_t payload[WEFT_FANOUT_MAX_SLOTS][64];
    bool     bracket_open[WEFT_FANOUT_MAX_SLOTS];

    // Writer SM
    w_state_t ws;
    uint64_t  w_seq;      // next frame to publish (1-based)
    unsigned  w_slot;
    unsigned  w_word;     // fill progress
    bool      w_rev;      // current frame fills in reversed order

    // Reader SMs
    r_state_t rs[4];
    uint64_t  r_last[4];
    uint64_t  r_target[4][64];  // copy buffer (values, not u32 — parity)
    uint64_t  r_l[4];           // candidate seq this tick
    unsigned  r_slot[4];
    unsigned  r_word[4];
    unsigned  r_attempts[4];
    bool      r_rev[4];         // reorder flag toggled by injections
    bool      r_rev_active[4];  // reorder active for the copy in progress

    // Scheduler
    chaos_rng_t rng;
    uint32_t    freeze[5];      // readers + 1 (writer)
    bool        rev_flag[5];    // sticky reorder toggles
} chaos_model_t;

typedef struct {
    const weft_chaos_config_t* cfg;
    chaos_model_t m;
    weft_chaos_ledger_t* led;
} stepped_t;

/// One scheduler step's fault draw — shared shape with every port.
static void fault_draw(stepped_t* s) {
    chaos_rng_t* rng = &s->m.rng;
    if (chaos_next(rng) % 1000u >= s->cfg->chaos_rate) return;
    uint32_t victim = chaos_next(rng) % (s->cfg->readers + 1u);
    uint32_t kind   = chaos_next(rng) % 4u;
    static const uint32_t freeze_add[4] = { 1, 2, 3, 0 }; // preempt/stall/throttle/reorder
    s->m.freeze[victim] += freeze_add[kind];
    if (kind == WEFT_CHAOS_REORDER) s->m.rev_flag[victim] = !s->m.rev_flag[victim];
    s->led->injected[kind]++;
}

static void writer_step(stepped_t* s) {
    chaos_model_t* m = &s->m;
    weft_chaos_ledger_t* led = s->led;
    const uint32_t M = s->cfg->slots, W = s->cfg->words;
    switch (m->ws) {
    case W_IDLE:
        if (m->w_seq > s->cfg->frames) { m->ws = W_DONE; return; }
        m->w_slot = (unsigned)((m->w_seq - 1) % M);
        if (m->bracket_open[m->w_slot]) {
            // L-C6: invalidating a slot whose previous frame never finished
            // means the writer wrapped onto an OPEN bracket — the
            // single-writer contract makes this impossible (the slot it
            // reuses is always the oldest fully published one).
            led->bracket_violations++;
        }
        m->slot_seq[m->w_slot] = 0;                    // FI1a: invalidate FIRST
        m->bracket_open[m->w_slot] = true;
        m->w_rev = m->rev_flag[0];                     // fault axis: reorder
        m->w_word = m->w_rev ? (W - 1) : 0;
        m->ws = W_FILL;
        return;
    case W_FILL:
        m->payload[m->w_slot][m->w_word] = tword((uint32_t)m->w_seq, m->w_word);
        if (m->w_rev) {
            if (m->w_word == 0) m->ws = W_STAMP;
            else m->w_word--;
        } else {
            m->w_word++;
            if (m->w_word == W) m->ws = W_STAMP;
        }
        return;
    case W_STAMP:
        m->slot_seq[m->w_slot] = m->w_seq;             // FI1b: stamp (Release)
        m->bracket_open[m->w_slot] = false;
        m->ws = W_PUBLISH;
        return;
    case W_PUBLISH:
        m->latest = m->w_seq;                          // the publication point
        m->publishes++;
        m->w_seq++;
        m->ws = W_IDLE;
        return;
    case W_DONE:
        return;
    }
}

/// Reader claim internals — mirrors weft_fanout_claim's bounded loop
/// exactly (4 iterations; each retry consumes one).
static void reader_step(stepped_t* s, uint32_t i) {
    chaos_model_t* m = &s->m;
    weft_chaos_ledger_t* led = s->led;
    const uint32_t M = s->cfg->slots, W = s->cfg->words;
    switch (m->rs[i]) {
    case R_IDLE: {
        uint64_t L = m->latest;
        if (L == 0 || L == m->r_last[i]) { m->rs[i] = R_TICK_END; return; }
        m->r_l[i] = L;
        m->r_attempts[i] = 0;
        m->rs[i] = R_STAMP_B;
        return;
    }
    case R_STAMP_B: {
        uint64_t L = m->r_l[i];
        m->r_slot[i] = (unsigned)((L - 1) % M);
        uint64_t sB = m->slot_seq[m->r_slot[i]];
        if (sB != L) {
            uint64_t l2 = m->latest;
            if (l2 == L) { led->skips[i]++; m->rs[i] = R_SKIP_TICK; return; }
            m->r_l[i] = l2;
            m->r_attempts[i]++;
            m->rs[i] = (m->r_attempts[i] >= 4) ? R_EXHAUSTED_TICK : R_STAMP_B;
            return;
        }
        m->r_word[i] = m->rev_flag[i + 1] ? (W - 1) : 0; // fault axis: reorder
        m->r_rev_active[i] = m->rev_flag[i + 1];
        m->rs[i] = R_COPY;
        return;
    }
    case R_COPY: {
        uint32_t w = m->r_word[i];
        m->r_target[i][w] = m->payload[m->r_slot[i]][w];
        if (m->r_rev_active[i]) {
            if (w == 0) m->rs[i] = R_STAMP_A;
            else m->r_word[i] = w - 1;
        } else {
            m->r_word[i]++;
            if (m->r_word[i] == W) m->rs[i] = R_STAMP_A;
        }
        return;
    }
    case R_STAMP_A: {
        uint64_t sA = m->slot_seq[m->r_slot[i]];
        if (sA == m->r_l[i]) { m->rs[i] = R_ACCEPT; return; }
        // torn copy — the revalidation caught it; retry on the newest frame
        m->r_l[i] = m->latest;
        m->r_attempts[i]++;
        m->rs[i] = (m->r_attempts[i] >= 4) ? R_EXHAUSTED_TICK : R_STAMP_B;
        return;
    }
    case R_ACCEPT: {
        uint64_t L = m->r_l[i];
        if (L > s->cfg->frames) led->future_claims++;      // L-C2
        for (uint32_t w = 0; w < W; w++) {
            if (m->r_target[i][w] != tword((uint32_t)L, w)) {
                led->torn_accepted++;                       // L-C1
                break;
            }
        }
        led->sum_dropped[i] += L - m->r_last[i] - 1;
        m->r_last[i] = L;
        led->fresh_claims[i]++;
        m->rs[i] = R_TICK_END;
        return;
    }
    case R_TICK_END:
        if (m->ws == W_DONE && m->r_last[i] == s->cfg->frames) {
            m->rs[i] = R_DONE;
        } else {
            m->rs[i] = R_IDLE;
        }
        return;
    case R_SKIP_TICK:
        m->rs[i] = R_TICK_END;
        return;
    case R_EXHAUSTED_TICK:
        led->exhausted[i]++;
        m->rs[i] = R_TICK_END;
        return;
    case R_DONE:
        return;
    }
}

static void advance(stepped_t* s, uint32_t tid) {
    if (tid == 0) writer_step(s);
    else reader_step(s, tid - 1);
}

static bool all_done(const stepped_t* s) {
    if (s->m.ws != W_DONE) return false;
    for (uint32_t i = 0; i < s->cfg->readers; i++) {
        if (s->m.rs[i] != R_DONE) return false;
    }
    return true;
}

int weft_chaos_run_stepped(const weft_chaos_config_t* cfg,
                           weft_chaos_verdict_t* out) {
    if (!out) return 2;
    memset(out, 0, sizeof(*out));
    snprintf(out->engine, sizeof(out->engine), "stepped");
    if (cfg_validate(cfg) != 0) return 2;

    static stepped_t s; // single fixed model block (Law 2: no per-run heap)
    memset(&s, 0, sizeof(s));
    s.cfg = cfg;
    s.led = &out->ledger;

    chaos_model_t* m = &s.m;
    chaos_seed(&m->rng, cfg->seed);
    m->w_seq = 1;
    m->ws = W_IDLE;
    for (uint32_t i = 0; i < cfg->readers; i++) m->rs[i] = R_IDLE;

    // The scheduler loop (chaos contract shape — ports copy this verbatim).
    while (s.led->steps_executed < cfg->steps && !all_done(&s)) {
        uint32_t tid = chaos_next(&m->rng) % (cfg->readers + 1u);
        fault_draw(&s);
        if (m->freeze[tid] > 0) {
            m->freeze[tid]--;               // scheduled, but made no progress
        } else {
            advance(&s, tid);
        }
        s.led->steps_executed++;
    }

    // Drain: round-robin (writer, reader 1..R), no faults, until done. The
    // drain MUST terminate (L-C5); its bound is generous and mechanical.
    uint64_t drain_bound = 64ull * (cfg->frames + 16ull * cfg->readers *
                                    (cfg->frames + 8)) + 64;
    uint64_t drained_steps = 0;
    while (!all_done(&s) && drained_steps < drain_bound) {
        for (uint32_t tid = 0; tid <= cfg->readers && !all_done(&s); tid++) {
            if (m->freeze[tid] > 0) m->freeze[tid] = 0; // faults end with the budget
            if (!all_done(&s)) advance(&s, tid);
        }
        drained_steps++;
    }
    out->ledger.drained = all_done(&s);

    // Sync the per-reader final anchors into the ledger (the model owns the
    // live copies; the ledger is the serializable surface).
    for (uint32_t i = 0; i < cfg->readers; i++) {
        out->ledger.last_seq[i] = m->r_last[i];
    }
    out->ledger.publishes = m->publishes;

    // L-C3 telescoping + L-C4 completion (adjudicated once, at drain end).
    bool telescoping_ok = out->ledger.drained;
    for (uint32_t i = 0; i < cfg->readers; i++) {
        if (out->ledger.sum_dropped[i] !=
            out->ledger.last_seq[i] - out->ledger.fresh_claims[i]) {
            telescoping_ok = false;
        }
        if (out->ledger.drained && out->ledger.last_seq[i] != cfg->frames) {
            telescoping_ok = false; // a drained reader ended on the final frame
        }
    }
    if (out->ledger.publishes != cfg->frames) telescoping_ok = false; // L-C4

    out->pass = telescoping_ok &&
                out->ledger.torn_accepted == 0 &&
                out->ledger.future_claims == 0 &&
                out->ledger.bracket_violations == 0;
    return out->pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// FREE engine — real threads, seed-deterministic fault sequence
// ---------------------------------------------------------------------------

#ifdef WEFT_FANOUT_CHAOS_FREE_MODE

/// Per-thread fault state. The fault SEQUENCE is seed-deterministic (one
/// xorshift128 stream per thread — derived below); which wall-clock
/// interleavings those faults land on is the OS's business. That is the
/// point: replay the faults, resample the interleavings.
typedef struct {
    chaos_rng_t rng;
    uint32_t    rate;
    bool        reorder;
    uint64_t    injected[WEFT_CHAOS_KINDS];
} chaos_tls_t;

static void chaos_tls_seed(chaos_tls_t* t, uint32_t seed, uint32_t tid,
                           uint32_t rate) {
    // Per-thread stream derivation (normative): fold the thread id into the
    // same constants the master derivation uses, so thread k's stream is
    // reproducible from (seed, k) alone.
    t->rng.a = mix32(seed ^ (0xA341316Cu + tid));
    t->rng.b = mix32(seed ^ (0xC8013EA4u + tid));
    t->rng.c = t->rng.a ^ 0x9E3779B9u;
    t->rng.d = t->rng.b ^ 0x85EBCA6Bu;
    t->rate = rate;
    t->reorder = false;
    memset(t->injected, 0, sizeof(t->injected));
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/// A chaos point inside a real thread's loop. Mirrors the stepped fault
/// classes with wall-clock analogs: PREEMPT yields the CPU, STALL spins on
/// an acquire load (a cache-line round-trip storm), THROTTLE sleeps a
/// seeded sub-millisecond slice, REORDER flips this thread's word order.
static void chaos_point(chaos_tls_t* t) {
    if (chaos_next(&t->rng) % 1000u >= t->rate) return;
    uint32_t kind = chaos_next(&t->rng) % 4u;
    t->injected[kind]++;
    switch (kind) {
    case WEFT_CHAOS_PREEMPT:
        sched_yield();
        break;
    case WEFT_CHAOS_STALL: {
        uint32_t iters = chaos_next(&t->rng) % 512u;
        extern uint8_t* weft_chaos_stall_line(void);
        for (uint32_t i = 0; i < iters; i++) {
            (void)atomic_load_explicit((const volatile _Atomic uint8_t*)weft_chaos_stall_line(),
                                       memory_order_acquire);
        }
        break;
    }
    case WEFT_CHAOS_THROTTLE: {
        uint32_t us = chaos_next(&t->rng) % 400u;
        struct timespec ts = { 0, (long)us * 1000L };
        nanosleep(&ts, NULL);
        break;
    }
    case WEFT_CHAOS_REORDER:
        t->reorder = !t->reorder;
        break;
    default:
        break;
    }
}

/// A shared, process-lifetime stall line for the bus-stall analog (one
/// contended cache line every thread spins on — the STALL class).
static uint8_t g_stall_line[64];
uint8_t* weft_chaos_stall_line(void) { return g_stall_line; }

typedef struct {
    weft_fanout_t*         f;
    weft_chaos_config_t    cfg;
    chaos_tls_t            tls;
    uint32_t*              src;        // writer pattern buffer
    uint64_t               injected[WEFT_CHAOS_KINDS];
} free_writer_t;

typedef struct {
    weft_fanout_reader_t*  r;
    weft_chaos_config_t    cfg;
    chaos_tls_t            tls;
    uint64_t               torn_accepted, future_claims;
    uint64_t               injected[WEFT_CHAOS_KINDS];
    int                    converged;
} free_reader_t;

static void* free_writer_fn(void* argp) {
    free_writer_t* a = (free_writer_t*)argp;
    const uint32_t W = a->cfg.words;
    for (uint32_t seq = 1; seq <= a->cfg.frames; seq++) {
        chaos_point(&a->tls);
        uint8_t* cur = weft_fanout_begin(a->f);
        chaos_point(&a->tls); // faults BETWEEN begin and publish: mid-bracket
        for (uint32_t w = 0; w < W; w++) a->src[w] = tword(seq, w);
        if (a->tls.reorder) {
            // REORDER axis: store the frame's words in REVERSED order via
            // the raw cursor (the same relaxed atomic store discipline
            // weft_fanout_fill uses — one word per store). Between the
            // per-word stores the slot still carries the previous frame's
            // words — the exact tear window the stamp bracket exists to
            // close.
            _Atomic uint32_t* dst = (_Atomic uint32_t*)cur;
            for (uint32_t w = W; w-- > 0;) {
                atomic_store_explicit(dst + w, a->src[w], memory_order_relaxed);
            }
        } else {
            (void)weft_fanout_fill(a->f, a->src, (size_t)W * 4);
        }
        chaos_point(&a->tls);
        (void)weft_fanout_publish(a->f);
    }
    for (uint32_t k = 0; k < WEFT_CHAOS_KINDS; k++) a->injected[k] = a->tls.injected[k];
    return NULL;
}

static void* free_reader_fn(void* argp) {
    free_reader_t* a = (free_reader_t*)argp;
    const uint32_t W = a->cfg.words;
    const uint32_t* view;
    uint64_t quiet_ticks = 0;
    while (a->r->last_seq < a->cfg.frames && quiet_ticks < 1000000u) {
        chaos_point(&a->tls);
        const weft_fanout_claim_t* c = weft_fanout_claim(a->r);
        if (!c->fresh) {
            // quiet-tick watchdog: a reader that can never converge with a
            // LIVE writer would hang the run — bound it (L-C5) and fail.
            quiet_ticks++;
            continue;
        }
        quiet_ticks = 0;
        if (c->seq > a->cfg.frames) a->future_claims++;
        view = (const uint32_t*)weft_fanout_view(a->r);
        for (uint32_t w = 0; w < W; w++) {
            if (view[w] != tword(c->seq, w)) { a->torn_accepted++; break; }
        }
    }
    a->converged = (a->r->last_seq == a->cfg.frames);
    for (uint32_t k = 0; k < WEFT_CHAOS_KINDS; k++) a->injected[k] = a->tls.injected[k];
    return NULL;
}

int weft_chaos_run_free(const weft_chaos_config_t* cfg,
                        weft_chaos_verdict_t* out) {
    if (!out) return 2;
    memset(out, 0, sizeof(*out));
    snprintf(out->engine, sizeof(out->engine), "free");
    if (cfg_validate(cfg) != 0) return 2;
    if (cfg->frames == 0) return 2;

    uint64_t t0 = now_ns();

    weft_fanout_t* f = weft_fanout_new((size_t)cfg->words * 4, cfg->slots);
    if (!f) { fprintf(stderr, "chaos-free: fanout_new failed\n"); return 2; }

    free_writer_t w;
    memset(&w, 0, sizeof(w));
    w.f = f; w.cfg = *cfg;
    chaos_tls_seed(&w.tls, cfg->seed, 0, cfg->chaos_rate);
    w.src = (uint32_t*)malloc((size_t)cfg->words * 4);
    if (!w.src) { fprintf(stderr, "chaos-free: src alloc failed\n"); weft_fanout_free(f); return 2; }

    free_reader_t rd[4];
    weft_fanout_reader_t* readers[4];
    pthread_t wth, rth[4];
    for (uint32_t i = 0; i < cfg->readers; i++) {
        readers[i] = weft_fanout_reader_new(weft_fanout_ring(f),
                                            weft_fanout_ring_bytes((size_t)cfg->words * 4, cfg->slots),
                                            (size_t)cfg->words * 4, cfg->slots);
        if (!readers[i]) {
            fprintf(stderr, "chaos-free: reader_new[%u] failed\n", i);
            for (uint32_t j = 0; j < i; j++) weft_fanout_reader_free(readers[j]);
            free(w.src); weft_fanout_free(f);
            return 2;
        }
    }
    for (uint32_t i = 0; i < cfg->readers; i++) {
        memset(&rd[i], 0, sizeof(rd[i]));
        rd[i].r = readers[i]; rd[i].cfg = *cfg;
        chaos_tls_seed(&rd[i].tls, cfg->seed, i + 1, cfg->chaos_rate);
    }

    if (pthread_create(&wth, NULL, free_writer_fn, &w) != 0) {
        perror("chaos-free: pthread_create(writer)");
        for (uint32_t i = 0; i < cfg->readers; i++) weft_fanout_reader_free(readers[i]);
        free(w.src); weft_fanout_free(f);
        return 2;
    }
    for (uint32_t i = 0; i < cfg->readers; i++) {
        if (pthread_create(&rth[i], NULL, free_reader_fn, &rd[i]) != 0) {
            perror("chaos-free: pthread_create(reader)");
            pthread_join(wth, NULL);
            for (uint32_t j = 0; j < cfg->readers; j++) weft_fanout_reader_free(readers[j]);
            free(w.src); weft_fanout_free(f);
            return 2;
        }
    }
    pthread_join(wth, NULL);
    for (uint32_t i = 0; i < cfg->readers; i++) pthread_join(rth[i], NULL);

    out->elapsed_ns = now_ns() - t0;

    weft_chaos_ledger_t* led = &out->ledger;
    weft_fanout_debug_t dbg;
    weft_fanout_debug_stats(f, &dbg);
    led->publishes = dbg.publishes;
    bool telescoping_ok = true;
    bool converged_all = true;
    for (uint32_t i = 0; i < cfg->readers; i++) {
        weft_fanout_stats_t st;
        weft_fanout_reader_stats(readers[i], &st);
        led->fresh_claims[i] = st.fresh;
        led->sum_dropped[i] = st.drops;
        led->last_seq[i] = readers[i]->last_seq;
        led->skips[i] = st.skipped_mid_overwrite;
        led->exhausted[i] = st.torn_exhausted;
        led->torn_accepted += rd[i].torn_accepted;
        led->future_claims += rd[i].future_claims;
        if (led->sum_dropped[i] != led->last_seq[i] - led->fresh_claims[i]) {
            telescoping_ok = false;
        }
        if (!rd[i].converged) converged_all = false;
        for (uint32_t k = 0; k < WEFT_CHAOS_KINDS; k++) {
            led->injected[k] += rd[i].injected[k];
        }
        weft_fanout_reader_free(readers[i]);
    }
    for (uint32_t k = 0; k < WEFT_CHAOS_KINDS; k++) led->injected[k] += w.injected[k];

    out->ledger.drained = converged_all;
    out->pass = converged_all && telescoping_ok &&
                led->torn_accepted == 0 && led->future_claims == 0 &&
                led->publishes == cfg->frames;

    free(w.src);
    weft_fanout_free(f);
    return out->pass ? 0 : 1;
}

#else // !WEFT_FANOUT_CHAOS_FREE_MODE

int weft_chaos_run_free(const weft_chaos_config_t* cfg,
                        weft_chaos_verdict_t* out) {
    if (out) {
        memset(out, 0, sizeof(*out));
        snprintf(out->engine, sizeof(out->engine), "free");
    }
    (void)cfg;
    return 2; // built without the free engine (see WEFT_FANOUT_CHAOS_FREE_MODE)
}

#endif // WEFT_FANOUT_CHAOS_FREE_MODE

// ---------------------------------------------------------------------------
// Verdict JSON — the byte-exact cross-port surface
// ---------------------------------------------------------------------------

static size_t put_u64_arr(char* dst, size_t cap, size_t pos, const char* key,
                          const uint64_t* v, uint32_t n) {
    int off = snprintf(dst + pos, cap - pos, ",\"%s\":[", key);
    if (off < 0) return pos;
    pos += (size_t)off;
    for (uint32_t i = 0; i < n; i++) {
        off = snprintf(dst + pos, cap - pos, "%s%llu", i ? "," : "",
                       (unsigned long long)v[i]);
        if (off < 0) return pos;
        pos += (size_t)off;
    }
    off = snprintf(dst + pos, cap - pos, "]");
    return off < 0 ? pos : pos + (size_t)off;
}

int weft_chaos_verdict_json(const weft_chaos_config_t* cfg,
                            const weft_chaos_verdict_t* v,
                            char* buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    size_t pos = 0;
    int n;
    const weft_chaos_ledger_t* led = &v->ledger;
    bool stepped = strcmp(v->engine, "stepped") == 0;

    n = snprintf(buf + pos, cap - pos,
                 "{\"engine\":\"weft-chaos-%s\",\"v\":1,\"seed\":%u,",
                 v->engine, cfg->seed);
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;

    if (stepped) {
        n = snprintf(buf + pos, cap - pos, "\"steps\":%u,", cfg->steps);
        if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;
    }
    n = snprintf(buf + pos, cap - pos,
                 "\"slots\":%u,\"words\":%u,\"readers\":%u,\"frames\":%u,"
                 "\"chaosRate\":%u,",
                 cfg->slots, cfg->words, cfg->readers, cfg->frames,
                 cfg->chaos_rate);
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;

    if (!stepped) {
        n = snprintf(buf + pos, cap - pos, "\"elapsedNs\":%llu,",
                     (unsigned long long)v->elapsed_ns);
        if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;
    } else {
        n = snprintf(buf + pos, cap - pos, "\"stepsExecuted\":%llu,",
                     (unsigned long long)led->steps_executed);
        if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;
    }

    n = snprintf(buf + pos, cap - pos,
                 "\"injections\":{\"preempt\":%llu,\"stall\":%llu,"
                 "\"throttle\":%llu,\"reorder\":%llu},",
                 (unsigned long long)led->injected[0],
                 (unsigned long long)led->injected[1],
                 (unsigned long long)led->injected[2],
                 (unsigned long long)led->injected[3]);
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;

    n = snprintf(buf + pos, cap - pos, "\"ledger\":{\"publishes\":%llu",
                 (unsigned long long)led->publishes);
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;

    size_t p2 = put_u64_arr(buf, cap, pos, "fresh", led->fresh_claims, cfg->readers);
    if (p2 == pos) return -1;
    pos = p2;
    pos = put_u64_arr(buf, cap, pos, "dropped", led->sum_dropped, cfg->readers);
    pos = put_u64_arr(buf, cap, pos, "lastSeq", led->last_seq, cfg->readers);
    pos = put_u64_arr(buf, cap, pos, "skips", led->skips, cfg->readers);
    pos = put_u64_arr(buf, cap, pos, "exhausted", led->exhausted, cfg->readers);

    n = snprintf(buf + pos, cap - pos,
                 ",\"tornAccepted\":%llu,\"futureClaims\":%llu,"
                 "\"bracketViolations\":%llu,\"drained\":%s},",
                 (unsigned long long)led->torn_accepted,
                 (unsigned long long)led->future_claims,
                 (unsigned long long)led->bracket_violations,
                 led->drained ? "true" : "false");
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;

    n = snprintf(buf + pos, cap - pos,
                 "\"telescoping\":\"%s\",\"verdict\":\"%s\"}",
                 (v->pass ? "OK" : "VIOLATED"),
                 (v->pass ? "PASS" : "FAIL"));
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;

    return (int)pos;
}

// ---------------------------------------------------------------------------
// Self-test — PRNG vectors + a known-good tiny run (chaos shard gate 1)
// ---------------------------------------------------------------------------

int weft_chaos_selftest(void) {
    // 1. PINNED vectors — hard constants shared by every port's self-test
    //    (a port that drifts from these numbers has a different PRNG/pattern
    //    than this reference and will fail the cross-language parity diff).
    if (mix32(0xDEADBEEFu) != 3861431939u) return 1;
    if (tword(1, 0) != 1834104592u) return 1;
    if (tword(7, 3) != 2500287888u) return 1;
    if (tword(100, 15) != 4197121613u) return 1;

    // 2. PRNG stream, seed 42 — first eight draws, pinned. Cross-checked
    //    against TS/Kotlin/Dart/Swift engines byte-for-byte.
    static const uint32_t seed42_draws[8] = {
        1034221180u, 2302191726u, 1921777443u, 3822789115u,
        4193179225u, 3051818586u, 2559959645u, 2724063783u,
    };
    chaos_rng_t r;
    chaos_seed(&r, 42u);
    for (int i = 0; i < 8; i++) {
        if (chaos_next(&r) != seed42_draws[i]) return 1;
    }
    // A different seed diverges (sanity, not equality).
    chaos_seed(&r, 43u);
    uint32_t d2 = chaos_next(&r);
    chaos_seed(&r, 42u);
    if (d2 == chaos_next(&r)) return 1;

    // 3. Tiny stepped run with known-good shape (2 readers, 2 slots,
    //    2 words, 4 frames, heavy chaos): all properties must hold.
    weft_chaos_config_t cfg = {
        .seed = 7u, .steps = 4000u, .slots = 2u, .words = 2u,
        .readers = 2u, .frames = 4u, .chaos_rate = 300u,
    };
    weft_chaos_verdict_t v;
    int rc = weft_chaos_run_stepped(&cfg, &v);
    if (rc != 0) {
        char jb[2048];
        if (weft_chaos_verdict_json(&cfg, &v, jb, sizeof(jb)) > 0) {
            fprintf(stderr, "selftest verdict: %s\n", jb);
        }
        return 1;
    }
    char json[2048];
    if (weft_chaos_verdict_json(&cfg, &v, json, sizeof(json)) < 0) return 1;
    if (!strstr(json, "\"verdict\":\"PASS\"")) return 1;
    if (strstr(json, "null")) return 1;
    return 0;
}
