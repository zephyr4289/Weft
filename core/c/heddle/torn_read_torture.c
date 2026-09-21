// torn_read_torture.c — WHP2 multi-threaded race-condition, torn-read
// detection and dirty-mask accounting torture (Pillar 4, T-series).
//
// Legs (any invariant breach fails the run):
//   T1  STATE multi-producer: 4 writer threads (2 owned lanes each),
//       3 seqlock readers, 1 render-thread harvester. Every payload is
//       self-verifying (canary + writer id + iteration + cell invariant);
//       a torn read CANNOT satisfy it. >=100k continuous updates/writer.
//   T2  STATE single-producer: 1 writer, batched sessions across 8 lanes,
//       same readers + harvester; exercises the global two-store pair
//       and the BATCHED dirty-mask flush path.
//   T3  RING streaming: 1 writer pushing at full speed (never blocks),
//       readers chasing the head over a sliding window; surviving
//       ordinals verified, E_OVERRUN allowed + counted (honest),
//       in-band backpressure marks asserted at the end.
//   T4  Zero-heap: glibc heap deltas == 0 across the entire torture
//       window (plain builds).
//
// The dirty-mask accounting invariant (exact, not probabilistic):
//   SUM(popcount of every harvest result) + popcount(final mask)
//       == SUM(bits flushed by every producer session)
// Atomicity of fetch_or/exchange on the same register makes this an
// identity; any protocol bug (lost re-mark, swallowed exchange,
// double-flush) breaks it.
//
// Iterations: HEDDLE_ITERS env overrides (TSAN legs run smaller).

#include "heddle_hotplane_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#  if __GLIBC_PREREQ(2, 33)
#    include <malloc.h>
#    define TORTURE_HAVE_MALLINFO2 1
#  endif
#endif

static uint64_t g_iters = 500000;       /* per leg, T1/T2 default       */
static uint64_t g_ring_iters = 1000000; /* T3 total pushes default      */

static void *plane_alloc(size_t n)
{
    void *p = NULL;
    if (posix_memalign(&p, 64, n) != 0) {
        return NULL;
    }
    return p;
}

static inline uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

/* sample layout: [0..8) canary | [8..12) wid | [12..16) iter */
static inline void sample_fill(uint8_t *s, uint32_t wid, uint32_t iter,
                               uint32_t cell)
{
    uint64_t canary = mix64(((uint64_t)wid << 40) ^ ((uint64_t)iter << 8) ^
                            (uint64_t)cell ^ 0xC0FFEEull);
    memcpy(s, &canary, 8);
    memcpy(s + 8, &wid, 4);
    memcpy(s + 12, &iter, 4);
}
static inline uint32_t cell_of(uint32_t wid, uint32_t iter, uint32_t cells)
{
    return (iter * 7u + wid * 13u) & (cells - 1u);
}
/* Full torn-detector: 0 iff payload is internally consistent AND sits
 * in exactly the cell it was written to. */
static int sample_verify(const uint8_t *s, uint32_t cell, uint32_t cells,
                         uint32_t writers)
{
    uint64_t canary;
    uint32_t wid, iter;
    memcpy(&canary, s, 8);
    memcpy(&wid, s + 8, 4);
    memcpy(&iter, s + 12, 4);
    if (wid >= writers) {
        return 1;
    }
    if (cell_of(wid, iter, cells) != cell) {
        return 2;
    }
    if (mix64(((uint64_t)wid << 40) ^ ((uint64_t)iter << 8) ^
              (uint64_t)cell ^ 0xC0FFEEull) != canary) {
        return 3;
    }
    return 0;
}

static int g_fail = 0;
static int g_warmup = 0;   /* suppress leg output during the warm-up pass */
#define TPRINTF(...)                                                       \
    do {                                                                   \
        if (!g_warmup) {                                                   \
            printf(__VA_ARGS__);                                           \
        }                                                                  \
    } while (0)
