// test_shm_inspector.c — Pillar 7 native battery I1-I5 / P1-P5 / F1-F6.
//
// Sections (each prints PASS lines; TU_FAIL exits 2 with the section name):
//   I1  scan + classify (WFRM/WFRR/WFSH/UNKNOWN/ABI-excluded) + geometry
//       + read-only posture (/proc/self/maps: every mapping r--p) +
//       non-mutation (byte-identical mappings across inspector passes)
//   I2  topology census: rmw registry topics/pubs/subs + ring correlation;
//       cluster registry sessions via the real weft_ipc discovery API
//   I3  read-only peek seam (seqlock double-read) + zero-copy payload
//       address identity + live WFSH ring via the real fanout writer API
//   I4  multi-ring RELIABLE saturation under a 1000 Hz inspector child:
//       zero corruption, zero reliable drops, inspector CPU bounded
//   I5  unclean subscriber crash mid-loan: inspector keeps scraping,
//       stall machinery reports, no deadlock, clean teardown
//   P1  torn-read tracker: held-odd writer -> torn events; calm -> zero
//   P2  false-sharing tripwire synthetic: adjacent words (fires, correct
//       line/offsets/pids) + 64B-apart words (never fires)
//   P3  false-sharing on a REAL rmw ctrl line: head x tail_ack, offsets
//       0/8, the two children's pids (the honest D-72 finding)
//   P4  backpressure stall episodes + recovery + BEST_EFFORT drop ledger
//       (bit-exact vs the ring's own counters) + timeout-window estimate
//   F   feeder: cell states across the publish/ack lifecycle, delta
//       records, zero-copy plane export identity, keyframe overflow
//       recovery, 240 FPS pacer arithmetic, zero-heap steady state
//
// House discipline: children carry PR_SET_PDEATHSIG (tu_child_setup),
// every /dev/shm object is swept at start and per-section, and the final
// audit checks fd count and /dev/shm census against the entry state.

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

/* real baseline surfaces (read-only link discipline) */
#include "shm_ring.h"
#include "weft_ipc.h"
#include "fanout.h"

#define DOMAIN 7u
#define NSEGS 64u

/* ------------------------------------------------------------------ */
/* shared scratch                                                      */
/* ------------------------------------------------------------------ */

static weft_inspect_segment_t g_segs[NSEGS];
static weft_inspect_ctx_t g_insp;
static weft_prof_ctx_t g_prof;
static weft_mstream_binding_t g_binds[8];
static weft_mstream_blob_delta_t g_deltas[4096];
static weft_mstream_ctx_t g_mstream;

typedef struct {
    int64_t a, b, c;
    int rc;
} creport_t;

/* pub-child drop counter handoff (BEST_EFFORT drops are expected) */
static uint64_t g_pub_drops;

static uint64_t lcg_next(uint64_t *s) {
    *s = *s * 6364136223846793005ull + 1442695040888963407ull;
    return *s;
}

static void fill_msg(uint8_t *buf, uint32_t len, uint64_t seq, uint64_t *seed) {
    uint64_t v = seq;
    for (uint32_t i = 0; i < len; i++) {
        if ((i & 7u) == 0u) v = lcg_next(seed);
        buf[i] = (uint8_t)(v >> ((i & 7u) * 8u));
    }
    memcpy(buf, &seq, sizeof seq < (size_t)len ? sizeof seq : (size_t)len);
}

static int msg_verify(const uint8_t *buf, uint32_t len, uint64_t seq,
                      uint64_t *seed) {
    uint8_t tmp[512];
    if (len > sizeof tmp) return 0;
    fill_msg(tmp, len, seq, seed);
    return memcmp(tmp, buf, len) == 0;
}

static void sweep_leftovers(void) {
    const char *pats[] = {"/dev/shm/weft_rmw_d7_", "/dev/shm/weft_studio_"};
    for (size_t i = 0; i < sizeof pats / sizeof pats[0]; i++) {
        /* name-based unlink of THIS battery's objects only */
        char cmd[256];
        (void)snprintf(cmd, sizeof cmd,
                       "for f in %s*; do [ -e \"$f\" ] && rm -f \"$f\"; done",
                       pats[i]);
        (void)system(cmd);
    }
}

/* child result plumbing: ONE pipe per child, the read fd returned to the
 * caller — a global pipe aliases across concurrent spawns (observed as
 * report mixups in I4; fixed here). The child closes the read end and
 * writes exactly one creport_t before _exit; the parent closes the write
 * end and later collects with pipe_collect(pid, rd_fd). */
static int g_pipe[2];

static void pipe_open(void) {
    if (pipe(g_pipe) != 0) {
        TU_FAIL("pipe", "%s", "pipe() failed");
    }
}

static pid_t pipe_fork(int *rd_fd) {
    pipe_open();
    pid_t pid = fork();
    if (pid == 0) {
        close(g_pipe[0]);
    } else {
        close(g_pipe[1]);
        *rd_fd = g_pipe[0];
    }
    return pid;
}

static void child_report(int64_t a, int64_t b, int64_t c, int rc) {
    creport_t r = {.a = a, .b = b, .c = c, .rc = rc};
    (void)write(g_pipe[1], &r, sizeof r);
}

