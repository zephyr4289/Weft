// weft_synth_bus.c — Memory-Bus Saturation & Cache Contention Tester
// (Pillar 8, module B).
//
// Implementation notes (the WHY lives in the header):
//   * hammer threads touch ONE chosen word per thread (word_map) so the
//     three modes differ ONLY in coherency topology, never in instruction
//     count — that is what makes the A/B ratio an honest false-sharing
//     signal instead of a code-shape artifact.
//   * bursts re-check the stop flag with acquire loads; the exact-quota
//     mode (max_ops_per_thread) breaks out of the burst loop after the
//     local counter crosses the quota, so measurement runs are replayable.
//   * the seqlock uses the fence-anchored discipline (odd version + seq_cst
//     fence before payload stores, release fence before the even bump;
//     readers acquire-load the version, fence after payload) — every access
//     is an atomic operation, so TSan sees no data race by construction.
//   * the probe escalates to sched_yield() every YIELD_AFTER failed spins
//     (or at the MAX_ATTEMPTS hard ceiling), which keeps the retry loop
//     cooperative under maximum saturation while the writer's 4-store
//     critical section bounds how long any yield phase can last.
//   * the cycle counter is calibrated here (20 ms against CLOCK_MONOTONIC_
//     RAW) because this module owns the only sub-100 ns budget in the lab.

#include "weft_synth/weft_synth_bus.h"

#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* cycle calibration + CPU count (this module owns the sub-100ns clock)  */
/* ------------------------------------------------------------------ */

uint64_t weft_synth_cycle_hz = 0;

uint64_t weft_synth_cycle_calibrate(void) {
    if (weft_synth_cycle_hz != 0ull) {
        return weft_synth_cycle_hz;
    }
    uint64_t c0 = weft_synth_cycles();
    int64_t t0 = weft_synth_now_ns();
    uint64_t c1 = c0;
    int64_t t1 = t0;
    while (t1 - t0 < 20000000ll) {  /* ~20 ms calibration window */
        WEFT_SYNTH_CPU_RELAX();
        c1 = weft_synth_cycles();
        t1 = weft_synth_now_ns();
    }
    if (c1 <= c0 || t1 <= t0) {
        return 0ull;  /* pathological counter: leave uncalibrated (honest) */
    }
    __uint128_t dt = (__uint128_t)(c1 - c0) * 1000000000ull;
    uint64_t hz = (uint64_t)(dt / (uint64_t)(t1 - t0));
    weft_synth_cycle_hz = hz;
    return hz;
}

unsigned weft_synth_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) {
        return 1u;
    }
    return (n > 1024l) ? 1024u : (unsigned)n;
}

/* ------------------------------------------------------------------ */
/* blender                                                              */
/* ------------------------------------------------------------------ */

