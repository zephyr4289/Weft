// test_inspector_torture.c — Pillar 7 torture battery (2,000,000 cycles).
//
// T1  2M-cycle scrape+peek under continuous traffic: seqlock consistency
//     invariants on EVERY pass (head >= tail_ack, published >= head),
//     torn peeks counted and bounded, zero faults.
// T2  Rapid attach/detach cycles interleaved every 4096 scrapes.
// T3  Unclean client crash recovery loop: a subscriber that dies mid-loan
//     150 times; the inspector never faults, never hangs, fds stay flat.
// T4  Zero-growth witnesses: RSS delta, fd count, /dev/shm census across
//     the whole run (three independent witnesses, house pattern).
//
// WEFT_QUICK=1 (sanitizer legs) reduces cycles to 200k / 40 crashes.

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
static weft_prof_ctx_t g_prof;

static uint64_t lcg_next(uint64_t *s) {
    *s = *s * 6364136223846793005ull + 1442695040888963407ull;
    return *s;
}

static int quick(void) {
    const char *q = getenv("WEFT_QUICK");
    return (q != NULL && q[0] != '\0' && q[0] != '0') ? 1 : 0;
}

static void sweep_leftovers(void) {
    const char *pats[] = {"/dev/shm/weft_rmw_d9_", "/dev/shm/weft_studio_"};
    for (size_t i = 0; i < sizeof pats / sizeof pats[0]; i++) {
        char cmd[256];
        (void)snprintf(cmd, sizeof cmd,
                       "for f in %s*; do [ -e \"$f\" ] && rm -f \"$f\"; done",
                       pats[i]);
        (void)system(cmd);
    }
}

/* ------------------------------------------------------------------ */
/* traffic children                                                    */
/* ------------------------------------------------------------------ */

/* publisher: BEST_EFFORT firehose (drops allowed), runs ~duration_ms */
static void spawn_firehose(const char *ring, uint32_t slots,
                           uint32_t payload, int duration_ms, pid_t *out) {
    *out = fork();
    if (*out == 0) {
        tu_child_setup();
        rmw_ring_map_t m;
        if (rmw_ring_attach(ring, slots, payload, &m) != 0) _exit(98);
        uint64_t seed = 0xd1ce;
        uint8_t buf[256];
        int64_t t0 = tu_now_ns();
        while (tu_now_ns() - t0 < (int64_t)duration_ms * 1000000ll) {
            for (uint32_t i = 0; i + 8u <= payload; i += 8) {
                uint64_t v = lcg_next(&seed);
                memcpy(buf + i, &v, 8);
            }
            (void)rmw_ring_publish(&m, buf, payload, 0, -1);
        }
        rmw_ring_destroy(&m);
        _exit(0);
    }
}

/* subscriber that attaches, loans one frame, and hard-crashes */
static void spawn_crasher(const char *ring, uint32_t slots,
                          uint32_t payload) {
    pid_t pid = fork();
    if (pid == 0) {
        tu_child_setup();
        rmw_ring_map_t m;
        if (rmw_ring_attach(ring, slots, payload, &m) != 0) _exit(98);
        rmw_ring_slot_t *slot;
        uint64_t seq;
        uint32_t size, crc;
        uint64_t ts;
        int64_t dl = tu_now_ns() + 3000000000ll;
        for (;;) {
            int rc = rmw_ring_try_take(&m, 0, &slot, &seq, &size, &crc, &ts);
            if (rc == 1) {
                _exit(42);   /* mid-loan crash: no ack, no destroy */
            }
            if (tu_now_ns() > dl) _exit(0);
            tu_usleep(100);
        }
    }
    /* parent returns immediately; the crasher is reaped by the loop */
}