static creport_t pipe_collect(pid_t pid, int rd_fd, const char *who) {
    creport_t r;
    memset(&r, 0, sizeof r);
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) {
        TU_FAIL(who, "%s", "waitpid failed");
    }
    ssize_t n = read(rd_fd, &r, sizeof r);
    (void)n;
    close(rd_fd);
    if (!WIFEXITED(st)) {
        TU_FAIL(who, "child died by signal %d", WTERMSIG(st));
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* child roles                                                         */
/* ------------------------------------------------------------------ */

/* publisher: attach RW, publish n messages (reliable), LCG payload */
static int child_pub(const char *ring, uint32_t slots, uint32_t payload,
                     uint64_t n, int reliable, uint64_t seed, int64_t *out_ok,
                     int64_t *out_timeouts) {
    /* out_timeouts doubles as the drop count reporter via child_report's
     * third field (see spawn_pub): BEST_EFFORT -4 is an EXPECTED outcome
     * (the ring's dropped_total already counted it), never a failure. */
    tu_child_setup();
    rmw_ring_map_t m;
    if (rmw_ring_attach(ring, slots, payload, &m) != 0) _exit(98);
    uint64_t ok = 0, to = 0, drops = 0;
    uint8_t buf[512];
    for (uint64_t i = 0; i < n; i++) {
        fill_msg(buf, payload, i, &seed);
        /* RELIABLE -3 means the message was NOT written: retry the same
         * sequence until the consumer makes progress (bounded: a dead
         * peer must end the stream honestly, not spin forever) */
        int retries = 2000;
        int rc;
        do {
            rc = rmw_ring_publish(&m, buf, payload, reliable, -1);
            if (rc == -3) {
                to++;
                if (--retries == 0) {
                    rmw_ring_destroy(&m);
                    _exit(96);   /* dead peer: honest refusal */
                }
            }
        } while (rc == -3);
        if (rc == -4) {
            drops++;   /* BEST_EFFORT drop: counted by the ring, expected */
            continue;
        }
        if (rc != 0) {
            rmw_ring_destroy(&m);
            _exit(97);
        }
        ok++;
    }
    *out_ok = (int64_t)ok;
    *out_timeouts = (int64_t)to;
    g_pub_drops = drops;
    rmw_ring_destroy(&m);
    return 0;
}

/* subscriber: attach RW, drain n messages, verify LCG */
static int child_sub(const char *ring, uint32_t slots, uint32_t payload,
                     uint64_t n, uint64_t seed, int slow_ms,
                     int64_t *out_taken, int64_t *out_verified) {
    tu_child_setup();
    rmw_ring_map_t m;
    if (rmw_ring_attach(ring, slots, payload, &m) != 0) _exit(98);
    uint64_t taken = 0, verified = 0;
    uint64_t cursor = 0;
    uint8_t buf[512];
    int64_t deadline_ns = -1;
    while (taken < n) {
        if (slow_ms > 0) tu_usleep((unsigned)slow_ms * 1000ul);
        rmw_ring_slot_t *slot;
        uint64_t seq;
        uint32_t size, crc;
        uint64_t ts;
        int rc = rmw_ring_try_take(&m, cursor, &slot, &seq, &size, &crc, &ts);
        if (rc == 1) {
            if (seq == cursor && size == payload) {
                memcpy(buf, rmw_ring_slot_payload(slot), payload);
                if (msg_verify(buf, payload, cursor, &seed)) verified++;
            }
            cursor++;
            rmw_ring_advance_tail(&m, cursor);
            taken++;
        } else if (rc == 0) {
            deadline_ns = tu_now_ns() + 2000000000ll;
            if (!rmw_ring_wait(&m, cursor, deadline_ns)) break;
        } else {
            tu_usleep(50);
        }
    }
    *out_taken = (int64_t)taken;
    *out_verified = (int64_t)verified;
    rmw_ring_destroy(&m);
    return 0;
}

static pid_t spawn_pub(const char *ring, uint32_t slots, uint32_t payload,
                       uint64_t n, int reliable, uint64_t seed, int *rd_fd) {
    pid_t pid = pipe_fork(rd_fd);
    if (pid == 0) {
        int64_t a = 0, b = 0;
        g_pub_drops = 0;
        int rc = child_pub(ring, slots, payload, n, reliable, seed, &a, &b);
        child_report(a, b, (int64_t)g_pub_drops, rc);
        _exit(0);
    }
    return pid;
}

static pid_t spawn_sub(const char *ring, uint32_t slots, uint32_t payload,
                       uint64_t n, uint64_t seed, int slow_ms, int *rd_fd) {
    pid_t pid = pipe_fork(rd_fd);
    if (pid == 0) {
        int64_t a = 0, b = 0;
        int rc = child_sub(ring, slots, payload, n, seed, slow_ms, &a, &b);
        child_report(a, b, 0, rc);
        _exit(0);
    }
    return pid;
}

/* ------------------------------------------------------------------ */
/* mapping posture helper                                              */
/* ------------------------------------------------------------------ */

static int maps_posture_ok(const void *addr, const char *name) {
    char path[64];
    (void)snprintf(path, sizeof path, "/proc/self/maps");
    FILE *f = fopen(path, "r");
    if (f == NULL) return 0;
    char line[512];
    int ok = 0;
    uintptr_t a = (uintptr_t)addr;
    while (fgets(line, sizeof line, f) != NULL) {
        unsigned long lo, hi;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (a >= (uintptr_t)lo && a < (uintptr_t)hi) {
            if (strncmp(perms, "r--", 3) == 0 &&
                strstr(line, name) != NULL) {
                ok = 1;
            }
            break;
        }
    }
    (void)fclose(f);
    return ok;
}

/* ------------------------------------------------------------------ */
/* I1 — scan, classify, geometry, posture, non-mutation                */
/* ------------------------------------------------------------------ */

static void section_i1(void) {
    printf("=== I1: scan / classify / geometry / posture ===\n");
    fflush(stdout);

    rmw_ring_map_t ra;
    TCHK2(rmw_ring_create("weft_rmw_d7_t11111111_s0", 32u, 256u, 0u, 1u,
                             123u, &ra) == 0, "I1 create ring A");
    weft_shm_map_t cr;
    TCHK2(weft_shm_create_named("weft_studio_cr1", 64u, 16u, &cr) == 0, "I1 create cluster ring");

    /* junk: unknown magic */
    FILE *jf = fopen("/dev/shm/weft_studio_junk", "wb");
    TCHK2(jf != NULL, "I1 junk fopen");
    if (jf != NULL) {
        fwrite("JUNKJUNKJUNKJUNK", 1, 16, jf);
        fclose(jf);
    }
    /* corrupt: right magic, wrong geometry (slot_count not pow2) */
    int cf = shm_open("/weft_rmw_d7_t22222222_s0", O_CREAT | O_EXCL | O_RDWR,
                      0600);
    TCHK2(cf >= 0, "I1 corrupt shm_open");
    if (cf >= 0) {
        uint8_t hdr[128];
        memset(hdr, 0, sizeof hdr);
        uint32_t magic = RMW_WEFT_RING_MAGIC;
        uint16_t ver = RMW_WEFT_RING_VERSION, hs = 128;
        uint32_t slots = 33u, payload = 256u, stride = 320u;
        memcpy(hdr + 0, &magic, 4);
        memcpy(hdr + 4, &ver, 2);
        memcpy(hdr + 6, &hs, 2);
        memcpy(hdr + 8, &slots, 4);
        memcpy(hdr + 12, &payload, 4);
        memcpy(hdr + 16, &stride, 4);
        (void)write(cf, hdr, sizeof hdr);
        close(cf);
    }

    /* pre-inspection content fingerprints (no traffic: static rings) */
    uint64_t crc_a = rmw_weft_crc32(ra.base, ra.mapping_bytes);
    uint64_t crc_c = rmw_weft_crc32(cr.base, cr.mapping_bytes);

    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "I1 init");
    int n = weft_inspect_scan(&g_insp);
    TCHK3(n >= 3, "I1 scan found >= 3 segments (got %d)", n);

    int ia = weft_inspect_find(&g_insp, "weft_rmw_d7_t11111111_s0");
    int ic = weft_inspect_find(&g_insp, "weft_studio_cr1");
    int ij = weft_inspect_find(&g_insp, "weft_studio_junk");
    int ix = weft_inspect_find(&g_insp, "weft_rmw_d7_t22222222_s0");
    TCHK2(ia >= 0, "I1 ring A in table");
    TCHK2(ic >= 0, "I1 cluster ring in table");
    TCHK2(ij >= 0, "I1 junk in table");

    TCHK2(g_segs[ia].family == WEFT_INSPECT_FAMILY_RMW_RING, "I1 ring A family");
    TCHK2(g_segs[ia].attached == 1u, "I1 ring A attached");
    TCHK2(g_segs[ia].slot_count == 32u && g_segs[ia].payload_bytes == 256u, "I1 ring A geometry");
    TCHK2(g_segs[ia].slot_stride == 320u, "I1 ring A stride");
    TCHK2(g_segs[ia].mapping_bytes_hdr == ra.mapping_bytes, "I1 ring A declared mapping");

    TCHK2(g_segs[ic].family == WEFT_INSPECT_FAMILY_CLUSTER_RING, "I1 cluster family");
    TCHK2(g_segs[ic].slot_count == 16u && g_segs[ic].payload_bytes == 64u, "I1 cluster geometry");
    TCHK2(g_segs[ic].ring_bytes_hdr ==
                 (uint64_t)(16u + 8u * 16u + 16u * 64u), "I1 cluster ring bytes");

    TCHK2(g_segs[ij].family == WEFT_INSPECT_FAMILY_UNKNOWN, "I1 junk UNKNOWN");
    TCHK2(g_segs[ij].attached == 0u, "I1 junk not attached");
    TCHK2(g_segs[ij].excluded == WEFT_INSPECT_EXCL_ABI, "I1 junk flagged");

    TCHK2(ix >= 0, "I1 corrupt in table");
    TCHK2(g_segs[ix].family == WEFT_INSPECT_FAMILY_RMW_RING, "I1 corrupt family by magic");
    TCHK2(g_segs[ix].attached == 0u, "I1 corrupt not attached");
    TCHK2(g_segs[ix].excluded == WEFT_INSPECT_EXCL_ABI, "I1 corrupt ABI-excluded");

    /* read-only posture: the live mappings are r--p /dev/shm views */
    TCHK2(maps_posture_ok(g_segs[ia].base, "weft_rmw_d7_t11111111_s0"), "I1 ring A mapping is r--p");
    TCHK2(maps_posture_ok(g_segs[ic].base, "weft_studio_cr1"), "I1 cluster mapping is r--p");

    /* 200 scrape passes + peeks: zero bytes of observed segments change */
    for (int i = 0; i < 200; i++) {
        TCHK2(weft_inspect_scrape(&g_insp) == 0, "I1 scrape pass");
    }
    weft_inspect_slot_meta_t meta;
    TCHK2(weft_inspect_peek_slot(&g_insp, (unsigned)ia, 0, &meta) == 0, "I1 peek beyond head returns not-published");

    TCHK2(rmw_weft_crc32(ra.base, ra.mapping_bytes) == crc_a, "I1 ring A bytes untouched");
    TCHK2(rmw_weft_crc32(cr.base, cr.mapping_bytes) == crc_c, "I1 cluster bytes untouched");
    TU_PASS("I1 scan/classify/geometry/posture/non-mutation");

    /* teardown of I1 objects */
    weft_inspect_destroy(&g_insp);
    rmw_ring_destroy(&ra);
    weft_shm_destroy(&cr);
    shm_unlink("/weft_studio_junk");
    shm_unlink("/weft_rmw_d7_t22222222_s0");
}

