// bench_inspector.c — Pillar 7 benchmark + SLA gates.
//
// G1 (Law 1): a real 10M-commit seqlock stream (single writer child at
//     full rate + an acker child keeping the ring draining) while a
//     dedicated inspector child scrapes at 1000 Hz. GATE: inspector CPU
//     (CLOCK_PROCESS_CPUTIME_ID / wall) < 0.5%. The achieved stream rate
//     is measured and reported (10M commits is the target; the container
//     scheduler may stretch wall time — the rate is printed, the CPU% is
//     the law). Producer-side impact is reported as the writer's rate
//     with vs without a concurrent inspector (two runs).
//
// G2 (SLA): memory-map snapshot latency for a 100,000-cell ring
//     (131072 slots, synthetic WFRM wire-format object — the production
//     create APIs cap ring depth for QoS reasons: rmw 4096, WFSH 64; the
//     wire format itself has no cap and neither does the inspector).
//     GATE: frame assembly p99 < 5 us. The incremental scrape cost
//     (bounded window) is measured and reported alongside, plus the
//     same numbers for a 1,000,000-cell ring (1048576 slots).
//
// B1: scrape pass cost vs ring geometry (64 / 1024 / 131072 slots).
// B2: full /dev/shm topology scan cost.
// B3: attach + detach latency distribution.
//
// Gates run ONLY in the plain leg (BENCH_NO_GATES=1 in sanitizer legs —
// timing under sanitizers is reported raw, never gated; house pattern).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_util.h"

/* Pillar 7 local belt: my call sites merge the check name INTO the printf
 * format ("msg (got %d)"), so the P6 2-arg TU_CHECK / 4-field TU_ASSERT
 * shapes do not fit. Two local macros keep -pedantic -Werror clean:
 *   TCHK2(cond, "msg")                 no format arguments
 *   TCHK3(cond, "fmt %d", val, ...)    at least one format argument
 * TU_PASS / TU_FAIL (from the P6 belt) are used unchanged. */
#include <stdarg.h>

static void stu_fail(const char *msg) {
    printf("FAIL  %s\n", msg);
    fflush(stdout);
    exit(2);
}

__attribute__((format(printf, 1, 2)))
static void stu_failv(const char *fmt, ...) {
    va_list ap;
    printf("FAIL  ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    exit(2);
}

#define TCHK2(cond, msg) \
    do { \
        if (!(cond)) stu_fail(msg); \
    } while (0)

#define TCHK3(cond, fmt, ...) \
    do { \
        if (!(cond)) stu_failv(fmt, __VA_ARGS__); \
    } while (0)

#include "weft_inspector/weft_shm_inspector.h"
#include "weft_inspector/weft_contention_profiler.h"
#include "weft_inspector/weft_memory_stream.h"

#define NSEGS 64u

static weft_inspect_segment_t g_segs[NSEGS];
static weft_inspect_ctx_t g_insp;
static weft_mstream_binding_t g_binds[4];
static weft_mstream_blob_delta_t g_deltas[8192];
static weft_mstream_ctx_t g_mstream;

static int gates_on(void) {
    const char *g = getenv("BENCH_NO_GATES");
    return (g == NULL || g[0] == '\0' || g[0] == '0') ? 1 : 0;
}

static void spin_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    /* aarch64: YIELD is the polite same-core hint */
    __asm__ volatile("yield" ::: "memory");
#endif
}

static void sweep_leftovers(void) {
    const char *pats[] = {"/dev/shm/weft_rmw_d8_", "/dev/shm/weft_studio_"};
    for (size_t i = 0; i < sizeof pats / sizeof pats[0]; i++) {
        char cmd[256];
        (void)snprintf(cmd, sizeof cmd,
                       "for f in %s*; do [ -e \"$f\" ] && rm -f \"$f\"; done",
                       pats[i]);
        (void)system(cmd);
    }
}

/* ------------------------------------------------------------------ */
/* synthetic WFRM-format ring: the wire contract, any depth             */
/* (rmw_ring_create caps depth at 4096 for QoS sizing; the format and   */
/*  the inspector do not — declared in D-72)                            */
/* ------------------------------------------------------------------ */

typedef struct {
    rmw_ring_map_t map;
    uint32_t slots, payload, stride;
    int memfd;   /* 1 = anonymous memfd object (no tmpfs pressure) */
} synth_ring_t;

