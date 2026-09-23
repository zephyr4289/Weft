// weft_ipc_torture.c — IPC mesh torture battery (RFC-0016).
//
// Modes (argv[1]): memfd | mesh | latency | all (default)
//
//   memfd   The sealed-descriptor road at volume: one producer process
//           publishing 100,000 frames (256B x 8 slots) through a sealed
//           memfd; 4 forked consumer processes that discovered nothing,
//           handshake over socketpairs, and claim with weft_shm_park
//           cadence. One consumer is SIGKILLed mid-claim at ~30k frames:
//           the ring must stay HEALTHY (Axis 3), the survivors must
//           finish with exact telescoping, and the registry's advisory
//           consumer count must show the crash leak HONESTLY (4-3=1).
//
//   mesh    The full control plane under a crashy restart: producer P1
//           registers in the mesh, serves named-socket handshakes,
//           publishes 60,000 frames with PER-FRAME heartbeats (the
//           continuity cadence), then SIGKILLs itself. A successor P2
//           takes the name over (epoch 2), seeds its numbering from the
//           registry mirror, and publishes 60,000 more. Consumers detect
//           the epoch change via discovery, re-handshake, resync their
//           readers with their OWN last seq, and continue. NORMATIVE:
//           fresh + drops == 120,000 EXACTLY per consumer — telescoping
//           spans the incarnation gap with zero phantom frames.
//
//   latency The wake-up primitive, measured cross-process: publish ->
//           park-observe latency (zero syscalls, bounded spin) vs an
//           eventfd write -> blocking-read baseline (syscall + wakeup).
//           10,000 rounds each, p50/p99/max reported. Informational
//           numbers (2 vCPU sandbox); the COMPARISON is the claim:
//           doorbell-free park observes publishes in the ~100ns class
//           while eventfd wakeups cost microseconds — with the honest
//           trade-off that park burns bounded CPU while waiting.
//
// Style: invariants are normative (hard exit code); throughput is
// informational and labeled with the sandbox shape. ASAN-clean by
// construction (all children waited on).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fanout.h"
#include "shm_ring.h"
#include "weft.h"
#include "weft_ipc.h"
#include "weft_shm.h"

static int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", name); } \
    else { printf("  [FAIL] %s\n", name); g_fail++; } \
} while (0)

static void nap_ms(long ms) {
    // tv_nsec must stay < 1e9 — split into seconds + nanoseconds (a plain
    // ms*1e6 overflows the field for naps >= 1s and EINVALs WITHOUT
    // sleeping; this bug actually cost a debugging round — see the
    // torture's successor-timing comments).
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static uint64_t mono_ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void fill_pattern(uint8_t* p, uint64_t seq, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) {
        p[i] = weft_pat((uint32_t)seq, (uint32_t)i);
    }
}

/// Shared per-consumer results (MAP_SHARED|MAP_ANONYMOUS, fork-inherited).
typedef struct {
    uint64_t fresh;
    uint64_t drops;      // sum of rec->dropped across fresh claims
    uint64_t skipped;
    uint64_t exhausted;
    uint64_t bitfails;
    uint64_t last_seq;
    uint64_t epoch_seen;
    uint64_t pad;
} tort_result_t;