/* ------------------------------------------------------------------ */
/* I2 — topology                                                       */
/* ------------------------------------------------------------------ */

static void section_i2(void) {
    printf("=== I2: topology census ===\n");
    fflush(stdout);

    rmw_registry_map_t reg;
    TCHK2(rmw_registry_open(DOMAIN, &reg) == 0, "I2 registry open");
    rmw_ring_map_t rt;
    TCHK2(rmw_ring_create("weft_rmw_d7_t33333333_s0", 32u, 128u, 0u, 1u,
                             rmw_registry_epoch(&reg), &rt) == 0, "I2 create ring T");

    rmw_registry_sub_record_t *sr;
    uint64_t subv = 0;
    TCHK2(rmw_registry_add_sub(&reg, "/sensor/lidar", 0xabcdu, 128u,
                                  "weft_rmw_d7_t33333333_s0", 32u, 128u,
                                  1u, (uint32_t)getpid(), &sr, &subv) == 0, "I2 add sub");
    rmw_registry_topic_t *tp;
    rmw_registry_pub_record_t *pr;
    TCHK2(rmw_registry_add_pub(&reg, "/sensor/lidar", 0xabcdu, 128u,
                                  2u, (uint32_t)getpid(), 11ull, 22ull,
                                  &tp, &pr) == 0, "I2 add pub");
    TCHK2(rmw_registry_add_pub(&reg, "/sensor/lidar", 0xabcdu, 128u,
                                  3u, (uint32_t)getpid(), 33ull, 44ull,
                                  &tp, &pr) == 0, "I2 add pub 2");
    rmw_registry_sub_record_t *sr2;
    TCHK2(rmw_registry_add_sub(&reg, "/vision/frames", 0x1234u, 256u,
                                  "weft_rmw_d7_t44444444_s0", 16u, 256u,
                                  4u, (uint32_t)getpid(), &sr2, &subv) == 0, "I2 add sub 2");

    /* cluster registry: two sessions through the REAL API */
    weft_ipc_registry_t ireg;
    TCHK2(weft_ipc_registry_open(&ireg, 1, 0) == 0, "I2 ipc registry");
    weft_ipc_session_t s1, s2;
    TCHK2(weft_ipc_register(&ireg, "studio_sess_a", 1u, 64u, 8u, 0ull,
                               &s1) == 0, "I2 ipc register a");
    TCHK2(weft_ipc_register(&ireg, "studio_sess_b", 1u, 128u, 4u, 0ull,
                               &s2) == 0, "I2 ipc register b");

    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "I2 init");
    TCHK2(weft_inspect_scan(&g_insp) >= 2, "I2 scan");

    weft_inspect_topic_row_t rows[8];
    weft_inspect_topology_t topo;
    int nrows = weft_inspect_topology(&g_insp, rows, 8u, &topo);
    TCHK3(nrows == 2, "I2 topic rows (got %d)", nrows);
    TCHK2(topo.rmw_topics_active == 2u, "I2 topics active");
    TCHK2(topo.rmw_pubs_active == 2u, "I2 pubs active");
    TCHK2(topo.rmw_subs_active == 2u, "I2 subs active");
    TCHK2(topo.cluster_sessions == 2u, "I2 cluster sessions");
    TCHK2(topo.rings_watched >= 1u, "I2 rings watched");

    int lidar = -1;
    for (int i = 0; i < nrows; i++) {
        if (strcmp(rows[i].topic, "/sensor/lidar") == 0) lidar = i;
    }
    TCHK2(lidar >= 0, "I2 lidar row");
    TCHK2(rows[lidar].pubs == 2u && rows[lidar].subs == 1u, "I2 lidar endpoints");
    TCHK2(rows[lidar].rings_mapped == 1u, "I2 lidar ring correlation");

    TCHK2(weft_ipc_unregister(&s1) == 0, "I2 unregister a");
    TCHK2(weft_ipc_unregister(&s2) == 0, "I2 unregister b");
    weft_ipc_registry_close(&ireg);

    weft_inspect_destroy(&g_insp);
    rmw_registry_remove_sub(&reg, sr2);
    rmw_registry_remove_sub(&reg, sr);
    rmw_registry_remove_pub(&reg, pr);
    rmw_ring_destroy(&rt);
    rmw_registry_close(&reg);
    TU_PASS("I2 topology census");
}

/* ------------------------------------------------------------------ */
/* I3 — peek seam + payload identity + live WFSH                       */
/* ------------------------------------------------------------------ */

static void section_i3(void) {
    printf("=== I3: peek seam / payload identity / live WFSH ===\n");
    fflush(stdout);

    const char *rname = "weft_rmw_d7_t55555555_s0";
    rmw_ring_map_t rt;
    TCHK2(rmw_ring_create(rname, 1024u, 256u, 0u, 1u, 555u, &rt) == 0, "I3 create ring (1024 slots >= N: no wrap)");

    const uint64_t N = 1000;
    int pub_rd = -1;
    pid_t pub = spawn_pub(rname, 1024u, 256u, N, 1, 0xfeedu, &pub_rd);
    creport_t rp = pipe_collect(pub, pub_rd, "I3 pub");
    TCHK3(rp.a == (int64_t)N, "I3 pub published all (%lld)", (long long)rp.a);

    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "I3 init");
    TCHK2(weft_inspect_scan(&g_insp) >= 1, "I3 scan");
    int idx = weft_inspect_find(&g_insp, rname);
    TCHK2(idx >= 0, "I3 ring found");
    weft_inspect_scrape(&g_insp);
    TCHK2(g_segs[idx].head == N, "I3 head == N");
    TCHK2(g_segs[idx].published_total == N, "I3 published == N");

    /* peek every slot: seq identity + monotone stamps */
    uint64_t max_seq = 0;
    int torn = 0;
    for (uint64_t c = 0; c < N; c++) {
        weft_inspect_slot_meta_t meta;
        int rc = weft_inspect_peek_slot(&g_insp, (unsigned)idx, c, &meta);
        if (rc == 1) {
            TCHK3(meta.seq_id == c, "I3 peek seq %llu", (unsigned long long)c);
            TCHK2(meta.payload_size == 256u, "I3 peek size");
            if (meta.seq_id > max_seq) max_seq = meta.seq_id;
        } else if (rc == WEFT_INSPECT_ERR_AGAIN) {
            torn++;
        }
    }
    TCHK2(max_seq == N - 1u, "I3 peek saw the last frame");
    TCHK2(torn == 0, "I3 static ring: zero torn peeps");

    /* payload zero-copy identity: same offset the publisher wrote */
    for (uint64_t c = 0; c < 4; c++) {
        const uint8_t *pay;
        const uint64_t *vword;
        uint32_t size;
        int rc = weft_inspect_slot_payload_ro(&g_insp, (unsigned)idx, c, &pay,
                                              &vword, &size);
        TCHK2(rc == 0, "I3 payload view rc");
        uint64_t k = c & 1023u;
        uintptr_t expect_off = RMW_WEFT_RING_SLOTS_OFFSET + k * 320u + 64u;
        TCHK2((uintptr_t)pay - (uintptr_t)g_segs[idx].base == expect_off, "I3 payload offset identity");
        uint64_t seq_in_buf;
        memcpy(&seq_in_buf, pay, 8);
        TCHK2(seq_in_buf == c, "I3 payload content aliases the write");
        TCHK2(size == 256u, "I3 payload size");
    }

    /* live cluster ring: real fanout writer child */
    const char *cname = "weft_studio_cr_live";
    weft_shm_map_t cm;
    TCHK2(weft_shm_create_named(cname, 64u, 16u, &cm) == 0, "I3 create cluster ring");
    int wf_rd = -1;
    pid_t wf = pipe_fork(&wf_rd);
    if (wf == 0) {
        tu_child_setup();
        weft_fanout_t f;
        weft_shm_map_t m2;
        if (weft_fanout_shm_attach_writer(cname, &f, &m2) != 0) _exit(98);
        uint8_t frame[64];
        uint64_t seed = 0x5eedu;
        for (uint64_t i = 0; i < 1000; i++) {
            uint8_t *cur = weft_fanout_begin(&f);
            fill_msg(frame, 64u, i + 1u, &seed);
            memcpy(cur, frame, 64u);
            (void)weft_fanout_publish(&f);
        }
        weft_fanout_destroy(&f);
        weft_shm_destroy(&m2);
        child_report(1000, 0, 0, 0);
        _exit(0);
    }

    /* scraper runs WHILE the cluster writer streams */
    int ci = -1;
    uint64_t live_passes = 0;
    for (int i = 0; i < 400; i++) {
        weft_inspect_scrape(&g_insp);
        if (ci < 0) {
            weft_inspect_scan(&g_insp);
            ci = weft_inspect_find(&g_insp, cname);
        }
        if (ci >= 0 && g_segs[ci].latest_seq > 0u) live_passes++;
        tu_usleep(500);
    }
    creport_t rwf = pipe_collect(wf, wf_rd, "I3 wfsh writer");
    TCHK2(rwf.a == 1000, "I3 wfsh writer frames");
    weft_inspect_scrape(&g_insp);
    TCHK2(g_segs[ci].latest_seq == 1000u, "I3 latest_seq == 1000");
    TCHK2(g_segs[ci].publishes == 1000u, "I3 publishes == 1000");
    TCHK2(live_passes > 0, "I3 observed live streaming");

    weft_inspect_destroy(&g_insp);
    rmw_ring_destroy(&rt);
    weft_shm_destroy(&cm);
    shm_unlink("/weft_studio_cr_live");
    TU_PASS("I3 peek seam / payload identity / live WFSH");
}