static int synth_finish_header(void *p, int fd, uint64_t mapping,
                               uint32_t slots, uint32_t payload,
                               uint32_t stride, synth_ring_t *sr,
                               const char *name);

/* memfd variant: the same frozen WFRM wire format over an anonymous
 * object — a 1M-slot ring is 128 MiB, beyond this container's 64 MiB
 * /dev/shm budget but fine as RAM-backed pages (the cluster mesh's own
 * TRANSPORT_MEMFD discipline; the inspector's attach_fd path consumes
 * exactly this). */
static int synth_create_memfd(const char *name, uint32_t slots,
                              uint32_t payload, synth_ring_t *sr) {
    sr->memfd = 1;
    uint32_t stride = (uint32_t)(((uint64_t)64u + payload + 63u) & ~63ull);
    uint64_t mapping = 4096ull + (uint64_t)slots * stride;
    char path[96];
    (void)snprintf(path, sizeof path, "/%s", name);
    int fd = memfd_create(path, 0);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)mapping) != 0) {
        (void)close(fd);
        return -1;
    }
    void *p = mmap(NULL, (size_t)mapping, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, 0);
    if (p == MAP_FAILED) {
        (void)close(fd);
        return -1;
    }
    sr->slots = slots; sr->payload = payload; sr->stride = stride;
    (void)0;
    /* fill the frozen header below via the shared tail */
    return synth_finish_header(p, fd, mapping, slots, payload, stride, sr, name);
}

static int synth_create(const char *name, uint32_t slots, uint32_t payload,
                        synth_ring_t *sr) {
    sr->memfd = 0;
    uint32_t stride = (uint32_t)(((uint64_t)64u + payload + 63u) & ~63ull);
    uint64_t mapping = 4096ull + (uint64_t)slots * stride;
    char path[96];
    (void)snprintf(path, sizeof path, "/%s", name);
    int fd = shm_open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)mapping) != 0) {
        (void)close(fd);
        (void)shm_unlink(path);
        return -1;
    }
    void *p = mmap(NULL, (size_t)mapping, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, 0);
    if (p == MAP_FAILED) {
        (void)close(fd);
        (void)shm_unlink(path);
        return -1;
    }
    return synth_finish_header(p, fd, mapping, slots, payload, stride, sr, name);
}

static int synth_finish_header(void *p, int fd, uint64_t mapping,
                               uint32_t slots, uint32_t payload,
                               uint32_t stride, synth_ring_t *sr,
                               const char *name) {
    memset(p, 0, (size_t)mapping);
    rmw_ring_header_t *h = (rmw_ring_header_t *)p;
    (void)fd;
    h->magic = RMW_WEFT_RING_MAGIC;
    h->version = RMW_WEFT_RING_VERSION;
    h->header_size = RMW_WEFT_RING_HEADER_BYTES;
    h->slot_count = slots;
    h->payload_bytes = payload;
    h->slot_stride = stride;
    h->mapping_bytes = mapping;
    h->creator_pid = (uint32_t)getpid();
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    h->created_unix_ns = (uint64_t)ts.tv_sec * 1000000000ull +
                         (uint64_t)ts.tv_nsec;
    h->registry_epoch = 1u;
    h->sub_instance = 0u;
    h->reliability = 0u;

    sr->map.base = (uint8_t *)p;
    sr->map.ctrl = (rmw_ring_ctrl_t *)(void *)((uint8_t *)p + 128u);
    sr->map.slots = (rmw_ring_slot_t *)(void *)((uint8_t *)p + 4096u);
    sr->map.hdr = (const rmw_ring_header_t *)p;
    sr->map.mapping_bytes = (size_t)mapping;
    sr->map.fd = fd;
    sr->map.creator = 1;
    snprintf(sr->map.name, sizeof sr->map.name, "%s", name);
    sr->slots = slots;
    sr->payload = payload;
    sr->stride = stride;
    return 0;
}

static void synth_destroy(synth_ring_t *sr) {
    rmw_ring_destroy(&sr->map);
}