int weft_synth_bus_defaults(weft_synth_bus_cfg_t *cfg) {
    if (cfg == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    cfg->n_threads = 4u;
    cfg->line_bytes = WEFT_SYNTH_CACHE_LINE;
    cfg->n_lines = 8u;
    cfg->burst_iters = 256u;
    cfg->max_ops_per_thread = 0ull;  /* run until stop() by default */
    cfg->seed = 1u;
    cfg->mode = WEFT_SYNTH_BUS_ADJACENT;
    return WEFT_SYNTH_OK;
}

size_t weft_synth_bus_arena_bytes(const weft_synth_bus_cfg_t *cfg) {
    if (cfg == NULL) {
        return 0u;
    }
    return (size_t)cfg->line_bytes * (size_t)cfg->n_lines;
}

int weft_synth_bus_blender_init(weft_synth_bus_blender_t *b,
                                const weft_synth_bus_cfg_t *cfg,
                                _Atomic uint64_t *arena,
                                size_t arena_words) {
    if (b == NULL || cfg == NULL || arena == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    if (cfg->n_threads == 0u || cfg->n_threads > WEFT_SYNTH_BUS_MAX_THREADS) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    if (cfg->line_bytes != WEFT_SYNTH_CACHE_LINE &&
        cfg->line_bytes != WEFT_SYNTH_CACHE_LINE2) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    if (cfg->n_lines == 0u || cfg->burst_iters == 0u) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    if (cfg->mode == WEFT_SYNTH_BUS_ISOLATED && cfg->n_lines < cfg->n_threads) {
        return WEFT_SYNTH_ERR_RANGE;
    }
    if (((uintptr_t)arena % cfg->line_bytes) != 0u) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    size_t need = (size_t)cfg->n_lines * (cfg->line_bytes / sizeof(uint64_t));
    if (arena_words < need) {
        return WEFT_SYNTH_ERR_RANGE;
    }

    memset(b, 0, sizeof(*b));
    b->cfg = *cfg;
    b->arena = arena;
    b->arena_words = arena_words;
    b->ops_total = 0ull;
    b->running = 0;

    const uint32_t words_per_line = cfg->line_bytes / (uint32_t)sizeof(uint64_t);
    for (uint32_t i = 0; i < cfg->n_threads; i++) {
        switch (cfg->mode) {
        case WEFT_SYNTH_BUS_ISOLATED:
            /* thread i owns line i, first word */
            b->word_map[i] = i * words_per_line;
            break;
        case WEFT_SYNTH_BUS_ADJACENT:
            /* all threads inside line 0, distinct words when possible */
            b->word_map[i] = i % words_per_line;
            break;
        case WEFT_SYNTH_BUS_FALSE_SHARE:
        default:
            b->word_map[i] = 0u;
            break;
        }
        atomic_store(&b->totals[i], 0ull);
    }
    atomic_store(&b->stop_flag, 0);
    return WEFT_SYNTH_OK;
}

/* The hammer threads receive their index through the blender's own
 * per-blender argument array (written by start() BEFORE pthread_create
 * of thread i, read exactly once by that thread) — no shared statics,
 * concurrent blenders stay independent (Law 1 / TSan-clean). */
static void *weft_synth_bus_hammer_main(void *arg) {
    weft_synth_bus_hammer_arg_t *ha = (weft_synth_bus_hammer_arg_t *)arg;
    weft_synth_bus_blender_t *b = ha->b;
    const uint32_t idx = ha->idx;
    const uint32_t word = b->word_map[idx];
    const uint32_t burst = b->cfg.burst_iters;
    const uint64_t quota = b->cfg.max_ops_per_thread;
    uint64_t done = 0ull;

    for (;;) {
        /* clip the final burst so an exact quota is an exact quota
         * (measurement replayability depends on it) */
        uint32_t n = burst;
        if (quota != 0ull && done + (uint64_t)n > quota) {
            n = (uint32_t)(quota - done);
        }
        for (uint32_t i = 0u; i < n; i++) {
            (void)atomic_fetch_add_explicit(&b->arena[word], 1ull,
                                            memory_order_relaxed);
        }
        done += n;
        atomic_store_explicit(&b->totals[idx], done, memory_order_relaxed);
        if (quota != 0ull && done >= quota) {
            break;
        }
        if (atomic_load_explicit(&b->stop_flag, memory_order_acquire)) {
            break;
        }
    }
    atomic_store_explicit(&b->totals[idx], done, memory_order_relaxed);
    return NULL;
}

int weft_synth_bus_blender_start(weft_synth_bus_blender_t *b) {
    if (b == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    if (b->running) {
        return WEFT_SYNTH_ERR_BUSY;
    }
    atomic_store(&b->stop_flag, 0);
    for (uint32_t i = 0u; i < b->cfg.n_threads; i++) {
        atomic_store(&b->totals[i], 0ull);
    }
    for (uint32_t i = 0u; i < b->cfg.n_threads; i++) {
        b->hammer_args[i].b = b;
        b->hammer_args[i].idx = i;
        int rc = pthread_create(&b->threads[i], NULL,
                                weft_synth_bus_hammer_main, &b->hammer_args[i]);
        if (rc != 0) {
            /* fail-closed: stop and join everything already spawned */
            atomic_store(&b->stop_flag, 1);
            while (i > 0u) {
                i--;
                (void)pthread_join(b->threads[i], NULL);
            }
            b->running = 0;
            return WEFT_SYNTH_ERR_SYS;
        }
    }
    b->running = 1;
    return WEFT_SYNTH_OK;
}

uint64_t weft_synth_bus_blender_stop(weft_synth_bus_blender_t *b) {
    if (b == NULL || !b->running) {
        return 0ull;
    }
    const uint64_t quota = b->cfg.max_ops_per_thread;
    if (quota != 0ull) {
        /* quota mode: the run is only complete when every hammer has
         * reached its quota — stopping early would measure thread
         * lifecycle, not the RMW stream. Bounded wait (30 s), then the
         * stop flag force-exits whatever is left (fail-closed, honest). */
        const int64_t deadline = weft_synth_now_ns() + 30000000000ll;
        for (;;) {
            uint32_t pending = 0u;
            for (uint32_t i = 0u; i < b->cfg.n_threads; i++) {
                if (atomic_load_explicit(&b->totals[i],
                                         memory_order_relaxed) < quota) {
                    pending++;
                }
            }
            if (pending == 0u) {
                break;
            }
            if (weft_synth_now_ns() >= deadline) {
                break;  /* forced: stop flag below ends the stragglers */
            }
            for (uint32_t i = 0u; i < 4096u; i++) {
                WEFT_SYNTH_CPU_RELAX();  /* keep the hammers fed */
            }
            (void)sched_yield();
        }
    }
    atomic_store(&b->stop_flag, 1);
    uint64_t total = 0ull;
    for (uint32_t i = 0u; i < b->cfg.n_threads; i++) {
        (void)pthread_join(b->threads[i], NULL);
        total += atomic_load(&b->totals[i]);
    }
    b->ops_total = total;
    b->running = 0;
    return total;
}

void weft_synth_bus_blender_destroy(weft_synth_bus_blender_t *b) {
    if (b == NULL) {
        return;
    }
    if (b->running) {
        (void)weft_synth_bus_blender_stop(b);
    }
    b->arena = NULL;
    b->arena_words = 0u;
}

/* ------------------------------------------------------------------ */
/* false-sharing A/B measurement                                        */
/* ------------------------------------------------------------------ */

static int weft_synth_bus_measure_run(uint32_t n_threads, uint32_t line_bytes,
                                      weft_synth_bus_mode_t mode,
                                      uint64_t ops_per_thread,
                                      double *ns_per_op_out,
                                      _Atomic uint64_t *arena,
                                      size_t arena_words) {
    weft_synth_bus_cfg_t cfg;
    weft_synth_bus_blender_t blender;
    (void)weft_synth_bus_defaults(&cfg);
    cfg.n_threads = n_threads;
    cfg.line_bytes = line_bytes;
    cfg.n_lines = (uint32_t)(arena_words / (line_bytes / sizeof(uint64_t)));
    cfg.mode = mode;
    cfg.max_ops_per_thread = ops_per_thread;
    cfg.burst_iters = 512u;

    int rc = weft_synth_bus_blender_init(&blender, &cfg, arena, arena_words);
    if (rc != WEFT_SYNTH_OK) {
        return rc;
    }
    rc = weft_synth_bus_blender_start(&blender);
    if (rc != WEFT_SYNTH_OK) {
        return rc;
    }
#if defined(__gnu_linux__)
    /* experimental control: pin each hammer to its own CPU when the box
     * has them — unpinned placement lets the scheduler co-locate the
     * threads, and a co-located pair never exhibits the cross-core
     * coherency penalty the A/B exists to measure. */
    const unsigned cpus = weft_synth_cpu_count();
    if (n_threads >= 2u && cpus >= n_threads) {
        for (uint32_t i = 0u; i < n_threads; i++) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(i, &set);
            (void)pthread_setaffinity_np(blender.threads[i],
                                         sizeof(set), &set);
        }
    }
#endif
    int64_t t0 = weft_synth_now_ns();
    uint64_t ops = weft_synth_bus_blender_stop(&blender);
    int64_t t1 = weft_synth_now_ns();
    weft_synth_bus_blender_destroy(&blender);
    if (ops == 0ull) {
        return WEFT_SYNTH_ERR_SYS;
    }
    *ns_per_op_out = (double)(t1 - t0) / (double)ops;
    return WEFT_SYNTH_OK;
}

int weft_synth_bus_measure_false_share(uint32_t n_threads,
                                       uint64_t ops_per_thread,
                                       weft_synth_bus_share_stat_t *out) {
    if (out == NULL || n_threads == 0u ||
        n_threads > WEFT_SYNTH_BUS_MAX_THREADS || ops_per_thread < 1000ull) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    const uint32_t line_bytes = WEFT_SYNTH_CACHE_LINE;
    size_t words = (size_t)(n_threads + 1u) * (line_bytes / sizeof(uint64_t));
    void *raw = NULL;
    if (posix_memalign(&raw, (size_t)WEFT_SYNTH_CACHE_LINE2,
                       words * sizeof(uint64_t)) != 0 || raw == NULL) {
        return WEFT_SYNTH_ERR_SYS;
    }
    _Atomic uint64_t *arena = (_Atomic uint64_t *)raw;

    double shared = 0.0, isolated = 0.0;
    int rc = weft_synth_bus_measure_run(n_threads, line_bytes,
                                        WEFT_SYNTH_BUS_ADJACENT,
                                        ops_per_thread, &shared,
                                        arena, words);
    if (rc == WEFT_SYNTH_OK) {
        rc = weft_synth_bus_measure_run(n_threads, line_bytes,
                                        WEFT_SYNTH_BUS_ISOLATED,
                                        ops_per_thread, &isolated,
                                        arena, words);
    }
    free(raw);
    if (rc != WEFT_SYNTH_OK) {
        return rc;
    }
    out->shared_ns_per_op = shared;
    out->isolated_ns_per_op = isolated;
    out->ratio = (isolated > 0.0) ? (shared / isolated) : 0.0;
    out->ops_per_thread = ops_per_thread;
    out->n_threads = n_threads;
    out->line_bytes = line_bytes;
    return WEFT_SYNTH_OK;
}

/* ------------------------------------------------------------------ */
/* reference seqlock                                                     */
/* ------------------------------------------------------------------ */

int weft_synth_seqlock_init(weft_synth_seqlock_t *sl) {
    if (sl == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    atomic_store_explicit(&sl->version, 0ull, memory_order_relaxed);
    for (uint32_t i = 0u; i < 4u; i++) {
        atomic_store_explicit(&sl->payload[i], 0ull, memory_order_relaxed);
    }
    atomic_thread_fence(memory_order_release);
    return WEFT_SYNTH_OK;
}

void weft_synth_seqlock_write(weft_synth_seqlock_t *sl,
                              const uint64_t payload[4]) {
    uint64_t v = atomic_load_explicit(&sl->version, memory_order_relaxed);
    atomic_store_explicit(&sl->version, v + 1ull, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);  /* odd visible before payload */
    for (uint32_t i = 0u; i < 4u; i++) {
        atomic_store_explicit(&sl->payload[i], payload[i],
                              memory_order_relaxed);
    }
    atomic_thread_fence(memory_order_release);  /* payload before even bump */
    atomic_store_explicit(&sl->version, v + 2ull, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* instrumented reader probe                                             */
/* ------------------------------------------------------------------ */

int weft_synth_bus_probe_init(weft_synth_bus_probe_t *p, uint32_t seed,
                              uint64_t *latency_scratch,
                              uint32_t scratch_cap) {
    if (p == NULL || (latency_scratch == NULL && scratch_cap != 0u)) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    memset(p, 0, sizeof(*p));
    p->rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)seed;
    (void)weft_synth_splitmix64(&p->rng);
    p->lat = latency_scratch;
    p->lat_cap = scratch_cap;
    p->lat_len = 0u;
    return WEFT_SYNTH_OK;
}

int weft_synth_bus_probe_read(weft_synth_bus_probe_t *p,
                              weft_synth_seqlock_t *sl,
                              uint64_t out_payload[4]) {
    if (p == NULL || sl == NULL) {
        return WEFT_SYNTH_ERR_INVALID;
    }
    uint64_t attempts = 0ull;
    uint32_t spin = 0u;
    uint64_t pay[4] = {0ull, 0ull, 0ull, 0ull};

    for (;;) {
        /* one retry-loop ITERATION: measure it end to end — this is the
         * population the directive's "p99 retry latency < 100 ns" SLA
         * is computed over (the loop's iterations stay bounded and fast
         * under saturation; scheduler preemption lands in the p99.9/max
         * tail and is reported honestly, not gated). */
        const uint64_t a0 = weft_synth_cycles();
        attempts++;
        const uint64_t s1 =
            atomic_load_explicit(&sl->version, memory_order_acquire);
        int stable = 0;
        if ((s1 & 1ull) == 0ull) {
            pay[0] = atomic_load_explicit(&sl->payload[0], memory_order_relaxed);
            pay[1] = atomic_load_explicit(&sl->payload[1], memory_order_relaxed);
            pay[2] = atomic_load_explicit(&sl->payload[2], memory_order_relaxed);
            pay[3] = atomic_load_explicit(&sl->payload[3], memory_order_relaxed);
            atomic_thread_fence(memory_order_acquire);
            const uint64_t s2 =
                atomic_load_explicit(&sl->version, memory_order_acquire);
            if (s1 == s2) {
                stable = 1;  /* stable snapshot */
            }
        }
        const uint64_t ns =
            weft_synth_cycles_to_ns(weft_synth_cycles() - a0);
        if (p->lat != NULL && p->lat_len < p->lat_cap) {
            p->lat[p->lat_len++] = ns;
        }
        if (stable) {
            break;
        }
        p->retries++;
        spin++;
        if (spin >= WEFT_SYNTH_PROBE_YIELD_AFTER ||
            attempts >= WEFT_SYNTH_PROBE_MAX_ATTEMPTS) {
            (void)sched_yield();  /* cooperative escalation (Law 3) */
            p->yields++;
            spin = 0u;
        }
        WEFT_SYNTH_CPU_RELAX();
    }

    p->reads++;
    if ((uint64_t)p->max_attempts < attempts) {
        p->max_attempts = (uint32_t)attempts;
    }
    /* fold the payload into the probe stream: makes every recorded read
     * value-dependent (no dead-load elimination in benchmarks) */
    p->rng ^= pay[0]; p->rng ^= pay[1]; p->rng ^= pay[2]; p->rng ^= pay[3];
    if (out_payload != NULL) {
        out_payload[0] = pay[0];
        out_payload[1] = pay[1];
        out_payload[2] = pay[2];
        out_payload[3] = pay[3];
    }
    return (int)attempts;
}

/* in-place heapsort (zero heap — qsort may allocate on large arrays) */
static void weft_synth_bus_sift(uint64_t *a, uint32_t start, uint32_t end) {
    uint32_t root = start;
    while ((2u * root + 1u) <= end) {
        uint32_t child = 2u * root + 1u;
        if (child + 1u <= end && a[child] < a[child + 1u]) {
            child++;
        }
        if (a[root] < a[child]) {
            uint64_t tmp = a[root];
            a[root] = a[child];
            a[child] = tmp;
            root = child;
        } else {
            return;
        }
    }
}

static void weft_synth_bus_heapsort_u64(uint64_t *a, uint32_t n) {
    if (n < 2u) {
        return;
    }
    for (int32_t start = (int32_t)(n / 2u) - 1; start >= 0; start--) {
        weft_synth_bus_sift(a, (uint32_t)start, n - 1u);
    }
    for (int32_t end = (int32_t)(n - 1u); end > 0; end--) {
        uint64_t tmp = a[end];
        a[end] = a[0];
        a[0] = tmp;
        weft_synth_bus_sift(a, 0u, (uint32_t)end - 1u);
    }
}

static double weft_synth_bus_pct_sorted(const uint64_t *a, uint32_t n,
                                        double pct) {
    if (n == 0u) {
        return 0.0;
    }
    double idx = (pct / 100.0) * (double)(n - 1u);
    if (idx < 0.0) {
        idx = 0.0;
    }
    if (idx > (double)(n - 1u)) {
        idx = (double)(n - 1u);
    }
    uint32_t lo = (uint32_t)idx;
    uint32_t hi = (lo + 1u < n) ? (lo + 1u) : lo;
    double frac = idx - (double)lo;
    return (double)a[lo] * (1.0 - frac) + (double)a[hi] * frac;
}

void weft_synth_bus_probe_stats(weft_synth_bus_probe_t *p,
                                weft_synth_probe_stat_t *out) {
    if (p == NULL || out == NULL) {
        return;
    }
    out->reads = p->reads;
    out->retries = p->retries;
    out->yields = p->yields;
    out->max_attempts = p->max_attempts;
    out->avg_retries_per_read =
        (p->reads > 0ull) ? ((double)p->retries / (double)p->reads) : 0.0;
    /* sorts the caller scratch in place (documented in the header) */
    weft_synth_bus_heapsort_u64(p->lat, p->lat_len);
    out->p50_ns = weft_synth_bus_pct_sorted(p->lat, p->lat_len, 50.0);
    out->p99_ns = weft_synth_bus_pct_sorted(p->lat, p->lat_len, 99.0);
    out->p999_ns = weft_synth_bus_pct_sorted(p->lat, p->lat_len, 99.9);
    out->max_ns = (p->lat_len > 0u) ? (double)p->lat[p->lat_len - 1u] : 0.0;
}

void weft_synth_bus_probe_merge(weft_synth_bus_probe_t *dst,
                                const weft_synth_bus_probe_t *src) {
    if (dst == NULL || src == NULL || dst == src) {
        return;
    }
    for (uint32_t i = 0u; i < src->lat_len; i++) {
        if (dst->lat_len < dst->lat_cap) {
            dst->lat[dst->lat_len++] = src->lat[i];
        } else {
            dst->merged_dropped++;  /* honest overflow accounting */
        }
    }
    dst->reads += src->reads;
    dst->retries += src->retries;
    dst->yields += src->yields;
    dst->merged_dropped += src->merged_dropped;
    if (src->max_attempts > dst->max_attempts) {
        dst->max_attempts = src->max_attempts;
    }
    dst->rng ^= src->rng;
}

/* ------------------------------------------------------------------ */
/* seqlock writer thread                                                 */
/* ------------------------------------------------------------------ */

void *weft_synth_seqlock_writer_thread(void *arg) {
    weft_synth_seqlock_writer_t *w = (weft_synth_seqlock_writer_t *)arg;
    if (w == NULL || w->sl == NULL || w->stop == NULL) {
        return NULL;
    }
    while (!atomic_load_explicit(w->stop, memory_order_acquire)) {
        uint64_t pay[4];
        pay[0] = weft_synth_xorshift64(&w->rng);
        pay[1] = weft_synth_xorshift64(&w->rng);
        pay[2] = weft_synth_xorshift64(&w->rng);
        pay[3] = weft_synth_xorshift64(&w->rng);
        weft_synth_seqlock_write(w->sl, pay);
        w->writes++;
        if (w->period_ns > 0u) {
            /* wall-clock pacing: no calibration dependency, granular to
             * the ~25 ns clock_gettime floor — plenty for >= 500 ns gaps */
            const int64_t t0 = weft_synth_now_ns();
            while (weft_synth_now_ns() - t0 < (int64_t)w->period_ns) {
                WEFT_SYNTH_CPU_RELAX();
            }
        }
    }
    return NULL;
}