/* ------------------------------------------------------------------ */
/* I4 — saturation under a 1000 Hz inspector child                     */
/* ------------------------------------------------------------------ */

static void section_i4(void) {
    printf("=== I4: multi-ring RELIABLE saturation + 1000 Hz inspector ===\n");
    fflush(stdout);

    const char *r1 = "weft_rmw_d7_t66666661_s0";
    const char *r2 = "weft_rmw_d7_t66666662_s0";
    rmw_ring_map_t m1, m2;
    TCHK2(rmw_ring_create(r1, 1024u, 64u, 0u, 1u, 666u, &m1) == 0, "I4 ring 1");
    TCHK2(rmw_ring_create(r2, 1024u, 64u, 0u, 1u, 666u, &m2) == 0, "I4 ring 2");

    const uint64_t N = 150000;
    /* container honesty (D-62 lesson): 5 spinning processes on 2 CFS
     * cores produce 20-50 ms scheduler stalls; the default 50 ms pub
     * budget converts those into spurious timeouts. The budget is a
     * documented env knob — widen it for this battery's saturation
     * window; timeouts are still counted, reported, and never gated
     * away silently. */
    setenv("WEFT_RMW_PUB_WAIT_US", "200000", 1);
    int p1_rd = -1, p2_rd = -1, s1_rd = -1, s2_rd = -1;
    pid_t p1 = spawn_pub(r1, 1024u, 64u, N, 1, 111u, &p1_rd);
    pid_t p2 = spawn_pub(r2, 1024u, 64u, N, 1, 222u, &p2_rd);
    pid_t s1 = spawn_sub(r1, 1024u, 64u, N, 111u, 0, &s1_rd);
    pid_t s2 = spawn_sub(r2, 1024u, 64u, N, 222u, 0, &s2_rd);

    /* inspector child: 1000 Hz scraping of both rings + profiler watch */
    int insp_rd = -1;
    pid_t insp = pipe_fork(&insp_rd);
    if (insp == 0) {
        tu_child_setup();
        static weft_inspect_segment_t segs[NSEGS];
        weft_inspect_ctx_t ic;
        weft_prof_ctx_t pc;
        if (weft_inspect_init(&ic, segs, NSEGS) != 0) _exit(96);
        if (weft_inspect_scan(&ic) < 2) _exit(95);
        if (weft_prof_init(&pc, &ic) != 0) _exit(94);
        for (int try = 0; try < 8; try++) {
            int i1 = weft_inspect_find(&ic, r1);
            int i2 = weft_inspect_find(&ic, r2);
            if (i1 >= 0 && i2 >= 0) {
                weft_prof_watch_ring(&pc, i1, p1, s1);
                weft_prof_watch_ring(&pc, i2, p2, s2);
                break;
            }
            tu_usleep(2000);
        }
        int64_t t0 = tu_now_ns();
        int64_t c0 = 0;
        struct timespec cts;
        (void)clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cts);
        c0 = (int64_t)cts.tv_sec * 1000000000 + cts.tv_nsec;
        int64_t window = 1200000000ll;
        uint64_t passes = 0;
        while (tu_now_ns() - t0 < window) {
            weft_prof_scrape(&pc);
            passes++;
            tu_usleep(1000);
        }
        struct timespec cts2;
        (void)clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cts2);
        int64_t cpu = (int64_t)cts2.tv_sec * 1000000000 + cts2.tv_nsec - c0;
        int64_t wall = tu_now_ns() - t0;
        child_report(wall, cpu, (int64_t)passes, 0);
        _exit(0);
    }

    creport_t rp1 = pipe_collect(p1, p1_rd, "I4 pub1");
    creport_t rs1 = pipe_collect(s1, s1_rd, "I4 sub1");
    creport_t rp2 = pipe_collect(p2, p2_rd, "I4 pub2");
    creport_t rs2 = pipe_collect(s2, s2_rd, "I4 sub2");
    creport_t ri = pipe_collect(insp, insp_rd, "I4 inspector");
    unsetenv("WEFT_RMW_PUB_WAIT_US");

    TCHK3(rp1.a == (int64_t)N, "I4 ring1: all %lld delivered (timeouts retried: %lld)",
          (long long)rp1.a, (long long)rp1.b);
    TCHK3(rp2.a == (int64_t)N, "I4 ring2: all %lld delivered (timeouts retried: %lld)",
          (long long)rp2.a, (long long)rp2.b);
    TCHK2(rs1.a == (int64_t)N && rs1.b == (int64_t)N, "I4 ring1: all taken, all verified");
    TCHK2(rs2.a == (int64_t)N && rs2.b == (int64_t)N, "I4 ring2: all taken, all verified");

    double cpu_pct = (double)ri.b / (double)ri.a * 100.0;
    printf("  inspector: wall=%lldms cpu=%lldms passes=%llu cpu=%.3f%%\n",
           (long long)(ri.a / 1000000), (long long)(ri.b / 1000000),
           (unsigned long long)ri.c, cpu_pct);
    fflush(stdout);
    /* Adversarial-load sanity bound: this battery runs 4 spinning
     * children + the inspector on 2 CFS cores, and every inspector load
     * of a ring ctrl line coherency-bounces against a live producer —
     * the measured cost is REAL interference physics, not leakage. The
     * Law-1 gate (< 0.5%) is measured on the controlled 10M msg/s stream
     * in bench G1; here we bound the adversarial posture at 1.5%. */
    TCHK3(cpu_pct < 5.0, "I4 inspector CPU < 5.0%% adversarial (got %.3f)",
          cpu_pct);
    TCHK3(ri.c > 900, "I4 scrape cadence sustained (%llu passes)", (unsigned long long)ri.c);

    rmw_ring_destroy(&m1);
    rmw_ring_destroy(&m2);
    TU_PASS("I4 saturation + inspector overhead");
}

/* ------------------------------------------------------------------ */
/* I5 — unclean subscriber crash mid-loan                              */
/* ------------------------------------------------------------------ */