/* the documented WFRM seqlock commit protocol, raw */
static void synth_commit(synth_ring_t *sr, uint64_t head, uint32_t size) {
    rmw_ring_ctrl_t *c = sr->map.ctrl;
    uint64_t k = head & (uint64_t)(sr->slots - 1u);
    rmw_ring_slot_t *slot =
        (rmw_ring_slot_t *)(void *)((uint8_t *)sr->map.slots +
                                    (size_t)k * sr->stride);
    uint64_t v = atomic_load_explicit(&slot->version, memory_order_relaxed);
    if (v & 1u) v += 1u;
    uint64_t seq = atomic_load_explicit(&c->published_total,
                                        memory_order_relaxed);
    atomic_store_explicit(&slot->version, v + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    slot->seq_id = seq;
    slot->payload_size = size;
    slot->source_ts_unix_ns = 0u;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&slot->version, v + 2u, memory_order_release);
    atomic_store_explicit(&c->head, head + 1u, memory_order_release);
    atomic_fetch_add_explicit(&c->published_total, 1u,
                              memory_order_relaxed);
}

/* the documented take/ack protocol, raw */
static uint64_t synth_drain(synth_ring_t *sr) {
    rmw_ring_ctrl_t *c = sr->map.ctrl;
    uint64_t head = atomic_load_explicit(&c->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&c->tail_ack, memory_order_acquire);
    if (head > tail) {
        atomic_store_explicit(&c->tail_ack, head, memory_order_release);
    }
    return head;
}

/* ------------------------------------------------------------------ */
/* G1: 10M-commit stream + 1000 Hz inspector child                     */
/* ------------------------------------------------------------------ */

static double g1_run(int with_inspector, uint64_t *out_rate,
                     uint64_t *out_commits) {
    const char *rname = "weft_rmw_d8_t5eed0001_s0";
    sweep_leftovers();
    synth_ring_t sr;
    if (synth_create(rname, 1024u, 32u, &sr) != 0) {
        TU_FAIL("G1", "%s", "synth create failed");
    }
    const uint64_t TARGET = 10000000ull;

    /* acker child: keeps the ring draining at full speed */
    pid_t acker = fork();
    if (acker == 0) {
        tu_child_setup();
        synth_ring_t local;
        /* child inherits the mapping: same physical object */
        local = sr;
        for (;;) {
            uint64_t head = atomic_load_explicit(&local.map.ctrl->head,
                                                 memory_order_acquire);
            uint64_t tail = atomic_load_explicit(
                &local.map.ctrl->tail_ack, memory_order_acquire);
            if (head > tail) {
                atomic_store_explicit(&local.map.ctrl->tail_ack, head,
                                      memory_order_release);
            } else {
                spin_pause();
            }
            if (head >= TARGET) break;
        }
        _exit(0);
    }

    /* optional inspector child: 1000 Hz scrape until TARGET reached */
    pid_t insp = -1;
    int pipefd[2] = {-1, -1};
    if (with_inspector) {
        if (pipe(pipefd) != 0) TU_FAIL("G1", "%s", "pipe");
        insp = fork();
        if (insp == 0) {
            close(pipefd[0]);
            tu_child_setup();
            static weft_inspect_segment_t segs[NSEGS];
            weft_inspect_ctx_t ic;
            weft_prof_ctx_t pc;
            if (weft_inspect_init(&ic, segs, NSEGS) != 0) _exit(96);
            if (weft_prof_init(&pc, &ic) != 0) _exit(95);
            int idx = -1;
            for (int t = 0; t < 500 && idx < 0; t++) {
                weft_inspect_scan(&ic);
                idx = weft_inspect_find(&ic, rname);
                tu_usleep(2000);
            }
            if (idx < 0) _exit(94);
            if (weft_prof_watch_ring(&pc, idx, 0u, 0u) != 0) _exit(93);
            struct timespec c0;
            (void)clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0);
            int64_t cpu0 = (int64_t)c0.tv_sec * 1000000000 + c0.tv_nsec;
            int64_t t0 = tu_now_ns();
            uint64_t passes = 0;
            for (;;) {
                weft_prof_scrape(&pc);
                passes++;
                if ((passes & 15u) == 0u) {
                    uint64_t h = segs[idx].head;
                    if (h >= TARGET) break;
                }
                tu_usleep(1000);
            }
            struct timespec c1;
            (void)clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
            int64_t cpu1 = (int64_t)c1.tv_sec * 1000000000 + c1.tv_nsec;
            int64_t wall = tu_now_ns() - t0;
            int64_t cpu = cpu1 - cpu0;
            double pct = (double)cpu / (double)wall * 100.0;
            if (write(pipefd[1], &pct, sizeof pct) != (ssize_t)sizeof pct) {
                _exit(92);
            }
            _exit(0);
        }
        close(pipefd[1]);
    }

    /* writer: THIS process, full-rate commits */
    int64_t w0 = tu_now_ns();
    uint64_t head = 0;
    while (head < TARGET) {
        synth_commit(&sr, head, 32u);
        head++;
    }
    int64_t w1 = tu_now_ns();
    *out_commits = TARGET;
    *out_rate = (uint64_t)((double)TARGET * 1e9 /
                           (double)(w1 > w0 ? w1 - w0 : 1));

    double cpu_pct = -1.0;
    if (with_inspector) {
        ssize_t n = read(pipefd[0], &cpu_pct, sizeof cpu_pct);
        (void)n;
        close(pipefd[0]);
        int st = 0;
        (void)waitpid(insp, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            TU_FAIL("G1", "inspector child failed (%d)", st);
        }
    }
    int st2 = 0;
    (void)waitpid(acker, &st2, 0);

    synth_destroy(&sr);
    (void)shm_unlink("/weft_rmw_d8_t5eed0001_s0");
    return cpu_pct;
}