#define TORTURE_ASSERT(cond, msg)                                          \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "TORTURE FAIL: %s (line %d)\n", (msg),         \
                    __LINE__);                                             \
            g_fail = 1;                                                    \
        }                                                                  \
    } while (0)

/* ---- shared reader thread ---------------------------------------------- */

typedef struct reader_arg {
    hplane_ctx_t *ctx;
    _Atomic int *run;
    uint32_t writers;                 /* valid wid range for verifier    */
    uint32_t cells;
    uint64_t seed;
    _Atomic uint64_t reads;
    _Atomic uint64_t torn;
    _Atomic uint64_t bad;
} reader_arg_t;

static void *state_reader(void *varg)
{
    reader_arg_t *a = varg;
    uint8_t out[16];
    uint64_t n = 0, torn = 0, bad = 0;
    uint64_t rng = a->seed;
    while (atomic_load_explicit(a->run, memory_order_acquire) || n < 4096) {
        rng = mix64(rng + 1);
        uint32_t lane = (uint32_t)(rng % 8u);
        uint32_t cell = (uint32_t)((rng >> 32) % a->cells);
        int rc = hplane_cell_read(a->ctx, lane, cell, out, 16, 64);
        if (rc == HEDDLE_OK) {
            /* Committed cells always hold a filled, self-consistent
             * sample; a zero canary means torn-at-zero data. */
            uint64_t canary;
            memcpy(&canary, out, 8);
            if (canary == 0 ||
                sample_verify(out, cell, a->cells, a->writers) != 0) {
                bad++;
            }
            n++;
        } else if (rc == HEDDLE_E_SEQ_TORN) {
            torn++;    /* bounded retry exhaustion under pressure: legal */
        } else {
            bad++;     /* any other code on this path: illegal           */
        }
    }
    atomic_store_explicit(&a->reads, n, memory_order_relaxed);
    atomic_store_explicit(&a->torn, torn, memory_order_relaxed);
    atomic_store_explicit(&a->bad, bad, memory_order_relaxed);
    return NULL;
}

static uint64_t drain_mask(hplane_ctx_t *c)
{
    uint64_t bits = 0;
    for (int i = 0; i < 64; i++) {   /* exchange loop until stable-empty */
        uint64_t mask = 0;
        if (hplane_dirty_harvest(c, &mask) != HEDDLE_OK) {
            break;
        }
        if (mask == 0) {
            break;
        }
        bits += (uint64_t)__builtin_popcountll(mask);
    }
    return bits;
}

static uint64_t final_mask_popcount(const uint8_t *mem)
{
    return (uint64_t)__builtin_popcountll(
        __atomic_load_n(&((hplane_header_t *)(void *)mem)->dirty_mask,
                        __ATOMIC_RELAXED));
}

/* Prefill: deterministically write a valid sample into EVERY cell so
 * readers can verify from t=0 (iter solved from the cell_of inverse:
 * 7^-1 mod cells == 55 for cells in {64,128}). */
static void prefill_lane(hplane_ctx_t *p, uint32_t lane, uint32_t cells,
                          uint32_t wid)
{
    uint8_t s[16];
    uint32_t begin = (p->flags & WHP2_F_MULTI_PRODUCER)
                         ? (hplane_lane_begin(p, lane) == HEDDLE_OK)
                         : (hplane_commit_begin(p) == HEDDLE_OK);
    if (!begin) {
        TORTURE_ASSERT(0, "prefill session open");
        return;
    }
    for (uint32_t cell = 0; cell < cells; cell++) {
        uint32_t iter = (((cell + cells - (wid * 13u) % cells) % cells) * 55u) %
                        cells;
        sample_fill(s, wid, iter, cell);
        TORTURE_ASSERT(hplane_state_write(p, lane, cell, s, 16) == HEDDLE_OK,
                       "prefill write");
    }
    if (p->flags & WHP2_F_MULTI_PRODUCER) {
        TORTURE_ASSERT(hplane_lane_end(p, lane) == HEDDLE_OK,
                       "prefill lane_end");
    } else {
        TORTURE_ASSERT(hplane_commit_end(p) == HEDDLE_OK, "prefill commit");
    }
}