static int cmp_u64(const void* a, const void* b) {
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void report_pct(const char* label, uint64_t* v, size_t n) {
    qsort(v, n, sizeof *v, cmp_u64);
    const uint64_t p50 = v[n / 2];
    const uint64_t p99 = v[(n * 99) / 100];
    printf("    %-22s p50=%6llu ns  p99=%6llu ns  max=%6llu ns\n",
           label, (unsigned long long)p50, (unsigned long long)p99,
           (unsigned long long)v[n - 1]);
}

// ---------------------------------------------------------------------------
// memfd mode
// ---------------------------------------------------------------------------

static int torture_memfd(void) {
    printf("=== ipc-torture memfd: 100,000 cross-process publications "
           "===\n");
    enum { KIDS = 4, FRAMES = 100000, VICTIM_AT = 30000, PB = 256, SLOTS = 8 };
    unlink("/dev/shm/weft_registry_v1");

    weft_ipc_registry_t reg;
    if (weft_ipc_registry_open(&reg, 1, 0) != 0) {
        printf("  [FAIL] registry open\n");
        return 1;
    }

    weft_shm_memfd_t m;
    if (weft_shm_memfd_create(PB, SLOTS, &m) != 0) {
        printf("  [FAIL] memfd create\n");
        return 1;
    }
    if (weft_shm_memfd_ro_view(&m) != 0) {
        printf("  [FAIL] RO view\n");
        return 1;
    }

    // Publish a READ token + registry entry (the mesh integration leg —
    // consumers here use socketpairs, but the session is discoverable).
    uint8_t key[WEFT_IPC_KEY_BYTES];
    weft_ipc_random_bytes(key, sizeof key);
    weft_ipc_session_t sess;
    if (weft_ipc_register(&reg, "torture-memfd", WEFT_IPC_TRANSPORT_MEMFD, PB,
                          SLOTS, 0, &sess) != 0) {
        printf("  [FAIL] register\n");
        return 1;
    }
    weft_ipc_claims_t cl = { .session_id = sess.session_id,
                             .perms = WEFT_IPC_PERM_READ | WEFT_IPC_PERM_CLAIM,
                             .epoch = sess.epoch, .expiry_unix = 0,
                             .key_id = 1, .nonce = 42 };
    uint8_t tok[WEFT_IPC_TOKEN_BYTES];
    weft_ipc_token_issue(&cl, key, tok);
    uint8_t tag[32];
    memcpy(tag, tok + 32, 32);
    weft_ipc_session_publish_token(&sess, 1, tag);

    tort_result_t* results = mmap(NULL, sizeof(tort_result_t) * KIDS,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (results == MAP_FAILED) return 1;
    memset(results, 0, sizeof(tort_result_t) * KIDS);

    int sv[KIDS][2];
    for (int k = 0; k < KIDS; k++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv[k]) != 0) return 1;
    }

    pid_t kids[KIDS];
    for (int k = 0; k < KIDS; k++) {
        kids[k] = fork();
        if (kids[k] == 0) {
            // ---- consumer process k ----
            // Close every inherited socketpair EXCEPT our own (j == k:
            // sv[k][0] belongs to the parent; sv[k][1] is ours).
            for (int j = 0; j < KIDS; j++) {
                if (j == k) continue;
                close(sv[j][0]);
                close(sv[j][1]);
            }
            // Close the parent's end of OUR pair; ours stays open.
            close(sv[k][0]);
            weft_ipc_registry_t creg;
            if (weft_ipc_registry_open(&creg, 1, 0) != 0) _exit(80);
            weft_shm_memfd_t cm;
            weft_shm_grant_info_t g;
            if (weft_shm_handshake_connect_fd(sv[k][1], 0, 0, NULL, &cm,
                                              &g) != 0) {
                _exit(81);
            }
            if (weft_ipc_consumer_attach(&creg, g.session_id ? g.session_id
                                                             : 1) != 0) {
                // session id unknown to the grant here — attach by discovery
                weft_ipc_discovered_t d[2];
                if (weft_ipc_discover(&creg, 1000, "torture-memfd", d, 2) == 1 &&
                    weft_ipc_consumer_attach(&creg, d[0].session_id) != 0) {
                    _exit(82);
                }
            }
            weft_fanout_reader_t r;
            memset(&r, 0, sizeof r);
            if (weft_fanout_reader_init(&r, cm.map.ring,
                                        weft_shm_ring_bytes(&cm.map), PB,
                                        SLOTS) != 0) {
                _exit(83);
            }
            uint64_t target = 0;
            const int victim = (k == KIDS - 1);
            while (target < FRAMES) {
                const weft_fanout_claim_t* c = weft_fanout_claim(&r);
                if (c->fresh) {
                    results[k].fresh++;
                    results[k].drops += c->dropped;
                    target = c->seq;
                    results[k].last_seq = c->seq;
                    const uint8_t* v = (const uint8_t*)weft_fanout_view(&r);
                    uint8_t e[PB];
                    fill_pattern(e, c->seq, PB);
                    if (memcmp(v, e, PB) != 0) results[k].bitfails++;
                    if (victim && target >= VICTIM_AT) {
                        // Crash mid-claim: no leave, no cleanup.
                        kill(getpid(), SIGKILL);
                    }
                } else {
                    results[k].skipped++;
                    uint64_t last = results[k].last_seq;
                    (void)weft_shm_park(&cm.map, &last, 2000);
                }
            }
            weft_fanout_stats_t st;
            weft_fanout_reader_stats(&r, &st);
            results[k].exhausted = st.torn_exhausted;
            weft_fanout_reader_destroy(&r);
            weft_shm_memfd_destroy(&cm);
            // clean detach (the crash victim never reaches here)
            weft_ipc_discovered_t d[2];
            if (weft_ipc_discover(&creg, 1000, "torture-memfd", d, 2) == 1) {
                weft_ipc_consumer_leave(&creg, d[0].session_id);
            }
            weft_ipc_registry_close(&creg);
            close(sv[k][1]);
            _exit(0);
        }
        close(sv[k][1]);
    }

    // ---- producer: serve KIDS handshakes, then publish FRAMES ----
    weft_fanout_t f;
    memset(&f, 0, sizeof f);
    if (weft_fanout_attach_writer(&f, m.map.ring, weft_shm_ring_bytes(&m.map),
                                  PB, SLOTS) != 0) {
        return 1;
    }
    for (int k = 0; k < KIDS; k++) {
        if (weft_shm_handshake_serve(sv[k][0], &m, sess.session_id, sess.epoch,
                                     NULL, NULL) != 0) {
            printf("  [FAIL] handshake %d\n", k);
            return 1;
        }
        close(sv[k][0]);
    }

    const uint64_t t0 = mono_ns_now();
    for (uint32_t seq = 1; seq <= FRAMES; seq++) {
        uint8_t* cur = weft_fanout_begin(&f);
        fill_pattern(cur, seq, PB);
        weft_fanout_publish(&f);
        if ((seq & 4095) == 0) weft_ipc_heartbeat(&sess, seq);
    }
    const uint64_t t1 = mono_ns_now();
    weft_ipc_heartbeat(&sess, FRAMES);

    int survivors_ok = 1, victim_crashed = 0;
    for (int k = 0; k < KIDS; k++) {
        int st = 0;
        waitpid(kids[k], &st, 0);
        if (k == KIDS - 1) {
            victim_crashed = WIFSIGNALED(st);
        } else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            survivors_ok = 0;
            printf("        consumer %d: status=%d\n", k, st);
        }
    }

    CHECK(survivors_ok, "3 survivor consumers exited clean");
    CHECK(victim_crashed, "consumer #4 crashed mid-claim (SIGKILL, no cleanup)");

    // Ring health after a mid-claim consumer crash (Axis 3).
    weft_ring_health_t after = WEFT_RING_HEALTHY;
    const weft_ring_health_t h = weft_ipc_heal_ring(&m.map, &after);
    CHECK(h == WEFT_RING_HEALTHY && after == WEFT_RING_HEALTHY,
          "ring HEALTHY after consumer crash mid-claim (readers are "
          "stateless observers)");

    // Telescoping identity per survivor: fresh + drops == FRAMES.
    int tele_ok = 1, bits_ok = 1;
    for (int k = 0; k < KIDS - 1; k++) {
        if (results[k].bitfails != 0) bits_ok = 0;
        if (results[k].fresh + results[k].drops != FRAMES) tele_ok = 0;
        if (results[k].last_seq != FRAMES) tele_ok = 0;
    }
    CHECK(bits_ok, "payload bit-exact on every fresh claim (survivors)");
    CHECK(tele_ok, "telescoping EXACT: fresh+drops == 100,000 per survivor");
    CHECK(results[KIDS - 1].fresh + results[KIDS - 1].drops >= VICTIM_AT,
          "victim progressed to frame seq >= 30,000 (fresh + telescoped) "
          "before crashing");

    // Advisory consumer count: 4 attaches, 3 clean leaves, 1 crash leak.
    weft_ipc_discovered_t d[2];
    int n = weft_ipc_discover(&reg, 1000, "torture-memfd", d, 2);
    CHECK(n == 1 && d[0].n_consumers == 1,
          "advisory n_consumers == 1 (crash leak visible, honest)");
    CHECK(n == 1 && d[0].heartbeat_seq >= FRAMES / 4096,
          "producer heartbeats advanced through the run");

    weft_ipc_unregister(&sess);
    weft_ipc_heal_report_t hr;
    weft_ipc_heal(&reg, 60000, &hr);

    const double secs = (double)(t1 - t0) / 1e9;
    printf("    [INFO] %d frames x %dB in %.3fs = %.0f pub/s "
           "(informational; 2 vCPU sandbox; 4 consumer processes claiming)\n",
           FRAMES, PB, secs, FRAMES / secs);
    for (int k = 0; k < KIDS; k++) {
        printf("    [INFO] consumer %d: fresh=%llu drops=%llu skip=%llu "
               "exhausted=%llu\n",
               k, (unsigned long long)results[k].fresh,
               (unsigned long long)results[k].drops,
               (unsigned long long)results[k].skipped,
               (unsigned long long)results[k].exhausted);
    }

    weft_fanout_destroy(&f);
    weft_shm_memfd_destroy(&m);
    weft_ipc_registry_close(&reg);
    return g_fail;
}