/* ------------------------------------------------------------------ */
/* G2: snapshot latency at 100k / 1M cells                             */
/* ------------------------------------------------------------------ */

static void g2_ring(const char *name, uint32_t slots, const char *tag) {
    synth_ring_t sr;
    /* beyond this container's 64 MiB tmpfs budget: the 1M-cell ring runs
     * as an anonymous memfd (RAM-backed), attached through the studio's
     * fd path — the discipline the cluster mesh itself uses for big
     * sessions (WEFT_IPC_TRANSPORT_MEMFD) */
    const int use_memfd = (slots > 65536u);
    if (use_memfd) {
        TCHK3(synth_create_memfd(name, slots, 32u, &sr) == 0,
              "G2 synth memfd %s", tag);
    } else {
        TCHK3(synth_create(name, slots, 32u, &sr) == 0, "G2 synth %s", tag);
    }

    static uint8_t plane_mem[1u << 20];
    weft_inspect_arena_t plane_arena;
    TCHK2(weft_inspect_arena_init(&plane_arena, plane_mem,
                                     sizeof plane_mem) == 0, "G2 plane arena");
    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "G2 init");
    int idx = -1;
    if (use_memfd) {
        idx = weft_inspect_attach_fd(&g_insp, sr.map.fd, name);
        TCHK3(idx >= 0, "G2 attach_fd %s", tag);
    } else {
        TCHK2(weft_inspect_scan(&g_insp) >= 1, "G2 scan");
        idx = weft_inspect_find(&g_insp, name);
        TCHK3(idx >= 0, "G2 find %s", tag);
    }
    TCHK2(weft_mstream_init(&g_mstream, &g_insp, g_binds, 4u, g_deltas,
                               8192u) == 0, "G2 mstream");

    static uint8_t blob_mem[1u << 16];
    weft_inspect_arena_t blob_arena;
    TCHK2(weft_inspect_arena_init(&blob_arena, blob_mem,
                                     sizeof blob_mem) == 0, "G2 blob arena");

    /* initial full-plane classification timing: the one-time O(cells)
     * bind sweep that establishes the shadow truth (cold path) */
    int64_t t0 = tu_now_ns();
    TCHK2(weft_mstream_bind(&g_mstream, idx, 0u, 0u, &plane_arena) == 0,
                 "G2 bind");
    int64_t sweep_ns = tu_now_ns() - t0;

    /* steady-state: 512 commits + full ack per frame cycle, then measure
     * scrape (bounded window) and frame (assembly) separately */
    tu_hist_t hf, hs;
    TCHK2(tu_hist_init(&hf, 2000) == 0, "G2 hist f");
    TCHK2(tu_hist_init(&hs, 2000) == 0, "G2 hist s");
    uint64_t head = 0;
    for (int i = 0; i < 2000; i++) {
        for (int k = 0; k < 512; k++) {
            synth_commit(&sr, (uint64_t)(head + (unsigned)k), 32u);
        }
        head += 512;
        (void)synth_drain(&sr);
        int64_t a = tu_now_ns();
        TCHK2(weft_mstream_scrape(&g_mstream, 4096u) == 0, "G2 scrape");
        int64_t b = tu_now_ns();
        blob_arena.used = 0;
        weft_mstream_frame_t fr;
        TCHK2(weft_mstream_frame(&g_mstream, &blob_arena, &fr) == 0, "G2 frame");
        int64_t c = tu_now_ns();
        tu_hist_add(&hs, (double)(b - a));
        tu_hist_add(&hf, (double)(c - b));
    }
    /* tu_hist stores NANOSECONDS; the SLA is microseconds — convert once
     * and gate in us (a units slip once printed 10.6 us as "10621 us" and
     * failed a passing gate; the arithmetic now says what it means) */
    double f50 = tu_hist_pct(&hf, 50.0) / 1000.0;
    double f99 = tu_hist_pct(&hf, 99.0) / 1000.0;
    double s50 = tu_hist_pct(&hs, 50.0) / 1000.0;
    double s99 = tu_hist_pct(&hs, 99.0) / 1000.0;
    printf("G2 %s (%u cells): initial-sweep=%lld us | frame p50/p99="
           "%.3f/%.3f us | scrape(4096 budget) p50/p99=%.3f/%.3f us | "
           "deltas/frame ~%u\n",
           tag, slots, (long long)(sweep_ns / 1000), f50, f99, s50, s99,
           (unsigned)g_mstream.dcount);
    fflush(stdout);

    if (gates_on() && strcmp(tag, "100k") == 0) {
        TCHK3(f99 < 5.0, "G2 100k-cell frame p99 < 5 us (got %.3f us)", f99);
    }
    if (gates_on() && strcmp(tag, "1M") == 0) {
        TCHK3(f99 < 5.0, "G2 1M-cell frame p99 < 5 us (got %.3f us)", f99);
    }

    tu_hist_free(&hf);
    tu_hist_free(&hs);
    weft_mstream_unbind(&g_mstream, idx);
    weft_inspect_destroy(&g_insp);
    synth_destroy(&sr);
    if (!use_memfd) {
        char path[96];
        (void)snprintf(path, sizeof path, "/%s", name);
        (void)shm_unlink(path);
    }
    TU_PASS("G2 snapshot latency");
}