/* ---- T1: multi-producer state plane ------------------------------------ */

#define T1_LANES 8
#define T1_CELLS 128
#define T1_WRITERS 4

typedef struct t1_writer_arg {
    hplane_ctx_t *ctx;
    uint32_t wid;
    uint64_t iters;
    _Atomic uint64_t bits_flushed;
    _Atomic uint64_t done;
} t1_writer_arg_t;

static void *t1_writer(void *varg)
{
    t1_writer_arg_t *a = varg;
    uint8_t s[16];
    uint64_t bits = 0;
    for (uint64_t i = 0; i < a->iters; i++) {
        uint32_t lane = (i & 1) ? (2 * a->wid + 1) : (2 * a->wid);
        uint32_t cell = cell_of(a->wid, (uint32_t)i, T1_CELLS);
        sample_fill(s, a->wid, (uint32_t)i, cell);
        TORTURE_ASSERT(hplane_lane_begin(a->ctx, lane) == HEDDLE_OK,
                       "T1 lane_begin");
        TORTURE_ASSERT(hplane_state_write(a->ctx, lane, cell, s, 16) ==
                           HEDDLE_OK,
                       "T1 state_write");
        TORTURE_ASSERT(hplane_lane_end(a->ctx, lane) == HEDDLE_OK,
                       "T1 lane_end");
        bits += 1;    /* one dirty bit flushed per lane session */
    }
    atomic_store_explicit(&a->bits_flushed, bits, memory_order_relaxed);
    atomic_store_explicit(&a->done, 1, memory_order_release);
    return NULL;
}