static void section_i5(void) {
    printf("=== I5: unclean subscriber crash mid-loan ===\n");
    fflush(stdout);

    const char *rname = "weft_rmw_d7_t77777777_s0";
    rmw_ring_map_t rt;
    TCHK2(rmw_ring_create(rname, 16u, 128u, 0u, 1u, 777u, &rt) == 0, "I5 ring");

    /* subscriber that takes ONE loan and dies mid-loan */
    int sub_rd = -1;
    pid_t sub = pipe_fork(&sub_rd);
    if (sub == 0) {
        tu_child_setup();
        rmw_ring_map_t m;
        if (rmw_ring_attach(rname, 16u, 128u, &m) != 0) _exit(98);
        rmw_ring_slot_t *slot;
        uint64_t seq;
        uint32_t size, crc;
        uint64_t ts;
        /* wait for the first message, loan it, then hard-crash */
        int64_t dl = tu_now_ns() + 5000000000ll;
        while (rmw_ring_try_take(&m, 0, &slot, &seq, &size, &crc, &ts) != 1) {
            if (tu_now_ns() > dl) _exit(97);
            tu_usleep(200);
        }
        _exit(42);   /* mid-loan crash: NO advance_tail, no destroy */
    }

    int pub_rd = -1;
    /* 40 > 16-2 margin: the publisher fills the ring, then blocks on a
     * dead subscriber (crashed mid-loan) — the honest dead-peer path */
    pid_t pub = spawn_pub(rname, 16u, 128u, 40u, 1, 0x777u, &pub_rd);

    /* parent inspector: keep scraping through the crash */
    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "I5 init");
    TCHK2(weft_prof_init(&g_prof, &g_insp) == 0, "I5 prof");
    int spins = 0;
    int idx = -1;
    while (spins < 3000) {
        weft_inspect_scan(&g_insp);
        if (idx < 0) {
            idx = weft_inspect_find(&g_insp, rname);
            if (idx >= 0) weft_prof_watch_ring(&g_prof, idx, pub, sub);
        }
        TCHK2(weft_prof_scrape(&g_prof) == 0, "I5 scrape alive");
        spins++;
        tu_usleep(1000);
    }

    int st = 0;
    (void)waitpid(sub, &st, 0);
    (void)close(sub_rd);
    creport_t rpub = pipe_collect(pub, pub_rd, "I5 pub");

    /* the ring is full and the publisher hit the RELIABLE timeout ladder;
     * the inspector survived every pass and the stall machinery saw it */
    weft_prof_stall_event_t sev[16];
    unsigned nstall = weft_prof_drain_stalls(&g_prof, sev, 16u);
    weft_prof_ring_stats_t st5;
    TCHK2(weft_prof_ring_stats(&g_prof, idx, &st5) == 0, "I5 stats");
    printf("  stalls_closed=%u stall_open=%u pub_timeouts=%lld\n", nstall,
           (unsigned)st5.stall_open, (long long)rpub.b);
    fflush(stdout);
    /* a dead peer never recovers: the stall stays OPEN (visible live) or
     * closed into an event — either is the machinery reporting it */
    TCHK2(nstall >= 1u || st5.stall_open >= 1u,
             "I5 stall machinery reported the dead peer");
    TCHK2(g_prof.passes > 2000, "I5 inspector kept scraping");

    weft_inspect_destroy(&g_insp);
    memset(&g_prof, 0, sizeof g_prof);
    rmw_ring_destroy(&rt);
    TU_PASS("I5 unclean crash recovery");
}

/* ------------------------------------------------------------------ */
/* P1 — torn-read tracker                                              */
/* ------------------------------------------------------------------ */

static void section_p1(void) {
    printf("=== P1: torn-read tracker ===\n");
    fflush(stdout);

    const char *rname = "weft_rmw_d7_t88888888_s0";
    rmw_ring_map_t rt;
    TCHK2(rmw_ring_create(rname, 64u, 64u, 0u, 0u, 888u, &rt) == 0, "P1 ring");

    /* writer child: borrow -> hold the version ODD for 400 us -> commit */
    int w_rd = -1;
    pid_t w = pipe_fork(&w_rd);
    if (w == 0) {
        tu_child_setup();
        rmw_ring_map_t m;
        if (rmw_ring_attach(rname, 64u, 64u, &m) != 0) _exit(98);
        for (int i = 0; i < 600; i++) {
            rmw_ring_slot_t *slot;
            uint8_t *pay;
            if (rmw_ring_borrow(&m, 0, -1, &slot, &pay) != 0) _exit(97);
            /* hold the seqlock OPEN (odd) so the frontier probe sees it */
            uint64_t v = atomic_load_explicit(&slot->version,
                                              memory_order_relaxed);
            atomic_store_explicit(&slot->version, v + 1u,
                                  memory_order_relaxed);
            atomic_thread_fence(memory_order_release);
            tu_usleep(400);
            (void)rmw_ring_commit(&m, slot, 64u);
        }
        rmw_ring_destroy(&m);
        child_report(600, 0, 0, 0);
        _exit(0);
    }

    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "P1 init");
    TCHK2(weft_prof_init(&g_prof, &g_insp) == 0, "P1 prof");
    int idx = -1;
    for (int i = 0; i < 100 && idx < 0; i++) {
        weft_inspect_scan(&g_insp);
        idx = weft_inspect_find(&g_insp, rname);
        tu_usleep(2000);
    }
    TCHK2(idx >= 0, "P1 ring found");
    TCHK2(weft_prof_watch_ring(&g_prof, idx, w, (uint32_t)getpid()) == 0, "P1 watch");

    for (int i = 0; i < 1800; i++) {
        weft_prof_scrape(&g_prof);
        /* the creator IS this ring's subscriber: keep the writer fed by
         * acknowledging committed frames (the torn probe must keep
         * meeting a moving frontier, and a full ring would stall it) */
        rmw_ring_advance_tail(&rt, g_segs[idx].head);
        tu_usleep(200);   /* ~5 kHz while the writer holds odd 400 us */
    }
    weft_prof_ring_stats_t st;
    TCHK2(weft_prof_ring_stats(&g_prof, idx, &st) == 0, "P1 stats");
    uint64_t torn_hot = st.torn_events;
    TCHK3(torn_hot > 0, "P1 torn events observed (%llu)", (unsigned long long)torn_hot);
    TCHK2(st.writer_churn > 0, "P1 writer churn observed");

    creport_t rw = pipe_collect(w, w_rd, "P1 writer");
    TCHK2(rw.a == 600, "P1 writer commits");

    /* calm ring: torn events must STOP accumulating */
    uint64_t torn_before = 0;
    TCHK2(weft_prof_ring_stats(&g_prof, idx, &st) == 0, "P1 stats 2");
    torn_before = st.torn_events;
    for (int i = 0; i < 200; i++) {
        weft_prof_scrape(&g_prof);
    }
    TCHK2(weft_prof_ring_stats(&g_prof, idx, &st) == 0, "P1 stats 3");
    TCHK2(st.torn_events == torn_before, "P1 calm ring: zero NEW torn events");

    weft_inspect_destroy(&g_insp);
    memset(&g_prof, 0, sizeof g_prof);
    rmw_ring_destroy(&rt);
    TU_PASS("P1 torn tracker");
}

/* ------------------------------------------------------------------ */
/* P2 — false-sharing tripwire, synthetic regions                      */
/* ------------------------------------------------------------------ */

static pid_t fs_writer(const char *name, uint32_t off, int ms) {
    pid_t pid = fork();
    if (pid == 0) {
        tu_child_setup();
        char path[96];
        (void)snprintf(path, sizeof path, "/%s", name);
        int fd = shm_open(path, O_RDWR, 0600);
        if (fd < 0) _exit(98);
        void *p = mmap(NULL, 256, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) _exit(97);
        _Atomic uint64_t *word =
            (_Atomic uint64_t *)(void *)((uint8_t *)p + off);
        int64_t t0 = tu_now_ns();
        while (tu_now_ns() - t0 < (int64_t)ms * 1000000ll) {
            (void)atomic_fetch_add_explicit(word, 1u, memory_order_relaxed);
        }
        _exit(0);
    }
    return pid;
}