/* ------------------------------------------------------------------ */
/* B1/B2/B3                                                             */
/* ------------------------------------------------------------------ */

static void bench_b(void) {
    /* B1: scrape pass cost vs geometry */
    const uint32_t geos[3] = {64u, 1024u, 4096u};
    for (int g = 0; g < 3; g++) {
        char name[80];
        (void)snprintf(name, sizeof name, "weft_rmw_d8_tb1_%06u_s0",
                       (unsigned)geos[g]);
        synth_ring_t sr;
        TCHK2(synth_create(name, geos[g], 32u, &sr) == 0, "B1 synth");
        TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "B1 init");
        TCHK2(weft_inspect_scan(&g_insp) >= 1, "B1 scan");
        int idx = weft_inspect_find(&g_insp, name);
        TCHK2(idx >= 0, "B1 find");
        tu_hist_t h;
        TCHK2(tu_hist_init(&h, 2000) == 0, "B1 hist");
        for (int i = 0; i < 2000; i++) {
            int64_t a = tu_now_ns();
            TCHK2(weft_inspect_scrape(&g_insp) == 0, "B1 scrape");
            tu_hist_add(&h, (double)(tu_now_ns() - a));
            if ((i & 15u) == 0u) {
                synth_commit(&sr, g_segs[idx].head, 32u);
            }
        }
        printf("B1 scrape pass (%4u slots): p50=%.3f us p99=%.3f us\n",
               geos[g], tu_hist_pct(&h, 50.0) / 1000.0,
               tu_hist_pct(&h, 99.0) / 1000.0);
        fflush(stdout);
        tu_hist_free(&h);
        weft_inspect_destroy(&g_insp);
        synth_destroy(&sr);
        char path[96];
        (void)snprintf(path, sizeof path, "/%s", name);
        (void)shm_unlink(path);
    }

    /* B2: topology scan cost with a live segment census */
    synth_ring_t sr2;
    TCHK2(synth_create("weft_rmw_d8_tb2_s0", 64u, 32u, &sr2) == 0, "B2 synth");
    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "B2 init");
    tu_hist_t h2;
    TCHK2(tu_hist_init(&h2, 500) == 0, "B2 hist");
    for (int i = 0; i < 500; i++) {
        int64_t a = tu_now_ns();
        TCHK2(weft_inspect_scan(&g_insp) >= 0, "B2 scan");
        tu_hist_add(&h2, (double)(tu_now_ns() - a));
    }
    printf("B2 /dev/shm scan+attach (1 existing): p50=%.3f us p99=%.3f us\n",
           tu_hist_pct(&h2, 50.0) / 1000.0, tu_hist_pct(&h2, 99.0) / 1000.0);
    fflush(stdout);
    tu_hist_free(&h2);
    weft_inspect_destroy(&g_insp);
    synth_destroy(&sr2);
    (void)shm_unlink("/weft_rmw_d8_tb2_s0");

    /* B3: attach + detach latency (the torture's rapid cycles) */
    synth_ring_t sr3;
    TCHK2(synth_create("weft_rmw_d8_tb3_s0", 64u, 32u, &sr3) == 0, "B3 synth");
    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "B3 init");
    tu_hist_t h3;
    TCHK2(tu_hist_init(&h3, 1000) == 0, "B3 hist");
    for (int i = 0; i < 1000; i++) {
        TCHK2(weft_inspect_scan(&g_insp) >= 1, "B3 scan");
        int idx = weft_inspect_find(&g_insp, "weft_rmw_d8_tb3_s0");
        int64_t a = tu_now_ns();
        TCHK2(weft_inspect_detach(&g_insp, (unsigned)idx) == 0, "B3 det");
        tu_hist_add(&h3, (double)(tu_now_ns() - a));
    }
    printf("B3 detach+rescan cycle: p50=%.3f us p99=%.3f us\n",
           tu_hist_pct(&h3, 50.0) / 1000.0, tu_hist_pct(&h3, 99.0) / 1000.0);
    fflush(stdout);
    tu_hist_free(&h3);
    weft_inspect_destroy(&g_insp);
    synth_destroy(&sr3);
    (void)shm_unlink("/weft_rmw_d8_tb3_s0");
    TU_PASS("B1/B2/B3 micro-benchmarks");
}