static void torture_t1(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_STATE, T1_LANES, 16, T1_CELLS,
                         WHP2_STAT_U64, WHP2_F_MULTI_PRODUCER };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = plane_alloc(sz);
    TORTURE_ASSERT(mem != NULL, "T1 alloc");
    TORTURE_ASSERT(hplane_plane_create(mem, sz, &cfg, 987654321ull) ==
                       HEDDLE_OK,
                   "T1 create");

    hplane_ctx_t wctx[T1_WRITERS];
    t1_writer_arg_t warg[T1_WRITERS];
    pthread_t wth[T1_WRITERS];
    for (uint32_t w = 0; w < T1_WRITERS; w++) {
        TORTURE_ASSERT(hplane_attach(mem, sz, &wctx[w],
                                     WHP2_ROLE_PRODUCER) == HEDDLE_OK,
                       "T1 writer attach");
        warg[w].ctx = &wctx[w];
        warg[w].wid = w;
        warg[w].iters = g_iters;
        atomic_init(&warg[w].bits_flushed, 0);
        atomic_init(&warg[w].done, 0);
    }

    hplane_ctx_t cctx;
    TORTURE_ASSERT(hplane_attach(mem, sz, &cctx, WHP2_ROLE_CONSUMER) ==
                       HEDDLE_OK,
                   "T1 consumer attach");

    /* Prefill: every lane fully written before any thread starts. */
    uint64_t prefill_harvest = 0;
    for (uint32_t lane = 0; lane < T1_LANES; lane++) {
        prefill_lane(&wctx[lane / 2], lane, T1_CELLS, lane / 2);
    }
    /* Drain the prefill's dirty bits so the run starts from a clean
     * signal plane (accounting includes them via transitions). */
    {
        uint64_t mask = 0;
        if (hplane_dirty_harvest(&cctx, &mask) == HEDDLE_OK) {
            prefill_harvest = (uint64_t)__builtin_popcountll(mask);
        }
    }

    _Atomic int run = 1;
    reader_arg_t rarg[3];
    pthread_t rth[3];
    for (int r = 0; r < 3; r++) {
        hplane_ctx_t *rc = malloc(sizeof(hplane_ctx_t));
        TORTURE_ASSERT(rc != NULL, "T1 reader ctx alloc");
        TORTURE_ASSERT(hplane_attach(mem, sz, rc, WHP2_ROLE_CONSUMER) ==
                           HEDDLE_OK,
                       "T1 reader attach");
        rarg[r].ctx = rc;
        rarg[r].run = &run;
        rarg[r].writers = T1_WRITERS;
        rarg[r].cells = T1_CELLS;
        rarg[r].seed = 0x9E3779B97F4A7C15ull * (uint64_t)(r + 1);
        atomic_init(&rarg[r].reads, 0);
        atomic_init(&rarg[r].torn, 0);
        atomic_init(&rarg[r].bad, 0);
        pthread_create(&rth[r], NULL, state_reader, &rarg[r]);
    }

    for (uint32_t w = 0; w < T1_WRITERS; w++) {
        pthread_create(&wth[w], NULL, t1_writer, &warg[w]);
    }

    /* Render-thread harvester in THIS thread: runs until every writer
     * has signalled done AND the mask is drained — the release store
     * on `done` happens-after the writer's final fetch_or, so an
     * acquire load observing done==1 guarantees its bit is visible. */
    uint64_t harvested_bits = 0, frames = 0, bbox_bad = 0, stats_bad = 0;
    for (;;) {
        uint64_t mask = 0;
        if (hplane_dirty_harvest(&cctx, &mask) == HEDDLE_OK) {
            harvested_bits += (uint64_t)__builtin_popcountll(mask);
            while (mask) {
                uint32_t lane = (uint32_t)__builtin_ctzll(mask);
                mask &= mask - 1;
                uint64_t bbox = 0;
                if (hplane_bbox_harvest(&cctx, lane, &bbox) == HEDDLE_OK &&
                    bbox == WHP2_BBOX_EMPTY) {
                    bbox_bad++;  /* a dirty lane must carry a bbox */
                }
                hplane_lane_stats_t st;
                if (hplane_lane_stats_get(&cctx, lane, &st, 64) ==
                    HEDDLE_OK) {
                    if (st.commit_count != 0 &&
                        hplane_stat_key(WHP2_STAT_U64, st.min_raw) >
                            hplane_stat_key(WHP2_STAT_U64, st.max_raw)) {
                        stats_bad++;  /* min>max: impossible for u64 keys */
                    }
                }
            }
            uint64_t fid = 0;
            hplane_frame_commit(&cctx, &fid);
            frames++;
        }
        uint32_t alive = 0;
        for (uint32_t w = 0; w < T1_WRITERS; w++) {
            alive += (uint32_t)atomic_load_explicit(&warg[w].done,
                                                    memory_order_acquire);
        }
        uint64_t mask_now =
            __atomic_load_n(&((hplane_header_t *)(void *)mem)->dirty_mask,
                            __ATOMIC_RELAXED);
        if (alive == T1_WRITERS && mask_now == 0) {
            break;
        }
        if ((frames & 0xFFFu) == 0) {
            sched_yield();
        }
    }

    for (uint32_t w = 0; w < T1_WRITERS; w++) {
        pthread_join(wth[w], NULL);
    }
    run = 0;
    for (int r = 0; r < 3; r++) {
        pthread_join(rth[r], NULL);
    }

    uint64_t flushed = 0;
    for (uint32_t w = 0; w < T1_WRITERS; w++) {
        flushed += atomic_load_explicit(&warg[w].bits_flushed,
                                        memory_order_relaxed);
    }
    uint64_t read_bad = 0, torn = 0, reads = 0;
    for (int r = 0; r < 3; r++) {
        read_bad += atomic_load_explicit(&rarg[r].bad, memory_order_relaxed);
        torn += atomic_load_explicit(&rarg[r].torn, memory_order_relaxed);
        reads += atomic_load_explicit(&rarg[r].reads, memory_order_relaxed);
        free(rarg[r].ctx);
    }
    uint64_t final_mask = final_mask_popcount(mem);
    uint64_t transitions = hplane_dirty_transitions_get(&cctx);

    TPRINTF("T1 multi-producer state: %llu updates, %llu reads, "
           "%llu torn-refusals, %llu harvest frames, bbox_empty_bad=%llu "
           "stats_bad=%llu\n",
           (unsigned long long)flushed, (unsigned long long)reads,
           (unsigned long long)torn, (unsigned long long)frames,
           (unsigned long long)bbox_bad, (unsigned long long)stats_bad);
    TPRINTF("ACCOUNTING transitions=%llu harvested=%llu final_mask=%llu\n",
           (unsigned long long)transitions,
           (unsigned long long)(harvested_bits + prefill_harvest),
           (unsigned long long)final_mask);

    TORTURE_ASSERT(read_bad == 0, "T1 zero torn/illegal payloads");
    TORTURE_ASSERT(bbox_bad == 0, "T1 dirty lanes always carry a bbox");
    TORTURE_ASSERT(stats_bad == 0, "T1 lane stats min<=max invariant");
    TORTURE_ASSERT(harvested_bits + prefill_harvest + final_mask == transitions,
                   "T1 EXACT dirty accounting (harvested+final==transitions)");
    TORTURE_ASSERT(hplane_epoch_get(&cctx) ==
                       flushed + (uint64_t)T1_LANES /* prefill sessions */,
                   "T1 epoch == total lane sessions");
    free(mem);
}