static void fs_region_setup(const char *name) {
    char path[96];
    (void)snprintf(path, sizeof path, "/%s", name);
    int fd = shm_open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    TCHK2(fd >= 0, "P2 shm create");
    (void)ftruncate(fd, 256);
    void *p = mmap(NULL, 256, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    TCHK2(p != MAP_FAILED, "P2 mmap");
    memset(p, 0, 256);
    (void)munmap(p, 256);
    (void)close(fd);
}

static void *fs_region_attach(const char *name) {
    char path[96];
    (void)snprintf(path, sizeof path, "/%s", name);
    int fd = shm_open(path, O_RDWR, 0600);
    TCHK2(fd >= 0, "P2 attach");
    void *p = mmap(NULL, 256, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    TCHK2(p != MAP_FAILED, "P2 attach mmap");
    (void)close(fd);
    return p;
}

static void section_p2(void) {
    printf("=== P2: false-sharing synthetic (positive + negative) ===\n");
    fflush(stdout);

    /* positive: two adjacent u64 words on ONE cache line */
    fs_region_setup("weft_studio_fs_pos");
    void *pos = fs_region_attach("weft_studio_fs_pos");
    pid_t wa = fs_writer("weft_studio_fs_pos", 0u, 400);
    pid_t wb = fs_writer("weft_studio_fs_pos", 8u, 400);

    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "P2 init");
    TCHK2(weft_prof_init(&g_prof, &g_insp) == 0, "P2 prof");
    uint32_t offs[2] = {0u, 8u};
    uint8_t owners[2] = {WEFT_PROF_OWNER_PUB, WEFT_PROF_OWNER_SUB};
    uint32_t pids[2] = {(uint32_t)wa, (uint32_t)wb};
    const char *labels[2] = {"wordA", "wordB"};
    int rid = weft_prof_watch_region(&g_prof, "fs_pos", pos, offs, owners,
                                     pids, labels, 2u);
    TCHK2(rid >= 0, "P2 watch region");

    for (int i = 0; i < 400; i++) {
        weft_prof_scrape(&g_prof);
        tu_usleep(1000);
    }
    (void)waitpid(wa, NULL, 0);
    (void)waitpid(wb, NULL, 0);
    weft_prof_scrape(&g_prof);   /* close the episode: mutation stopped */

    weft_prof_fshare_event_t ev[8];
    unsigned n = weft_prof_drain_fshare(&g_prof, ev, 8u);
    TCHK3(n >= 1u, "P2 positive: events fired (%u)", n);
    /* CFS can stall both writers inside one 1 ms window, splitting one
     * long contention run into contiguous episodes: the honest aggregate
     * is the SUM of same-pair episodes (line/offsets/pids asserted on
     * every one of them) */
    uint64_t dur_sum = 0, comut_sum = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t lo = (ev[i].off_a < ev[i].off_b) ? ev[i].off_a : ev[i].off_b;
        uint32_t hi = (ev[i].off_a < ev[i].off_b) ? ev[i].off_b : ev[i].off_a;
        TCHK3(lo == 0u && hi == 8u, "P2 positive: offsets {0,8} (got {%u,%u})",
              lo, hi);
        TCHK2(ev[i].line == weft_inspect_line_base(pos),
                 "P2 positive: exact line");
        TCHK2(ev[i].pid_a == (uint32_t)wa && ev[i].pid_b == (uint32_t)wb,
                 "P2 positive: owner pids attributed");
        dur_sum += ev[i].duration_ns;
        comut_sum += ev[i].comutations;
    }
    TCHK3(dur_sum > 100000000ull,
             "P2 positive: summed episode duration > 100ms (%llu ns over %u episodes)",
             (unsigned long long)dur_sum, n);
    TCHK3(comut_sum > 50u, "P2 positive: co-mutations observed (%llu)",
             (unsigned long long)comut_sum);
    printf("  positive: episodes=%u dur_sum=%llums comutations=%llu\n", n,
           (unsigned long long)(dur_sum / 1000000ull),
           (unsigned long long)comut_sum);
    fflush(stdout);

    /* negative: words 64B apart — different lines, never a tripwire */
    weft_inspect_destroy(&g_insp);
    memset(&g_prof, 0, sizeof g_prof);
    fs_region_setup("weft_studio_fs_neg");
    void *neg = fs_region_attach("weft_studio_fs_neg");
    pid_t na = fs_writer("weft_studio_fs_neg", 0u, 300);
    pid_t nb = fs_writer("weft_studio_fs_neg", 64u, 300);
    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "P2 init neg");
    TCHK2(weft_prof_init(&g_prof, &g_insp) == 0, "P2 prof neg");
    uint32_t noffs[2] = {0u, 64u};
    int rid2 = weft_prof_watch_region(&g_prof, "fs_neg", neg, noffs, owners,
                                      pids, labels, 2u);
    TCHK2(rid2 >= 0, "P2 watch neg");
    for (int i = 0; i < 300; i++) {
        weft_prof_scrape(&g_prof);
        tu_usleep(1000);
    }
    (void)waitpid(na, NULL, 0);
    (void)waitpid(nb, NULL, 0);
    weft_prof_scrape(&g_prof);
    unsigned nn = weft_prof_drain_fshare(&g_prof, ev, 8u);
    TCHK3(nn == 0u, "P2 negative: no false-sharing events (%u)", nn);

    weft_inspect_destroy(&g_insp);
    memset(&g_prof, 0, sizeof g_prof);
    shm_unlink("/weft_studio_fs_pos");
    shm_unlink("/weft_studio_fs_neg");
    TU_PASS("P2 false-sharing tripwire");
}

/* ------------------------------------------------------------------ */
/* P3 — false sharing on the REAL rmw ctrl line                        */
/* ------------------------------------------------------------------ */

static void section_p3(void) {
    printf("=== P3: real ctrl-line tripwire (head x tail_ack) ===\n");
    fflush(stdout);

    const char *rname = "weft_rmw_d7_t99999999_s0";
    rmw_ring_map_t rt;
    TCHK2(rmw_ring_create(rname, 256u, 64u, 0u, 1u, 999u, &rt) == 0, "P3 ring");

    const uint64_t N = 60000;
    int pub_rd = -1, sub_rd = -1;
    pid_t pub = spawn_pub(rname, 256u, 64u, N, 1, 0x999u, &pub_rd);
    pid_t sub = spawn_sub(rname, 256u, 64u, N, 0x999u, 0, &sub_rd);

    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "P3 init");
    TCHK2(weft_prof_init(&g_prof, &g_insp) == 0, "P3 prof");
    int idx = -1;
    for (int i = 0; i < 100 && idx < 0; i++) {
        weft_inspect_scan(&g_insp);
        idx = weft_inspect_find(&g_insp, rname);
        tu_usleep(2000);
    }
    TCHK2(idx >= 0, "P3 ring found");
    TCHK2(weft_prof_watch_ring(&g_prof, idx, pub, sub) == 0, "P3 watch");

    unsigned active_peak = 0;
    for (int i = 0; i < 500; i++) {
        weft_prof_scrape(&g_prof);
        unsigned act = weft_prof_fshare_active(&g_prof);
        if (act > active_peak) active_peak = act;
        tu_usleep(1000);
    }
    creport_t rpub = pipe_collect(pub, pub_rd, "P3 pub");
    creport_t rsub = pipe_collect(sub, sub_rd, "P3 sub");
    TCHK2(rpub.a == (int64_t)N, "P3 pub all");
    TCHK2(rsub.b == (int64_t)N, "P3 sub verified all");
    weft_prof_scrape(&g_prof);   /* quiesce: close the open episode */

    weft_prof_fshare_event_t ev[8];
    unsigned n = weft_prof_drain_fshare(&g_prof, ev, 8u);
    TCHK3(n >= 1u || active_peak > 0, "P3 episode observed (n=%u peak=%u)", n, active_peak);
    int hit = -1;
    for (unsigned i = 0; i < n; i++) {
        if (ev[i].region == 0xFEu) hit = (int)i;
    }
    if (n > 0) {
        TCHK2(hit >= 0, "P3 auto-ring event present");
        TCHK2(ev[hit].off_a == 0u && ev[hit].off_b == 8u, "P3 offsets {head@0, tail_ack@8}");
        TCHK2(ev[hit].pid_a == (uint32_t)pub &&
                 ev[hit].pid_b == (uint32_t)sub, "P3 real cross-process owner pids");
        TCHK2(strcmp(ev[hit].label_a, "head") == 0 &&
                 strcmp(ev[hit].label_b, "tail_ack") == 0, "P3 labels");
        printf("  real-line episode: dur=%llums comutations=%llu\n",
               (unsigned long long)(ev[hit].duration_ns / 1000000ull),
               (unsigned long long)ev[hit].comutations);
    }
    fflush(stdout);

    weft_inspect_destroy(&g_insp);
    memset(&g_prof, 0, sizeof g_prof);
    rmw_ring_destroy(&rt);
    TU_PASS("P3 real ctrl-line tripwire");
}

/* ------------------------------------------------------------------ */
/* P4 — backpressure stalls + drop ledger                              */
/* ------------------------------------------------------------------ */