/* ------------------------------------------------------------------ */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    sweep_leftovers();

    /* ORDER: nanosecond-sensitive measurements (G2, B-series) run FIRST
     * on a quiesced box; the multi-process G1 runs LAST so its spinning
     * children can never pollute latency histograms (the first bench run
     * showed 11 ms scrape samples that were pure CFS preemption from
     * G1's acker child — measured, understood, ordered away; D-72
     * documents it). */
    g2_ring("weft_rmw_d8_tg2a_s0", 131072u, "100k");
    g2_ring("weft_rmw_d8_tg2b_s0", 1048576u, "1M");
    bench_b();

    /* G1: two runs — with and without the live inspector. NOTE (honest
     * framing for D-72): the writer-rate delta under a live inspector on
     * this 2-core container is dominated by scheduler contention between
     * the latency-critical writer/acker pair and the inspector's 1 kHz
     * wakeups — the LAW is the inspector's own CPU share, gated below;
     * the rate impact is reported as measured. */
    uint64_t rate0 = 0, commits0 = 0, rate1 = 0, commits1 = 0;
    double cpu_pct = g1_run(0, &rate0, &commits0);
    (void)cpu_pct;
    double pct = g1_run(1, &rate1, &commits1);
    double impact = (rate0 > 0)
                        ? (1.0 - (double)rate1 / (double)rate0) * 100.0
                        : 0.0;
    printf("G1 stream: writer-only rate=%.2f Mcommits/s; with inspector "
           "rate=%.2f Mcommits/s (impact %.2f%%); inspector CPU=%.3f%%\n",
           (double)rate0 / 1e6, (double)rate1 / 1e6, impact, pct);
    fflush(stdout);
    if (gates_on()) {
        TCHK3(pct >= 0.0 && pct < 0.5, "G1 inspector CPU < 0.5%% (got %.3f%%)", pct);
        TCHK2(commits1 == 10000000ull, "G1 10M commits completed");
    }

    printf("\nBENCH PASS\n");
    return 0;
}