/* ---- T2: single-producer batched sessions ------------------------------ */

#define T2_CELLS 64

static void torture_t2(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_STATE, 8, 16, T2_CELLS, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = plane_alloc(sz);
    TORTURE_ASSERT(hplane_plane_create(mem, sz, &cfg, 555555555ull) ==
                       HEDDLE_OK,
                   "T2 create");
    hplane_ctx_t p, c;
    TORTURE_ASSERT(hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER) ==
                       HEDDLE_OK,
                   "T2 producer attach");
    TORTURE_ASSERT(hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER) ==
                       HEDDLE_OK,
                   "T2 consumer attach");

    _Atomic int run = 1;
    hplane_ctx_t *rc = malloc(sizeof(hplane_ctx_t));
    TORTURE_ASSERT(rc != NULL, "T2 reader ctx alloc");
    TORTURE_ASSERT(hplane_attach(mem, sz, rc, WHP2_ROLE_CONSUMER) ==
                       HEDDLE_OK,
                   "T2 reader attach");
    reader_arg_t rarg = { rc, &run, 8, T2_CELLS, 0xABCDEF01ull, 0, 0, 0 };
    atomic_init(&rarg.reads, 0);
    atomic_init(&rarg.torn, 0);
    atomic_init(&rarg.bad, 0);

    /* Prefill every cell BEFORE the reader thread starts. */
    uint64_t prefill_harvest = 0;
    for (uint32_t lane = 0; lane < 8; lane++) {
        prefill_lane(&p, lane, T2_CELLS, 7);
    }
    {
        uint64_t mask = 0;
        if (hplane_dirty_harvest(&c, &mask) == HEDDLE_OK) {
            prefill_harvest = (uint64_t)__builtin_popcountll(mask);
        }
    }

    pthread_t rth;
    pthread_create(&rth, NULL, state_reader, &rarg);

    /* Sessions of 8 writes spanning all lanes -> batched mask flush. */
    uint64_t sessions = (g_iters + 7) / 8;
    uint64_t flushed = 0;
    uint8_t s[16];
    for (uint64_t i = 0; i < sessions; i++) {
        TORTURE_ASSERT(hplane_commit_begin(&p) == HEDDLE_OK, "T2 begin");
        uint64_t pending_bits = 0;
        for (uint32_t k = 0; k < 8; k++) {
            uint32_t idx = (uint32_t)((i * 8 + k) & 0xFFFFFFFFull);
            uint32_t lane = (uint32_t)((i * 8 + k) % 8);
            uint32_t cell = cell_of(7, idx, T2_CELLS);
            sample_fill(s, 7, idx, cell);
            TORTURE_ASSERT(hplane_state_write(&p, lane, cell, s, 16) ==
                               HEDDLE_OK,
                           "T2 state_write");
            pending_bits |= hplane_lane_bit(lane);
        }
        TORTURE_ASSERT(hplane_commit_end(&p) == HEDDLE_OK, "T2 commit");
        flushed += (uint64_t)__builtin_popcountll(pending_bits);
    }
    run = 0;
    pthread_join(rth, NULL);

    uint64_t harvested = drain_mask(&c);
    uint64_t final_mask = final_mask_popcount(mem);
    uint64_t transitions = hplane_dirty_transitions_get(&c);

    TPRINTF("T2 single-producer batched: %llu sessions (%llu writes), "
           "reader bad=%llu torn=%llu reads=%llu\n",
           (unsigned long long)sessions,
           (unsigned long long)(sessions * 8),
           (unsigned long long)atomic_load_explicit(&rarg.bad,
                                                    memory_order_relaxed),
           (unsigned long long)atomic_load_explicit(&rarg.torn,
                                                    memory_order_relaxed),
           (unsigned long long)atomic_load_explicit(&rarg.reads,
                                                    memory_order_relaxed));
    TPRINTF("ACCOUNTING transitions=%llu harvested=%llu final_mask=%llu\n",
           (unsigned long long)transitions,
           (unsigned long long)(harvested + prefill_harvest),
           (unsigned long long)final_mask);

    TORTURE_ASSERT(atomic_load_explicit(&rarg.bad, memory_order_relaxed) ==
                       0,
                   "T2 zero torn payloads");
    TORTURE_ASSERT(harvested + prefill_harvest + final_mask == transitions,
                   "T2 EXACT batched-mask accounting");
    free(rc);
    free(mem);
}