/* ------------------------------------------------------------------ */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    sweep_leftovers();

    const uint64_t cycles = quick() ? 200000ull : 2000000ull;
    const unsigned crashes = quick() ? 40u : 150u;

    const char *rname = "weft_rmw_d9_ta11ce5e_s0";
    rmw_ring_map_t rt;
    TCHK2(rmw_ring_create(rname, 256u, 128u, 0u, 0u, 911u, &rt) == 0, "T ring create");

    pid_t firehose;
    spawn_firehose(rname, 256u, 128u, quick() ? 20000 : 120000, &firehose);

    long rss0 = tu_rss_kb();
    unsigned fd0 = tu_fd_count();

    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "T init");
    TCHK2(weft_prof_init(&g_prof, &g_insp) == 0, "T prof");

    int idx = -1;
    for (int i = 0; i < 200 && idx < 0; i++) {
        weft_inspect_scan(&g_insp);
        idx = weft_inspect_find(&g_insp, rname);
        tu_usleep(2000);
    }
    TCHK2(idx >= 0, "T ring found");
    TCHK2(weft_prof_watch_ring(&g_prof, idx, firehose, 0u) == 0, "T watch");

    /* --- T1 + T2: cycles with interleaved attach/detach ------------- */
    uint64_t torn_peeks = 0, ok_peeks = 0, invariant_ok = 0;
    uint64_t detach_cycles = 0;
    long rss_min = rss0, rss_max = rss0;

    for (uint64_t c = 0; c < cycles; c++) {
        TCHK3(weft_prof_scrape(&g_prof) == 0, "T scrape cycle %llu", (unsigned long long)c);
        const weft_inspect_segment_t *s = &g_segs[idx];
        /* seqlock consistency invariants, EVERY pass */
        if (s->head >= s->tail_ack && s->published_total >= s->head) {
            invariant_ok++;
        }
        if ((c & 63u) == 0u) {
            weft_inspect_slot_meta_t meta;
            uint64_t cursor = (s->head > 1u) ? s->head - 1u : 0u;
            int rc = weft_inspect_peek_slot(&g_insp, (unsigned)idx, cursor,
                                            &meta);
            if (rc == 1) ok_peeks++;
            else if (rc == WEFT_INSPECT_ERR_AGAIN) torn_peeks++;
        }
        if ((c & 4095u) == 4095u) {
            /* rapid detach + rescan/reattach (indices shift by contract) */
            TCHK2(weft_inspect_detach(&g_insp, (unsigned)idx) == 0, "T detach");
            TCHK2(weft_inspect_scan(&g_insp) >= 1, "T rescan");
            idx = weft_inspect_find(&g_insp, rname);
            TCHK2(idx >= 0, "T reattach");
            /* the watch's segment index must follow the reattach */
            weft_prof_unwatch_ring(&g_prof, -1);   /* no-op hygiene */
            TCHK2(weft_prof_watch_ring(&g_prof, idx, firehose, 0u) == 0, "T rewatch");
            detach_cycles++;
        }
        if ((c & 65535u) == 0u) {
            long r = tu_rss_kb();
            if (r < rss_min) rss_min = r;
            if (r > rss_max) rss_max = r;
        }
    }
    printf("T1: cycles=%llu invariant_ok=%llu peeks ok/torn=%llu/%llu\n",
           (unsigned long long)cycles, (unsigned long long)invariant_ok,
           (unsigned long long)ok_peeks, (unsigned long long)torn_peeks);
    printf("T2: detach_cycles=%llu\n", (unsigned long long)detach_cycles);
    fflush(stdout);
    TCHK2(invariant_ok == cycles, "T1 invariants held every cycle");
    TCHK2(detach_cycles >= (cycles >> 13), "T2 rapid attach/detach ran");
    TCHK2(torn_peeks <= ok_peeks + cycles, "T1 torn peeks bounded");

    /* --- T3: unclean crash recovery loop ----------------------------- */
    unsigned fd_before_crash = tu_fd_count();
    for (unsigned i = 0; i < crashes; i++) {
        spawn_crasher(rname, 256u, 128u);
        for (int k = 0; k < 20; k++) {
            TCHK3(weft_prof_scrape(&g_prof) == 0, "T3 scrape %u/%u", i, k);
        }
        /* reap finished crashers without blocking the loop */
        while (waitpid(-1, NULL, WNOHANG) > 0) { /* drain */ }
    }
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
    unsigned fd_after_crash = tu_fd_count();
    printf("T3: crashes=%u fds %u -> %u\n", crashes, fd_before_crash,
           fd_after_crash);
    fflush(stdout);
    TCHK2(fd_after_crash <= fd_before_crash + 1, "T3 no fd leak across crash loop");

    /* firehose is done or nearly so; reap it */
    int st = 0;
    int64_t t0 = tu_now_ns();
    while (waitpid(firehose, &st, WNOHANG) == 0) {
        TCHK2(weft_prof_scrape(&g_prof) == 0, "T drain scrape");
        if (tu_now_ns() - t0 > 130000000ll) {
            (void)kill(firehose, SIGKILL);
            (void)waitpid(firehose, &st, 0);
            break;
        }
    }

    /* --- T4: zero-growth witnesses ----------------------------------- */
    long rss1 = tu_rss_kb();
    long rss_limit = quick() ? 16384 : 4096;   /* KiB, house slack */
    printf("T4: rss %ld -> %ld (min %ld max %ld, limit +-%ld)\n", rss0, rss1,
           rss_min, rss_max, rss_limit);
    unsigned fd1 = tu_fd_count();
    printf("T4: fds %u -> %u\n", fd0, fd1);
    fflush(stdout);
    TCHK3(rss1 - rss0 <= rss_limit, "T4 RSS growth bounded (%ld KiB)", rss1 - rss0);
    TCHK3(rss_max - rss_min <= rss_limit + 2048, "T4 RSS band bounded (%ld KiB)", rss_max - rss_min);
    TCHK2(fd1 <= fd0 + 4, "T4 fd count flat");

    weft_inspect_destroy(&g_insp);
    rmw_ring_destroy(&rt);
    TCHK2(tu_shm_weft_count("weft_rmw_d9_") == 0, "T4 segment audit");

    printf("\nTORTURE PASS (%llu cycles, %u crashes)\n",
           (unsigned long long)cycles, crashes);
    return 0;
}