// ---------------------------------------------------------------------------
// mesh mode
// ---------------------------------------------------------------------------

static int torture_mesh(void) {
    printf("=== ipc-torture mesh: registry + crashy producer restart + "
           "successor (120,000 publications across 2 incarnations) ===\n");
    enum { CONS = 3, PHASE1 = 60000, FINAL = 120000, PB = 128, SLOTS = 4 };
    unlink("/dev/shm/weft_registry_v1");
    unlink("/tmp/weft-ipc/torture-mesh.sock");

    weft_ipc_registry_t reg;
    if (weft_ipc_registry_open(&reg, 1, 0) != 0) return 1;

    tort_result_t* results = mmap(NULL, sizeof(tort_result_t) * CONS,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (results == MAP_FAILED) return 1;
    memset(results, 0, sizeof(tort_result_t) * CONS);

    // ---- producer incarnation 1 (P1): crashy publisher -----------------
    pid_t p1 = fork();
    if (p1 == 0) {
        weft_ipc_registry_t r;
        if (weft_ipc_registry_open(&r, 1, 0) != 0) _exit(90);
        weft_shm_memfd_t m;
        if (weft_shm_memfd_create(PB, SLOTS, &m) != 0) _exit(91);
        if (weft_shm_memfd_ro_view(&m) != 0) _exit(92);
        int ls = -1;
        if (weft_shm_listen("torture-mesh", &ls) != 0) _exit(93);
        weft_ipc_session_t s;
        if (weft_ipc_register(&r, "torture-mesh", WEFT_IPC_TRANSPORT_MEMFD, PB,
                              SLOTS, 0, &s) != 0) _exit(94);
        // Serve CONS handshakes.
        for (int i = 0; i < CONS; i++) {
            int conn = weft_shm_accept(ls);
            if (conn < 0) _exit(95);
            if (weft_shm_handshake_serve(conn, &m, s.session_id, s.epoch,
                                         NULL, NULL) != 0) _exit(96);
            close(conn);
        }
        weft_fanout_t f;
        memset(&f, 0, sizeof f);
        if (weft_fanout_attach_writer(&f, m.map.ring,
                                      weft_shm_ring_bytes(&m.map), PB,
                                      SLOTS) != 0) _exit(97);
        // PER-FRAME heartbeat cadence: the continuity contract (the
        // successor seeds its numbering from the mirror, so the mirror
        // must never lag the published truth).
        for (uint32_t seq = 1; seq <= PHASE1; seq++) {
            uint8_t* cur = weft_fanout_begin(&f);
            fill_pattern(cur, seq, PB);
            weft_fanout_publish(&f);
            weft_ipc_heartbeat(&s, seq);
        }
        nap_ms(50);  // let consumers drain the tail
        kill(getpid(), SIGKILL);  // CRASHY exit: no unregister, no cleanup
        _exit(98);
    }

    // ---- consumers: discover, attach, watch for the successor ----------
    pid_t cons[CONS];
    for (int k = 0; k < CONS; k++) {
        cons[k] = fork();
        if (cons[k] == 0) {
            weft_ipc_registry_t r;
            if (weft_ipc_registry_open(&r, 1, 0) != 0) _exit(70);
            // Discover the session (bounded retry while P1 spins up).
            uint64_t sid = 0;
            for (int tries = 0; tries < 2000; tries++) {
                weft_ipc_discovered_t d[2];
                if (weft_ipc_discover(&r, 500, "torture-mesh", d, 2) == 1) {
                    sid = d[0].session_id;
                    break;
                }
                nap_ms(2);
            }
            if (sid == 0) _exit(71);

            weft_shm_memfd_t cm;
            weft_shm_grant_info_t g;
            int attached = 0;
            for (int tries = 0; tries < 2000 && !attached; tries++) {
                int rc = weft_shm_handshake_connect("torture-mesh", 0, sid,
                                                     NULL, &cm, &g);
                if (rc == 0 && g.status == WEFT_SHM_GRANT_OK) attached = 1;
                else nap_ms(2);
            }
            if (!attached) _exit(72);
            weft_ipc_consumer_attach(&r, sid);

            weft_fanout_reader_t rd;
            memset(&rd, 0, sizeof rd);
            if (weft_fanout_reader_init(&rd, cm.map.ring,
                                        weft_shm_ring_bytes(&cm.map), PB,
                                        SLOTS) != 0) _exit(73);

            uint64_t target = 0;
            uint64_t cur_epoch = g.epoch;
            results[k].epoch_seen = cur_epoch;
            int polls = 0;
            while (target < FINAL) {
                const weft_fanout_claim_t* c = weft_fanout_claim(&rd);
                if (c->fresh) {
                    results[k].fresh++;
                    results[k].drops += c->dropped;
                    target = c->seq;
                    results[k].last_seq = c->seq;
                    const uint8_t* v = (const uint8_t*)weft_fanout_view(&rd);
                    uint8_t e[PB];
                    fill_pattern(e, c->seq, PB);
                    if (memcmp(v, e, PB) != 0) results[k].bitfails++;
                } else if (++polls % 64 == 0) {
                    // Watch the mesh for an incarnation change.
                    weft_ipc_discovered_t d[2];
                    const int n = weft_ipc_discover(&r, 400, "torture-mesh", d, 2);
                    if (n == 1 && d[0].epoch != cur_epoch) {
                        // Successor detected: re-handshake, resync with OUR
                        // own last seq — telescoping spans the outage gap.
                        weft_shm_memfd_t nm;
                        weft_shm_grant_info_t ng;
                        int got = 0;
                        for (int tries = 0; tries < 5000 && !got; tries++) {
                            int rc = weft_shm_handshake_connect(
                                "torture-mesh", 0, d[0].session_id, NULL, &nm,
                                &ng);
                            if (rc == 0 && ng.status == WEFT_SHM_GRANT_OK &&
                                ng.epoch == d[0].epoch) {
                                got = 1;
                            } else {
                                nap_ms(2);
                            }
                        }
                        if (!got) _exit(74);
                        weft_fanout_reader_destroy(&rd);
                        memset(&rd, 0, sizeof rd);
                        if (weft_fanout_reader_init(
                                &rd, nm.map.ring,
                                weft_shm_ring_bytes(&nm.map), PB,
                                SLOTS) != 0) _exit(75);
                        weft_ipc_reader_resync(&rd, results[k].last_seq);
                        cur_epoch = d[0].epoch;
                        results[k].epoch_seen = cur_epoch;
                        weft_ipc_consumer_leave(&r, sid);
                        sid = d[0].session_id;
                        weft_ipc_consumer_attach(&r, sid);
                        // (cm replaced by nm; old map released)
                        weft_shm_memfd_destroy(&cm);
                        cm = nm;
                    } else if (n == 0) {
                        nap_ms(1);  // outage window — entry mid-takeover
                    }
                } else {
                    uint64_t last = results[k].last_seq;
                    (void)weft_shm_park(&cm.map, &last, 2000);
                }
            }
            weft_fanout_stats_t st;
            weft_fanout_reader_stats(&rd, &st);
            results[k].exhausted = st.torn_exhausted;
            weft_fanout_reader_destroy(&rd);
            weft_shm_memfd_destroy(&cm);
            weft_ipc_consumer_leave(&r, sid);
            weft_ipc_registry_close(&r);
            _exit(0);
        }
    }

    // ---- wait for P1's crash, then incarnate the successor P2 ----------
    int st1 = 0;
    waitpid(p1, &st1, 0);
    CHECK(WIFSIGNALED(st1), "producer P1 crashed (SIGKILL, no cleanup)");

    pid_t p2 = fork();
    if (p2 == 0) {
        weft_ipc_registry_t r;
        if (weft_ipc_registry_open(&r, 1, 0) != 0) _exit(90);
        // Successor: read the mirror from the STILL-ACTIVE (but dead)
        // incumbent, wait out the register() takeover gate's staleness
        // floor, then take the name over.
        uint64_t mirror = 0;
        uint32_t incumbent_epoch = 0;
        for (int tries = 0; tries < 400; tries++) {
            weft_ipc_discovered_t d[2];
            if (weft_ipc_discover(&r, 100, "torture-mesh", d, 2) == 1) {
                mirror = d[0].latest_seq_pub;
                incumbent_epoch = d[0].epoch;
                break;
            }
            nap_ms(5);
        }
        nap_ms(2300);
        weft_shm_memfd_t m;
        if (weft_shm_memfd_create(PB, SLOTS, &m) != 0) _exit(91);
        if (weft_shm_memfd_ro_view(&m) != 0) _exit(92);
        int ls = -1;
        if (weft_shm_listen("torture-mesh", &ls) != 0) _exit(93);
        weft_ipc_session_t s;
        int rc = weft_ipc_register(&r, "torture-mesh",
                                   WEFT_IPC_TRANSPORT_MEMFD, PB, SLOTS, mirror,
                                   &s);
        if (rc != 0) _exit(94);
        if (s.epoch != incumbent_epoch + 1) _exit(99);
        for (int i = 0; i < CONS; i++) {
            int conn = weft_shm_accept(ls);
            if (conn < 0) _exit(95);
            if (weft_shm_handshake_serve(conn, &m, s.session_id, s.epoch,
                                         NULL, NULL) != 0) _exit(96);
            close(conn);
        }
        weft_fanout_t f;
        memset(&f, 0, sizeof f);
        if (weft_fanout_attach_writer(&f, m.map.ring,
                                      weft_shm_ring_bytes(&m.map), PB,
                                      SLOTS) != 0) _exit(97);
        // Seed numbering from the mirror: the stream CONTINUES.
        f.w_seq = mirror;
        for (uint64_t seq = mirror + 1; seq <= FINAL; seq++) {
            uint8_t* cur = weft_fanout_begin(&f);
            fill_pattern(cur, seq, PB);
            weft_fanout_publish(&f);
            weft_ipc_heartbeat(&s, seq);
        }
        nap_ms(100);  // let consumers drain the tail
        weft_ipc_unregister(&s);  // CLEAN detach for the successor
        weft_ipc_heal_report_t hr;
        weft_ipc_heal(&r, 60000, &hr);
        _exit(0);
    }

    int survivors_ok = 1, p2_ok = 0;
    for (int k = 0; k < CONS; k++) {
        int st = 0;
        waitpid(cons[k], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            survivors_ok = 0;
            printf("        consumer %d: status=%d (code %d)\n", k, st,
                   WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        }
    }
    int st2 = 0;
    waitpid(p2, &st2, 0);
    p2_ok = WIFEXITED(st2) && WEXITSTATUS(st2) == 0;

    CHECK(p2_ok, "successor producer P2: takeover (epoch 2), 60k frames, "
                 "clean detach");
    CHECK(survivors_ok, "3 consumers survived the crash + re-attached");

    int tele_ok = 1, bits_ok = 1, epoch_ok = 1;
    for (int k = 0; k < CONS; k++) {
        if (results[k].bitfails != 0) bits_ok = 0;
        if (results[k].fresh + results[k].drops != FINAL) tele_ok = 0;
        if (results[k].last_seq != FINAL) tele_ok = 0;
        if (results[k].epoch_seen != 2) epoch_ok = 0;
    }
    CHECK(bits_ok, "payload bit-exact ACROSS incarnations "
                   "(pattern keyed by continued frame seq)");
    CHECK(tele_ok, "telescoping spans the outage EXACTLY: fresh+drops == "
                   "120,000 per consumer");
    CHECK(epoch_ok, "every consumer observed the epoch 1 -> 2 transition");

    // Mesh cleanliness: successor unregistered + healed; consumer count 0
    // (all clean leaves — no crash victims among mesh consumers).
    weft_ipc_discovered_t d[2];
    const int n = weft_ipc_discover(&reg, 1000, "torture-mesh", d, 2);
    CHECK(n == 0, "mesh entry gone after successor clean detach + heal");

    for (int k = 0; k < CONS; k++) {
        printf("    [INFO] mesh consumer %d: fresh=%llu drops=%llu "
               "exhausted=%llu epoch=%llu\n",
               k, (unsigned long long)results[k].fresh,
               (unsigned long long)results[k].drops,
               (unsigned long long)results[k].exhausted,
               (unsigned long long)results[k].epoch_seen);
    }
    printf("    [INFO] the drops count IS the outage gap + any claim-cadence "
           "losses — accounted, never phantom\n");

    weft_ipc_registry_close(&reg);
    unlink("/tmp/weft-ipc/torture-mesh.sock");
    return g_fail;
}

// ---------------------------------------------------------------------------
// latency mode
// ---------------------------------------------------------------------------

static int torture_latency(void) {
    printf("=== ipc-torture latency: park (zero-syscall) vs eventfd "
           "wake-up, cross-process ===\n");
    enum { ROUNDS = 10000, PB = 64, SLOTS = 2 };
    unlink("/dev/shm/weft_registry_v1");

    // Shared: ack + results arrays.
    struct shared {
        volatile uint64_t ack;
        uint64_t park_ns[ROUNDS];
        uint64_t efd_ns[ROUNDS];
    };
    struct shared* sh = mmap(NULL, sizeof(struct shared), PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) return 1;
    memset(sh, 0, sizeof *sh);

    // ---- leg 1: publish -> park (the real data path) -------------------
    {
        weft_shm_memfd_t m;
        if (weft_shm_memfd_create(PB, SLOTS, &m) != 0) return 1;
        if (weft_shm_memfd_ro_view(&m) != 0) return 1;
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        pid_t obs = fork();
        if (obs == 0) {
            // Observer: handshake for the RO view, park until fresh.
            close(sv[0]);
            weft_shm_memfd_t cm;
            weft_shm_grant_info_t g;
            if (weft_shm_handshake_connect_fd(sv[1], 0, 0, NULL, &cm, &g) != 0) {
                _exit(60);
            }
            weft_fanout_reader_t r;
            memset(&r, 0, sizeof r);
            if (weft_fanout_reader_init(&r, cm.map.ring,
                                        weft_shm_ring_bytes(&cm.map), PB,
                                        SLOTS) != 0) _exit(61);
            for (int i = 0; i < ROUNDS; i++) {
                const weft_fanout_claim_t* c = weft_fanout_claim(&r);
                if (!c->fresh) {
                    // Seed park with the CURRENT latestSeq: it returns when
                    // a NEWER frame lands (that is the doorbell-free wait).
                    uint64_t last = weft_shm_latest_seq(&cm.map);
                    while (weft_shm_park(&cm.map, &last, 2000000) != 0) {
                        // budget exhausted — re-park (caller cadence);
                        // with a live publisher this virtually never trips.
                    }
                    c = weft_fanout_claim(&r);
                }
                if (!c->fresh) _exit(62);  // park lied — hard failure
                // Verify + ack.
                const uint8_t* v = (const uint8_t*)weft_fanout_view(&r);
                uint8_t e[PB];
                fill_pattern(e, c->seq, PB);
                if (memcmp(v, e, PB) != 0) _exit(63);
                __atomic_store_n(&sh->ack, (uint64_t)(i + 1),
                                 __ATOMIC_RELEASE);
            }
            _exit(0);
        }
        close(sv[1]);
        weft_fanout_t f;
        memset(&f, 0, sizeof f);
        weft_fanout_attach_writer(&f, m.map.ring, weft_shm_ring_bytes(&m.map),
                                  PB, SLOTS);
        if (weft_shm_handshake_serve(sv[0], &m, 1, 1, NULL, NULL) != 0) {
            return 1;
        }
        close(sv[0]);
        for (uint32_t i = 1; i <= ROUNDS; i++) {
            const uint64_t t0 = mono_ns_now();
            uint8_t* cur = weft_fanout_begin(&f);
            fill_pattern(cur, i, PB);
            weft_fanout_publish(&f);
            // Wait for the observer's ack (bounded poll on shared memory).
            while (__atomic_load_n(&sh->ack, __ATOMIC_ACQUIRE) < i) {
                /* spin */
            }
            sh->park_ns[i - 1] = mono_ns_now() - t0;
        }
        int st = 0;
        waitpid(obs, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "park leg: 10,000 rounds, every frame claimed + verified");
        weft_fanout_destroy(&f);
        weft_shm_memfd_destroy(&m);
    }

    // ---- leg 2: eventfd write -> blocking read (the classic doorbell) --
    {
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        pid_t child = fork();
        if (child == 0) {
            close(sv[0]);
            int efd = -1;
            uint8_t tag = 0;
            size_t got = 0;
            if (weft_shm_recv_fd(sv[1], &efd, &tag, 1, &got) != 0 || got != 1) {
                _exit(50);
            }
            for (int i = 0; i < ROUNDS; i++) {
                uint64_t v = 0;
                if (read(efd, &v, 8) != 8) _exit(51);
                __atomic_store_n(&sh->ack, (uint64_t)(i + 1),
                                 __ATOMIC_RELEASE);
            }
            _exit(0);
        }
        close(sv[1]);
        const int efd = eventfd(0, EFD_CLOEXEC);
        if (efd < 0) return 1;
        if (weft_shm_send_fd(sv[0], efd, "E", 1) != 0) return 1;
        uint64_t acks = 0;
        __atomic_store_n(&sh->ack, 0, __ATOMIC_RELEASE);
        for (int i = 0; i < ROUNDS; i++) {
            const uint64_t t0 = mono_ns_now();
            const uint64_t one = 1;
            if (write(efd, &one, 8) != 8) return 1;
            while ((acks = __atomic_load_n(&sh->ack, __ATOMIC_ACQUIRE)) <
                   (uint64_t)i + 1) {
                /* spin on ack */
            }
            sh->efd_ns[i] = mono_ns_now() - t0;
        }
        int st = 0;
        waitpid(child, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "eventfd leg: 10,000 wake-up rounds");
        close(efd);
        close(sv[0]);
    }

    report_pct("park (zero-syscall)", sh->park_ns, ROUNDS);
    report_pct("eventfd wake-up", sh->efd_ns, ROUNDS);
    printf("    [INFO] park = bounded spin (burns CPU while waiting, Law 1 "
           "capped); eventfd = blocking (zero CPU while waiting, syscall + "
           "scheduler wakeup). Different tools — measured, not asserted.\n");
    return g_fail;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "all";
    int fails = 0;
    if (strcmp(mode, "memfd") == 0 || strcmp(mode, "all") == 0) {
        fails += torture_memfd();
    }
    if (strcmp(mode, "mesh") == 0 || strcmp(mode, "all") == 0) {
        fails += torture_mesh();
    }
    if (strcmp(mode, "latency") == 0 || strcmp(mode, "all") == 0) {
        fails += torture_latency();
    }
    printf("=== ipc-torture (%s): %d failures ===\n", mode, fails);
    return fails == 0 ? 0 : 1;
}