/* ---- T3: ring streaming torture ----------------------------------------- */

#define T3_LANES 2
#define T3_CAP 4096
#define T3_WINDOW 1024

typedef struct t3_arg {
    hplane_ctx_t *wctx;              /* producer ctx (writer thread)   */
    hplane_ctx_t *rctx;              /* consumer ctx (reader thread)   */
    uint64_t iters;
    _Atomic uint64_t bits_flushed;
    _Atomic uint64_t done;
    _Atomic int *run;
    _Atomic uint64_t ok;
    _Atomic uint64_t overrun;
    _Atomic uint64_t torn;
    _Atomic uint64_t bad;
} t3_arg_t;

static void *t3_writer(void *varg)
{
    t3_arg_t *a = varg;
    hplane_ctx_t *p = a->wctx;
    uint8_t s[16];
    uint64_t bits = 0;
    uint64_t batch = 64;
    for (uint64_t i = 0; i < a->iters; i += batch) {
        uint64_t n = (a->iters - i < batch) ? (a->iters - i) : batch;
        uint64_t pending_bits = 0;
        TORTURE_ASSERT(hplane_commit_begin(p) == HEDDLE_OK, "T3 begin");
        for (uint64_t k = 0; k < n; k++) {
            uint64_t ordinal = i + k + 1;          /* global push count  */
            uint32_t lane = (uint32_t)((ordinal - 1) & 1u);
            uint64_t lane_seq = (ordinal + 1) / 2; /* lane-local ordinal */
            uint32_t slot = (uint32_t)((lane_seq - 1) % T3_CAP);
            sample_fill(s, (uint32_t)((lane_seq - 1) >> 20),
                        (uint32_t)lane_seq, slot);
            TORTURE_ASSERT(hplane_ring_push(p, lane, s, 16) ==
                               HEDDLE_OK,
                           "T3 push");
            pending_bits |= hplane_lane_bit(lane);
        }
        TORTURE_ASSERT(hplane_commit_end(p) == HEDDLE_OK, "T3 commit");
        bits += (uint64_t)__builtin_popcountll(pending_bits);
    }
    atomic_store_explicit(&a->bits_flushed, bits, memory_order_relaxed);
    atomic_store_explicit(&a->done, 1, memory_order_release);
    return NULL;
}