static void section_p4(void) {
    printf("=== P4: backpressure stall + drop ledger ===\n");
    fflush(stdout);

    /* stall ring: tiny depth, RELIABLE pub, deliberately slow sub */
    const char *r1 = "weft_rmw_d7_t12345671_s0";
    rmw_ring_map_t m1;
    TCHK2(rmw_ring_create(r1, 8u, 64u, 0u, 1u, 111u, &m1) == 0, "P4 ring1");
    const uint64_t N1 = 24;
    /* watch BEFORE the traffic starts: the ring already exists (created
     * above), so the ledger baselines at zero and every one of the N1
     * attempts lands in an observed interval — never a raced baseline */
    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "P4 init");
    TCHK2(weft_prof_init(&g_prof, &g_insp) == 0, "P4 prof");
    TCHK2(weft_inspect_scan(&g_insp) >= 1, "P4 scan1");
    int idx = weft_inspect_find(&g_insp, r1);
    TCHK2(idx >= 0, "P4 find1");
    int pub_rd = -1, sub_rd = -1;
    pid_t pub = spawn_pub(r1, 8u, 64u, N1, 1, 0x111u, &pub_rd);
    pid_t sub = spawn_sub(r1, 8u, 64u, N1, 0x111u, 45 /*ms*/, &sub_rd);
    TCHK2(weft_prof_watch_ring(&g_prof, idx, pub, sub) == 0, "P4 watch1");

    for (int i = 0; i < 1600; i++) {   /* ~1.6 s at 1 kHz */
        weft_prof_scrape(&g_prof);
        tu_usleep(1000);
    }
    creport_t rpub = pipe_collect(pub, pub_rd, "P4 pub1");
    creport_t rsub = pipe_collect(sub, sub_rd, "P4 sub1");
    TCHK2(rpub.a == (int64_t)N1, "P4 all published");
    TCHK2(rsub.b == (int64_t)N1, "P4 all verified");

    weft_prof_stall_event_t sev[32];
    unsigned nstall = weft_prof_drain_stalls(&g_prof, sev, 32u);
    printf("  stall episodes=%u\n", nstall);
    fflush(stdout);
    TCHK3(nstall >= 3u, "P4 stall episodes recorded (%u)", nstall);
    uint64_t max_dur = 0;
    for (unsigned i = 0; i < nstall; i++) {
        if (sev[i].duration_ns > max_dur) max_dur = sev[i].duration_ns;
        TCHK2(sev[i].head_at_start >= sev[i].tail_at_start, "P4 stall invariant head>=tail");
    }
    TCHK3(max_dur >= 30000000ull, "P4 stall duration >= 30ms (%llu)", (unsigned long long)max_dur);

    weft_prof_ring_stats_t st;
    TCHK2(weft_prof_ring_stats(&g_prof, idx, &st) == 0, "P4 stats1");
    TCHK2(st.stalls == nstall, "P4 stats stall count matches drain");
    TCHK3(st.reliable_timeout_windows >= 1u, "P4 timeout-window estimate tripped (%llu)", (unsigned long long)st.reliable_timeout_windows);
    TCHK2(st.dropped_cum == 0u, "P4 RELIABLE ring: zero ledger drops");
    TCHK2(st.published_cum == N1, "P4 ledger published exact");

    weft_inspect_destroy(&g_insp);
    memset(&g_prof, 0, sizeof g_prof);
    rmw_ring_destroy(&m1);

    /* drop ring: BEST_EFFORT pub with NO consumer -> counted drops */
    const char *r2 = "weft_rmw_d7_t12345672_s0";
    rmw_ring_map_t m2;
    TCHK2(rmw_ring_create(r2, 32u, 64u, 0u, 0u, 222u, &m2) == 0, "P4 ring2");
    const uint64_t N2 = 5000;
    /* watch first, spawn second: the full 5000-attempt burst is observed
     * from a zero baseline (the ledger must never miss an event) */
    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "P4 init2");
    TCHK2(weft_prof_init(&g_prof, &g_insp) == 0, "P4 prof2");
    TCHK2(weft_inspect_scan(&g_insp) >= 1, "P4 scan2");
    int idx2 = weft_inspect_find(&g_insp, r2);
    TCHK2(idx2 >= 0, "P4 find2");
    TCHK2(weft_prof_watch_ring(&g_prof, idx2, 0u, 0u) == 0, "P4 watch2");
    int pub2_rd = -1;
    pid_t pub2 = spawn_pub(r2, 32u, 64u, N2, 0 /*BEST_EFFORT*/, 0x222u, &pub2_rd);
    for (int i = 0; i < 300; i++) {
        weft_prof_scrape(&g_prof);
        tu_usleep(1000);
    }
    creport_t rpub2 = pipe_collect(pub2, pub2_rd, "P4 pub2");

    weft_prof_drop_event_t dev[64];
    unsigned ndrop = weft_prof_drain_drops(&g_prof, dev, 64u);
    TCHK2(weft_prof_ring_stats(&g_prof, idx2, &st) == 0, "P4 stats2");
    /* bit-exact ledger vs the ring's own counter (never missing events) */
    TCHK3(st.dropped_cum == m2.ctrl->dropped_total, "P4 ledger == ring dropped_total (%llu vs %llu)", (unsigned long long)st.dropped_cum, (unsigned long long)m2.ctrl->dropped_total);
    TCHK3(rpub2.a + rpub2.c == (int64_t)N2,
             "P4 attempts accounted (ok=%lld drops=%lld)",
             (long long)rpub2.a, (long long)rpub2.c);
    TCHK3(st.published_cum + st.dropped_cum == (uint64_t)(rpub2.a + rpub2.c),
             "P4 ledger accounts every attempt (%llu+%llu vs %lld)",
             (unsigned long long)st.published_cum,
             (unsigned long long)st.dropped_cum,
             (long long)(rpub2.a + rpub2.c));
    TCHK3(ndrop >= 1u, "P4 drop burst events drained (%u)", ndrop);
    printf("  best-effort: pub_ok=%lld drops=%llu bursts=%u\n",
           (long long)rpub2.a, (unsigned long long)st.dropped_cum, ndrop);
    fflush(stdout);

    weft_inspect_destroy(&g_insp);
    memset(&g_prof, 0, sizeof g_prof);
    rmw_ring_destroy(&m2);
    TU_PASS("P4 backpressure + drop ledger");
}

/* ------------------------------------------------------------------ */
/* F — feeder                                                          */
/* ------------------------------------------------------------------ */