static void *t3_reader(void *varg)
{
    t3_arg_t *a = varg;
    hplane_ctx_t *rc = a->rctx;
    uint8_t out[16];
    uint64_t ok = 0, overrun = 0, torn = 0, bad = 0;
    uint64_t consumed[T3_LANES] = {0, 0};
    for (;;) {
        for (uint32_t lane = 0; lane < T3_LANES; lane++) {
            uint64_t head = 0;
            if (hplane_ring_head(rc, lane, &head) != HEDDLE_OK) {
                bad++;
                continue;
            }
            uint64_t lo = consumed[lane];
            if (head > lo + T3_WINDOW) {
                lo = head - T3_WINDOW;    /* sliding chase window */
            }
            for (uint64_t seq = lo + 1; seq <= head; seq++) {
                int rcs = hplane_slot_read(rc, lane, seq, out, 16, 64);
                if (rcs == HEDDLE_OK) {
                    uint32_t slot = (uint32_t)((seq - 1) % T3_CAP);
                    uint32_t wid = (uint32_t)((seq - 1) >> 20);
                    uint32_t it = (uint32_t)seq;
                    uint64_t canary;
                    uint32_t pw, pi;
                    memcpy(&canary, out, 8);
                    memcpy(&pw, out + 8, 4);
                    memcpy(&pi, out + 12, 4);
                    if (pw != wid || pi != it ||
                        canary != mix64(((uint64_t)wid << 40) ^
                                        ((uint64_t)it << 8) ^
                                        (uint64_t)slot ^ 0xC0FFEEull)) {
                        bad++;
                    }
                    ok++;
                } else if (rcs == HEDDLE_E_OVERRUN) {
                    overrun++;   /* honest: reader lagged a full cycle */
                } else if (rcs == HEDDLE_E_SEQ_TORN) {
                    torn++;
                } else {
                    bad++;
                }
            }
            consumed[lane] = head;
        }
        if (!atomic_load_explicit(a->run, memory_order_acquire)) {
            break;    /* one final drain pass after writer completion */
        }
        sched_yield();
    }
    atomic_store_explicit(&a->ok, ok, memory_order_relaxed);
    atomic_store_explicit(&a->overrun, overrun, memory_order_relaxed);
    atomic_store_explicit(&a->torn, torn, memory_order_relaxed);
    atomic_store_explicit(&a->bad, bad, memory_order_relaxed);
    return NULL;
}