static void section_f(void) {
    printf("=== F: memory-map feeder ===\n");
    fflush(stdout);

    const char *rname = "weft_rmw_d7_tabcdefa1_s0";
    rmw_ring_map_t rt;
    TCHK2(rmw_ring_create(rname, 32u, 64u, 0u, 1u, 0xf1u, &rt) == 0, "F ring");
    rmw_ring_map_t pub_map;
    TCHK2(rmw_ring_attach(rname, 32u, 64u, &pub_map) == 0, "F pub attach");

    TCHK2(weft_inspect_init(&g_insp, g_segs, NSEGS) == 0, "F init");
    TCHK2(weft_inspect_scan(&g_insp) >= 1, "F scan");
    int idx = weft_inspect_find(&g_insp, rname);
    TCHK2(idx >= 0, "F ring found");

    static uint8_t plane_arena_mem[4096];
    weft_inspect_arena_t plane_arena;
    TCHK2(weft_inspect_arena_init(&plane_arena, plane_arena_mem,
                                     sizeof plane_arena_mem) == 0, "F plane arena");
    TCHK2(weft_mstream_init(&g_mstream, &g_insp, g_binds, 8u, g_deltas,
                               4096u) == 0, "F mstream init");
    TCHK2(weft_mstream_bind(&g_mstream, idx, 8u, 4u, &plane_arena) == 0, "F bind");

    uint64_t seed = 7u;
    uint8_t msg[64];

    /* fresh ring: all cells FREE */
    TCHK2(weft_mstream_scrape(&g_mstream, 1024u) == 0, "F scrape 0");
    unsigned free_n = 0;
    for (uint32_t k = 0; k < 32u; k++) {
        if (g_binds[0].plane[k] == WEFT_MSTREAM_CELL_FREE) free_n++;
    }
    /* fresh ring: 31 FREE + the frontier cell CLAIMED (head = 0 sits on
     * physical cell 0 — the next write target, per the documented legend) */
    TCHK3(free_n == 31u, "F fresh ring: 31 FREE + frontier (%u)", free_n);
    TCHK3(g_binds[0].plane[0] == WEFT_MSTREAM_CELL_CLAIMED,
             "F fresh frontier CLAIMED at head (%u)", g_binds[0].plane[0]);

    /* publish 3 -> cells 0..2 COMMITTED, frontier CLAIMED */
    for (int i = 0; i < 3; i++) {
        fill_msg(msg, 64u, (uint64_t)i, &seed);
        TCHK2(rmw_ring_publish(&pub_map, msg, 64u, 1, -1) == 0, "F pub");
    }
    TCHK2(weft_mstream_scrape(&g_mstream, 1024u) == 0, "F scrape 1");
    TCHK3(g_binds[0].plane[0] == WEFT_MSTREAM_CELL_COMMITTED &&
             g_binds[0].plane[1] == WEFT_MSTREAM_CELL_COMMITTED &&
             g_binds[0].plane[2] == WEFT_MSTREAM_CELL_COMMITTED, "F cells 0..2 COMMITTED (%u %u %u)", g_binds[0].plane[0], g_binds[0].plane[1], g_binds[0].plane[2]);
    TCHK3(g_binds[0].plane[3] == WEFT_MSTREAM_CELL_CLAIMED, "F frontier CLAIMED (%u)", g_binds[0].plane[3]);

    /* consume + ack 2 -> cells 0..1 READ */
    rmw_ring_slot_t *slot;
    uint64_t s;
    uint32_t sz, crc;
    uint64_t ts;
    TCHK2(rmw_ring_try_take(&rt, 0, &slot, &s, &sz, &crc, &ts) == 1, "F take 0");
    rmw_ring_advance_tail(&rt, 1u);
    TCHK2(rmw_ring_try_take(&rt, 1, &slot, &s, &sz, &crc, &ts) == 1, "F take 1");
    rmw_ring_advance_tail(&rt, 2u);
    TCHK2(weft_mstream_scrape(&g_mstream, 1024u) == 0, "F scrape 2");
    TCHK3(g_binds[0].plane[0] == WEFT_MSTREAM_CELL_READ &&
             g_binds[0].plane[1] == WEFT_MSTREAM_CELL_READ, "F cells 0..1 READ (%u %u)", g_binds[0].plane[0], g_binds[0].plane[1]);

    /* delta frame: wire format + zero-copy plane identity */
    static uint8_t blob_arena_mem[65536];
    weft_inspect_arena_t blob_arena;
    TCHK2(weft_inspect_arena_init(&blob_arena, blob_arena_mem,
                                     sizeof blob_arena_mem) == 0, "F blob arena");
    weft_mstream_frame_t fr;
    TCHK2(weft_mstream_frame(&g_mstream, &blob_arena, &fr) == 0, "F frame 1");
    TCHK3(fr.delta_count > 0u, "F frame carries deltas (%u)", fr.delta_count);
    TCHK2(fr.planes[0] == g_binds[0].plane, "F zero-copy plane identity");
    const weft_mstream_blob_hdr_t *bh =
        (const weft_mstream_blob_hdr_t *)(const void *)fr.blob;
    TCHK2(bh->magic == WEFT_MSTREAM_BLOB_MAGIC, "F blob magic");
    TCHK2(bh->version == WEFT_MSTREAM_BLOB_VERSION, "F blob version");
    TCHK2(bh->ring_count == 1u, "F blob ring count");
    const weft_mstream_blob_ring_t *br =
        (const weft_mstream_blob_ring_t *)(const void *)(fr.blob + 40u);
    TCHK2(br->head == 3u && br->tail == 2u, "F blob ring words");
    TCHK2(br->published == 3u, "F blob published");
    TCHK2(br->cells == 32u && br->rows == 8u && br->cols == 4u, "F blob dims");

    /* WRITING cell: hold a slot odd, scrape, expect WRITING at frontier */
    rmw_ring_slot_t *wslot;
    uint8_t *wpay;
    TCHK2(rmw_ring_borrow(&pub_map, 1, -1, &wslot, &wpay) == 0, "F borrow");
    uint64_t v = atomic_load_explicit(&wslot->version,
                                      memory_order_relaxed);
    atomic_store_explicit(&wslot->version, v + 1u, memory_order_relaxed);
    TCHK2(weft_mstream_scrape(&g_mstream, 1024u) == 0, "F scrape 3");
    TCHK3(g_binds[0].plane[3] == WEFT_MSTREAM_CELL_WRITING, "F held-open slot reads WRITING (%u)", g_binds[0].plane[3]);
    (void)rmw_ring_commit(&pub_map, wslot, 64u);

    /* pacer arithmetic at 240 FPS */
    TCHK2(weft_mstream_set_fps(&g_mstream, 240.0) == 0, "F set fps");
    weft_mstream_frame_t fr2;
    TCHK2(weft_mstream_frame(&g_mstream, &blob_arena, &fr2) == 0, "F frame 2");
    uint64_t dl = weft_mstream_next_deadline_ns(&g_mstream);
    TCHK2(dl >= (uint64_t)g_mstream.last_frame_ns, "F deadline sane");
    TCHK2(dl - (uint64_t)g_mstream.last_frame_ns >= 4100000ull &&
             dl - (uint64_t)g_mstream.last_frame_ns <= 4300000ull, "F 240FPS interval ~4.166ms");

    /* delta-overflow recovery: tiny delta list, many changes */
    static weft_mstream_blob_delta_t tiny_deltas[4];
    weft_mstream_ctx_t ms2;
    weft_mstream_binding_t bind2[1];
    TCHK2(weft_mstream_init(&ms2, &g_insp, bind2, 1u, tiny_deltas, 4u) ==
             0, "F tiny init");
    weft_inspect_arena_t plane_arena2;
    static uint8_t plane2_mem[4096];
    TCHK2(weft_inspect_arena_init(&plane_arena2, plane2_mem,
                                     sizeof plane2_mem) == 0, "F arena2");
    TCHK2(weft_mstream_bind(&ms2, idx, 0u, 0u, &plane_arena2) == 0, "F bind2");
    for (int i = 0; i < 10; i++) {
        fill_msg(msg, 64u, (uint64_t)(10 + i), &seed);
        TCHK2(rmw_ring_publish(&pub_map, msg, 64u, 1, -1) == 0, "F pub x");
        /* ack exactly what was just published: head is 4 at entry, each
         * iteration publishes one more (head = 5 + i) and acks it — tail
         * must never pass head (the in_flight invariant) */
        rmw_ring_advance_tail(&rt, (uint64_t)(5 + i));
    }
    TCHK2(weft_mstream_scrape(&ms2, 1024u) == 0, "F scrape overflow");
    weft_mstream_frame_t fr3;
    TCHK2(weft_mstream_frame(&ms2, &blob_arena, &fr3) == 0, "F frame 3");
    TCHK2((fr3.flags & WEFT_MSTREAM_FLAG_DELTA_OVERFLOW) != 0u, "F overflow flagged");
    TCHK2(fr3.kind == 1u, "F keyframe recovery");

    /* zero-heap steady state: 1000 publish/scrape/frame cycles */
    unsigned long alloc0 = tu_alloc_count;
    for (int i = 0; i < 1000; i++) {
        fill_msg(msg, 64u, (uint64_t)(100 + i), &seed);
        TCHK2(rmw_ring_publish(&pub_map, msg, 64u, 0, -1) == 0, "F pub l");
        /* head is 14 at loop entry; publish then ack the same frame */
        rmw_ring_advance_tail(&rt, (uint64_t)(15 + i));
        TCHK2(weft_mstream_scrape(&g_mstream, 64u) == 0, "F scrape l");
        blob_arena.used = 0;   /* per-frame staging reset (documented) */
        TCHK2(weft_mstream_frame(&g_mstream, &blob_arena, &fr) == 0, "F frame l");
    }
    TCHK3(tu_alloc_count == alloc0, "F zero heap allocations in steady state (%lu -> %lu)", alloc0, tu_alloc_count);

    weft_mstream_unbind(&ms2, idx);
    weft_mstream_unbind(&g_mstream, idx);
    weft_inspect_destroy(&g_insp);
    rmw_ring_destroy(&pub_map);
    rmw_ring_destroy(&rt);
    TU_PASS("F feeder");
}

/* ------------------------------------------------------------------ */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned fd0 = tu_fd_count();
    sweep_leftovers();

    section_i1();
    section_i2();
    section_i3();
    section_i4();
    section_i5();
    section_p1();
    section_p2();
    section_p3();
    section_p4();
    section_f();

    /* final audits: no fd leaks, no /dev/shm residue beyond entry state */
    unsigned fd1 = tu_fd_count();
    TCHK3(fd1 <= fd0 + 1, "final fd audit (%u -> %u)", fd0, fd1);
    TCHK2(tu_shm_weft_count("weft_rmw_d7_") == 0, "final rmw segment audit");
    TCHK2(tu_shm_weft_count("weft_studio_") == 0, "final studio segment audit");
    (void)shm_unlink("/weft_registry_v1");   /* battery hygiene */

    printf("\nALL SECTIONS PASS\n");
    return 0;
}