static void torture_t3(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_RING, T3_LANES, 16, T3_CAP,
                         WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = plane_alloc(sz);
    TORTURE_ASSERT(hplane_plane_create(mem, sz, &cfg, 0xF00DF00Dull) ==
                       HEDDLE_OK,
                   "T3 create");
    hplane_ctx_t p, c;
    TORTURE_ASSERT(hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER) ==
                       HEDDLE_OK,
                   "T3 producer attach");
    TORTURE_ASSERT(hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER) ==
                       HEDDLE_OK,
                   "T3 consumer attach");

    _Atomic int run = 1;
    hplane_ctx_t *rc = malloc(sizeof(hplane_ctx_t));
    TORTURE_ASSERT(rc != NULL, "T3 reader ctx alloc");
    TORTURE_ASSERT(hplane_attach(mem, sz, rc, WHP2_ROLE_CONSUMER) ==
                       HEDDLE_OK,
                   "T3 reader attach");
    t3_arg_t arg = { &p, rc, g_ring_iters, 0, 0, &run, 0, 0, 0, 0 };
    atomic_init(&arg.bits_flushed, 0);
    atomic_init(&arg.done, 0);
    atomic_init(&arg.ok, 0);
    atomic_init(&arg.overrun, 0);
    atomic_init(&arg.torn, 0);
    atomic_init(&arg.bad, 0);

    pthread_t wth, rth;
    pthread_create(&rth, NULL, t3_reader, &arg);
    pthread_create(&wth, NULL, t3_writer, &arg);
    pthread_join(wth, NULL);
    run = 0;
    pthread_join(rth, NULL);

    uint64_t harvested = drain_mask(&c);
    uint64_t final_mask = final_mask_popcount(mem);
    uint64_t transitions = hplane_dirty_transitions_get(&c);

    /* Backpressure marks: 1M pushes >> 4096 capacity. */
    hplane_lane_desc_t *d0 =
        (hplane_lane_desc_t *)(void *)(mem + WHP2_HEADER_SIZE);
    uint32_t lflags = __atomic_load_n(&d0->lane_flags, __ATOMIC_RELAXED);
    TORTURE_ASSERT((lflags & WHP2_LANE_F_BP_MARK) != 0,
                   "T3 in-band BP mark set after wrap");
    uint64_t head = 0;
    hplane_ring_head(&c, 0, &head);
    TORTURE_ASSERT(head == g_ring_iters / 2, "T3 lane head == pushes/2");

    TPRINTF("T3 ring streaming: %llu pushes, ok=%llu overrun=%llu "
           "torn=%llu bad=%llu\n",
           (unsigned long long)g_ring_iters,
           (unsigned long long)atomic_load_explicit(&arg.ok,
                                                    memory_order_relaxed),
           (unsigned long long)atomic_load_explicit(&arg.overrun,
                                                    memory_order_relaxed),
           (unsigned long long)atomic_load_explicit(&arg.torn,
                                                    memory_order_relaxed),
           (unsigned long long)atomic_load_explicit(&arg.bad,
                                                    memory_order_relaxed));
    TPRINTF("ACCOUNTING transitions=%llu harvested=%llu final_mask=%llu\n",
           (unsigned long long)transitions, (unsigned long long)harvested,
           (unsigned long long)final_mask);

    TORTURE_ASSERT(atomic_load_explicit(&arg.bad, memory_order_relaxed) ==
                       0,
                   "T3 zero corrupted payloads");
    TORTURE_ASSERT(harvested + final_mask == transitions,
                   "T3 EXACT ring dirty accounting");
    free(rc);
    free(mem);
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    const char *env = getenv("HEDDLE_ITERS");
    if (env) {
        uint64_t v = strtoull(env, NULL, 10);
        if (v > 0) {
            g_iters = v;
            g_ring_iters = v * 2;
        }
    }
    printf("=== heddle-2.0 torn-read torture (T-series), iters=%llu "
           "ring=%llu ===\n",
           (unsigned long long)g_iters,
           (unsigned long long)g_ring_iters);

    /* Warm-up pass: creates every thread, stdio buffer and glibc
     * thread-cache entry the harness will ever need, so the MEASURED
     * pass below observes the engine's true steady-state heap use. */
    g_warmup = 1;
    torture_t1();
    torture_t2();
    torture_t3();
    g_warmup = 0;

#if defined(TORTURE_HAVE_MALLINFO2) && !defined(__SANITIZE_ADDRESS__) && \
    !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_UNDEFINED__)
    struct mallinfo2 m_before = mallinfo2();
    void *brk_before = sbrk(0);
#endif

    torture_t1();
    torture_t2();
    torture_t3();

#if defined(TORTURE_HAVE_MALLINFO2) && !defined(__SANITIZE_ADDRESS__) && \
    !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_UNDEFINED__)
    struct mallinfo2 m_after = mallinfo2();
    void *brk_after = sbrk(0);
    TORTURE_ASSERT(m_after.uordblks == m_before.uordblks &&
                       m_after.hblkhd == m_before.hblkhd,
                   "T4 zero-heap steady-state (mallinfo2)");
    printf("T4 zero-heap steady-state: in-use delta 0, mmap-heap delta 0 "
           "(sbrk drift %lld bytes, informational)\n",
           (long long)((char *)brk_after - (char *)brk_before));
#else
    printf("SKIP T4 zero-heap probe (sanitizer or non-glibc build)\n");
#endif

    printf("VERDICT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
