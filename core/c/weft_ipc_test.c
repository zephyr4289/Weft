// weft_ipc_test.c — R-series: IPC mesh registry + token conformance
// (RFC-0016).
//
//   R1  registry lifecycle: create-or-attach, exact-size validation,
//       decoy objects refused (wrong magic / wrong size)
//   R2  session lifecycle: register -> heartbeat -> discover -> unregister;
//       immutable fields verbatim; heartbeat advances; mirror refreshes
//   R3  concurrent registration: 8 threads racing register/unregister;
//       no duplicate ACTIVE names survive; capacity respected
//   R4  crash detection: forked producer SIGKILLed; heal (stale + dead
//       pid — both legs) crashes it out to DEAD
//   R5  successor semantics: same-name takeover from a crashed incumbent
//       (epoch+1); live duplicate refused; tombstone reaped after
//   R6  capability tokens: issue/verify happy path, tampered tag, wrong
//       session, expired, epoch mismatch, malformed, adapter mapping
//   R7  consumer advisory counter: attach/leave exact under clean
//       lifecycles; leave-at-zero is a no-op (never wraps)
//   R8  cross-process discovery: forked child registers + heartbeats,
//       parent discovers by name; child detaches cleanly; entry gone
//   R9  heal idempotence: second pass finds nothing to do
//   R10 monitor posture: read-only registry view discovers, refuses
//       mutations loudly
//   R11 Axis-3 integration: corrupted ring detected + recovered through
//       weft_ipc_heal_ring
//
// Registry note: the object path is fixed mesh infrastructure
// (/dev/shm/weft_registry_v1); this suite unlinks + recreates it at start
// for determinism (CI runs suites sequentially — declared).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fanout.h"
#include "shm_ring.h"
#include "weft.h"
#include "weft_ipc.h"
#include "weft_shm.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", name); g_pass++; } \
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

// ---------------------------------------------------------------------
// R3 worker (file scope — C has no nested functions)
// ---------------------------------------------------------------------
#define R3_THREADS 8
#define R3_CYCLES 30

static weft_ipc_registry_t* r3_reg;
static int r3_dup_total = 0;
static pthread_mutex_t r3_mu = PTHREAD_MUTEX_INITIALIZER;

static void* r3_worker(void* p) {
    const int id = *(int*)p;
    char name[32];
    for (int c = 0; c < R3_CYCLES; c++) {
        snprintf(name, sizeof name, "r3-w%d-%d", id, c % 4);
        weft_ipc_session_t s;
        if (weft_ipc_register(r3_reg, name, WEFT_IPC_TRANSPORT_NAMED, 64, 4,
                              0, &s) == 0) {
            // Audit: exactly ONE active entry with this name.
            weft_ipc_discovered_t f[4];
            const int n = weft_ipc_discover(r3_reg, 60000, name, f, 4);
            if (n > 1) {
                pthread_mutex_lock(&r3_mu);
                r3_dup_total++;
                pthread_mutex_unlock(&r3_mu);
            }
            weft_ipc_heartbeat(&s, (uint64_t)c);
            weft_ipc_unregister(&s);
        }
    }
    return NULL;
}

int main(void) {
    printf("=== R-series: Weft IPC mesh registry + tokens (RFC-0016) ===\n");

    // Fresh registry for determinism.
    unlink("/dev/shm/weft_registry_v1");

    // ------------------------------------------------------------------ R1
    weft_ipc_registry_t reg;
    {
        CHECK(weft_ipc_registry_open(&reg, 1, 0) == 0,
              "R1a create-or-attach (creator)");
        struct stat st;
        CHECK(stat("/dev/shm/weft_registry_v1", &st) == 0 &&
                  (size_t)st.st_size == WEFT_IPC_REGISTRY_BYTES,
              "R1b exact registry size");
        weft_ipc_registry_t again;
        CHECK(weft_ipc_registry_open(&again, 1, 0) == 0,
              "R1c second opener attaches");
        weft_ipc_registry_close(&again);
        // Decoy 1: wrong size.
        unlink("/dev/shm/weft_registry_v1");
        {
            int fd = open("/dev/shm/weft_registry_v1",
                          O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
            write(fd, "garbage", 7);
            close(fd);
            weft_ipc_registry_t dec;
            CHECK(weft_ipc_registry_open(&dec, 0, 0) == WEFT_IPC_ERR_INVALID,
                  "R1d garbage object refused");
        }
        // Decoy 2: right size, wrong magic.
        unlink("/dev/shm/weft_registry_v1");
        {
            int fd = open("/dev/shm/weft_registry_v1",
                          O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
            for (size_t i = 0; i < WEFT_IPC_REGISTRY_BYTES; i += 4096) {
                static const char zero[4096] = {0};
                write(fd, zero, sizeof zero);
            }
            close(fd);
            weft_ipc_registry_t dec;
            CHECK(weft_ipc_registry_open(&dec, 0, 0) == WEFT_IPC_ERR_INVALID,
                  "R1e zeroed-right-size decoy refused");
        }
        // Absent + no-create.
        unlink("/dev/shm/weft_registry_v1");
        weft_ipc_registry_t dec;
        CHECK(weft_ipc_registry_open(&dec, 0, 0) == WEFT_IPC_ERR_SYS,
              "R1f absent registry, create disabled -> explicit failure");
        // Recreate for the rest of the suite.
        CHECK(weft_ipc_registry_open(&reg, 1, 0) == 0, "R1g recreate");
    }

    // ------------------------------------------------------------------ R2
    {
        weft_ipc_session_t s;
        int rc = weft_ipc_register(&reg, "r2-engine", WEFT_IPC_TRANSPORT_MEMFD,
                                   256, 8, 0, &s);
        CHECK(rc == 0 && s.active == 1 && s.session_id != 0,
              "R2a register (epoch 1)");
        CHECK(s.epoch == 1, "R2b fresh epoch = 1");
        nap_ms(5);
        CHECK(weft_ipc_heartbeat(&s, 41) == 0, "R2c heartbeat ok");
        nap_ms(5);
        CHECK(weft_ipc_heartbeat(&s, 97) == 0, "R2d second heartbeat");

        weft_ipc_discovered_t found[8];
        int n = weft_ipc_discover(&reg, 1000, "r2-engine", found, 8);
        CHECK(n == 1, "R2e discover by exact name");
        if (n == 1) {
            CHECK(found[0].session_id == s.session_id &&
                      found[0].epoch == 1 &&
                      found[0].transport == WEFT_IPC_TRANSPORT_MEMFD &&
                      found[0].payload_bytes == 256 &&
                      found[0].slot_count == 8 &&
                      strcmp(found[0].name, "r2-engine") == 0 &&
                      found[0].producer_pid == (uint32_t)getpid(),
                  "R2f immutable fields verbatim");
            CHECK(found[0].heartbeat_seq == 2 && found[0].latest_seq_pub == 97,
                  "R2g advisory heartbeat + mirror");
            CHECK(found[0].stale == 0 && found[0].producer_alive == 1,
                  "R2h liveness verdict fresh+alive");
        }
        CHECK(weft_ipc_unregister(&s) == 0 && s.active == 0,
              "R2i clean unregister");
        CHECK(weft_ipc_unregister(&s) == 0, "R2j unregister idempotent");
        n = weft_ipc_discover(&reg, 1000, "r2-engine", found, 8);
        CHECK(n == 0, "R2k DEAD entry not discovered");
    }

    // ------------------------------------------------------------------ R3
    {
        // 8 threads x 30 register/unregister cycles; each worker audits
        // that its own name never duplicates while registered.
        pthread_t th[R3_THREADS];
        int ids[R3_THREADS];
        r3_reg = &reg;
        r3_dup_total = 0;
        for (int i = 0; i < R3_THREADS; i++) {
            ids[i] = i;
            pthread_create(&th[i], NULL, r3_worker, &ids[i]);
        }
        for (int i = 0; i < R3_THREADS; i++) pthread_join(th[i], NULL);
        CHECK(r3_dup_total == 0,
              "R3a 8 threads x 30 register/unregister: zero duplicate names");
        weft_ipc_heal_report_t hr;
        weft_ipc_heal(&reg, 60000, &hr);  // reap all DEAD tombstones
        CHECK(hr.active_now == 0, "R3b registry empty after heal");
    }

    // ------------------------------------------------------------------ R4
    {
        pid_t pid = fork();
        if (pid == 0) {
            // Crashed producer: registers, one heartbeat, then SIGKILL self.
            weft_ipc_registry_t r2;
            if (weft_ipc_registry_open(&r2, 1, 0) != 0) _exit(1);
            weft_ipc_session_t s;
            if (weft_ipc_register(&r2, "r4-doomed", WEFT_IPC_TRANSPORT_MEMFD,
                                  128, 4, 5, &s) != 0) _exit(2);
            weft_ipc_heartbeat(&s, 10);
            kill(getpid(), SIGKILL);
            _exit(3);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(!(WIFEXITED(st) && WEXITSTATUS(st) != 0) || WIFSIGNALED(st),
              "R4a producer crashed (SIGKILL)");
        // Entry still ACTIVE-looking; heartbeat is fresh. Heal must NOT
        // evict a fresh entry even though... it is dead (staleness first).
        weft_ipc_heal_report_t hr;
        CHECK(weft_ipc_heal(&reg, 60000, &hr) == 0 && hr.crashed_detected == 0,
              "R4b fresh-heartbeat entry NOT crashed out (staleness gate)");
        nap_ms(250);  // let the heartbeat go stale (stale_ms=100 below)
        CHECK(weft_ipc_heal(&reg, 100, &hr) == 0 && hr.crashed_detected == 1,
              "R4c stale + dead pid -> crashed out (both legs)");
        weft_ipc_discovered_t f[4];
        CHECK(weft_ipc_discover(&reg, 100, "r4-doomed", f, 4) == 0,
              "R4d crashed session no longer discoverable");
    }

    // ------------------------------------------------------------------ R5
    {
        int st = 0;
        // The R4 incumbent is DEAD (not yet reaped): successor takes the
        // SAME name with epoch+1.
        weft_ipc_session_t s;
        int rc = weft_ipc_register(&reg, "r4-doomed", WEFT_IPC_TRANSPORT_MEMFD,
                                   128, 4, 11, &s);
        CHECK(rc == 0 && s.epoch == 2,
              "R5a successor takeover from DEAD incumbent (epoch 2)");
        // Live duplicate must refuse.
        pid_t pid = fork();
        if (pid == 0) {
            weft_ipc_registry_t r2;
            if (weft_ipc_registry_open(&r2, 1, 0) != 0) _exit(1);
            weft_ipc_session_t dup;
            int drc = weft_ipc_register(&r2, "r4-doomed",
                                        WEFT_IPC_TRANSPORT_MEMFD, 128, 4, 0,
                                        &dup);
            _exit(drc == WEFT_IPC_ERR_DUPLICATE ? 0 : 2);
        }
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "R5b live incumbent refuses duplicate producer");
        weft_ipc_unregister(&s);
        weft_ipc_heal_report_t hr;
        weft_ipc_heal(&reg, 60000, &hr);
        CHECK(hr.reaped_dead >= 1, "R5c tombstone reaped after clean detach");
        // After a FULL reap the name is gone: a third registration is a
        // fresh epoch-1 session (continuity lives in the tombstone window
        // and in the fresh random session_id — documented semantics).
        weft_ipc_session_t s3;
        rc = weft_ipc_register(&reg, "r4-doomed", WEFT_IPC_TRANSPORT_MEMFD,
                               128, 4, 0, &s3);
        CHECK(rc == 0 && s3.epoch == 1,
              "R5d post-reap registration: fresh epoch (tombstone gone)");
        weft_ipc_unregister(&s3);
        weft_ipc_heal(&reg, 60000, &hr);
    }

    // ------------------------------------------------------------------ R6
    {
        const uint64_t SID = 0xFEED0001ull;
        uint8_t key[WEFT_IPC_KEY_BYTES];
        weft_ipc_random_bytes(key, sizeof key);
        weft_ipc_claims_t c = { .session_id = SID,
                                .perms = WEFT_IPC_PERM_READ |
                                         WEFT_IPC_PERM_CLAIM |
                                         WEFT_IPC_PERM_WRITE,
                                .epoch = 3, .expiry_unix = 0,
                                .key_id = 9, .nonce = 123 };
        uint8_t tok[WEFT_IPC_TOKEN_BYTES];
        CHECK(weft_ipc_token_issue(&c, key, tok) == 0, "R6a issue");
        weft_ipc_claims_t out;
        CHECK(weft_ipc_token_verify(tok, key, SID, 3, &out) == 0 &&
                  out.perms == c.perms && out.expiry_unix == 0,
              "R6b verify happy path");
        // Codec round trip.
        uint8_t canon[32];
        weft_ipc_claims_encode(&c, canon);
        weft_ipc_claims_t rt;
        CHECK(weft_ipc_claims_decode(canon, &rt) == 0 &&
                  rt.session_id == SID && rt.nonce == 123,
              "R6c claims codec round trip");
        // Tampered tag.
        uint8_t bad[WEFT_IPC_TOKEN_BYTES];
        memcpy(bad, tok, sizeof bad);
        bad[40] ^= 1;
        CHECK(weft_ipc_token_verify(bad, key, SID, 3, &out) == -2,
              "R6d tampered tag -> bad HMAC");
        // Tampered claims (re-issue with different perms, same tag).
        memcpy(bad, tok, sizeof bad);
        bad[8] = 0xFF;  // perms field -> unknown bits AND tag mismatch
        CHECK(weft_ipc_token_verify(bad, key, SID, 3, &out) != 0,
              "R6e tampered claims refused");
        // Wrong session.
        CHECK(weft_ipc_token_verify(tok, key, SID + 1, 3, &out) == -3,
              "R6f wrong session");
        // Expired.
        weft_ipc_claims_t ce = c;
        ce.expiry_unix = 1;
        uint8_t toke[WEFT_IPC_TOKEN_BYTES];
        weft_ipc_token_issue(&ce, key, toke);
        CHECK(weft_ipc_token_verify(toke, key, SID, 3, &out) == -4,
              "R6g expired");
        // Epoch mismatch.
        CHECK(weft_ipc_token_verify(tok, key, SID, 4, &out) == -5,
              "R6h epoch mismatch");
        // Malformed: all-zero token.
        uint8_t zero[WEFT_IPC_TOKEN_BYTES] = {0};
        CHECK(weft_ipc_token_verify(zero, key, SID, 3, &out) == -6,
              "R6i all-zero token malformed");
        // Adapter mapping.
        weft_ipc_token_ctx_t vctx = { .session_id = SID, .epoch = 3 };
        memcpy(vctx.key, key, sizeof key);
        uint32_t granted = 0;
        CHECK(weft_ipc_token_verify_adapter(&vctx, tok, SID,
                                            WEFT_IPC_PERM_WRITE,
                                            &granted) == 0 &&
                  (granted & WEFT_IPC_PERM_WRITE) != 0,
              "R6j adapter grants WRITE from valid token");
        CHECK(weft_ipc_token_verify_adapter(&vctx, bad, SID,
                                            WEFT_IPC_PERM_WRITE,
                                            &granted) != 0,
              "R6k adapter denies tampered token");
    }

    // ------------------------------------------------------------------ R7
    {
        weft_ipc_session_t s;
        weft_ipc_register(&reg, "r7-counted", WEFT_IPC_TRANSPORT_NAMED, 64, 4,
                          0, &s);
        CHECK(weft_ipc_consumer_attach(&reg, s.session_id) == 0, "R7a attach 1");
        weft_ipc_consumer_attach(&reg, s.session_id);
        weft_ipc_consumer_attach(&reg, s.session_id);
        weft_ipc_discovered_t f[2];
        weft_ipc_discover(&reg, 1000, "r7-counted", f, 2);
        CHECK(f[0].n_consumers == 3, "R7b advisory count = 3");
        weft_ipc_consumer_leave(&reg, s.session_id);
        weft_ipc_consumer_leave(&reg, s.session_id);
        weft_ipc_discover(&reg, 1000, "r7-counted", f, 2);
        CHECK(f[0].n_consumers == 1, "R7c clean leaves decrement exactly");
        weft_ipc_consumer_leave(&reg, s.session_id);
        weft_ipc_consumer_leave(&reg, s.session_id);  // at zero: no-op
        weft_ipc_discover(&reg, 1000, "r7-counted", f, 2);
        CHECK(f[0].n_consumers == 0, "R7d leave-at-zero never wraps");
        CHECK(weft_ipc_consumer_attach(&reg, 0x1234) == WEFT_IPC_ERR_INVALID,
              "R7e unknown session refused");
        weft_ipc_unregister(&s);
        weft_ipc_heal_report_t hr;
        weft_ipc_heal(&reg, 60000, &hr);
    }

    // ------------------------------------------------------------------ R8
    {
        pid_t pid = fork();
        if (pid == 0) {
            weft_ipc_registry_t r2;
            if (weft_ipc_registry_open(&r2, 1, 0) != 0) _exit(1);
            weft_ipc_session_t s;
            if (weft_ipc_register(&r2, "r8-crossproc", WEFT_IPC_TRANSPORT_MEMFD,
                                  512, 6, 3, &s) != 0) _exit(2);
            for (int i = 0; i < 5; i++) {
                nap_ms(10);
                if (weft_ipc_heartbeat(&s, (uint64_t)(3 + i)) != 0) _exit(3);
            }
            weft_ipc_unregister(&s);
            _exit(0);
        }
        int st = 0;
        // Parent discovers while the child heartbeats.
        int seen_active = 0, max_hb = 0;
        for (int i = 0; i < 12; i++) {
            weft_ipc_discovered_t f[2];
            int n = weft_ipc_discover(&reg, 1000, "r8-crossproc", f, 2);
            if (n == 1) {
                seen_active = 1;
                if (f[0].heartbeat_seq > (uint64_t)max_hb) {
                    max_hb = (int)f[0].heartbeat_seq;
                }
                CHECK(f[0].producer_pid != (uint32_t)getpid(),
                      "R8a discovered producer is ANOTHER process");
            }
            nap_ms(8);
        }
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "R8b child clean detach");
        CHECK(seen_active && max_hb >= 1,
              "R8c cross-process discovery + live heartbeats observed");
        weft_ipc_discovered_t f[2];
        CHECK(weft_ipc_discover(&reg, 1000, "r8-crossproc", f, 2) == 0,
              "R8d entry gone after clean detach");
        weft_ipc_heal_report_t hr;
        weft_ipc_heal(&reg, 60000, &hr);
    }

    // ------------------------------------------------------------------ R9
    {
        weft_ipc_heal_report_t h1, h2;
        weft_ipc_heal(&reg, 60000, &h1);
        weft_ipc_heal(&reg, 60000, &h2);
        CHECK(h1.crashed_detected == h2.crashed_detected &&
                  h2.reaped_dead == 0 && h2.reaped_reserved == 0,
              "R9a heal idempotent (second pass has nothing to do)");
        // Generation/ABA: register, crash-out via heal, re-register — the
        // slot's generation advanced, the old handle is loudly invalid.
        weft_ipc_session_t s;
        weft_ipc_register(&reg, "r9-aba", WEFT_IPC_TRANSPORT_NAMED, 64, 4, 0,
                          &s);
        // Simulate the producer vanishing: overwrite the pid field with a
        // dead one, let the heartbeat go stale, heal.
        // (Direct field surgery: TEST-ONLY, reachable via the entry layout
        // the header documents.)
        // entry pid offset: 64 + slot*160 + 24.
        uint8_t* ent = reg.base + WEFT_IPC_REGISTRY_HEADER_BYTES +
                       (size_t)s.slot * WEFT_IPC_REGISTRY_ENTRY_BYTES;
        __atomic_store_n((uint32_t*)(ent + 24), 0xFFFFFFu, __ATOMIC_RELAXED);
        nap_ms(250);
        weft_ipc_heal(&reg, 100, &h1);
        CHECK(h1.crashed_detected == 1, "R9b stale+dead pid crashed out");
        CHECK(weft_ipc_heartbeat(&s, 1) == WEFT_IPC_ERR_INVALID,
              "R9c healed-out handle fails loudly (no silent zombie)");
        weft_ipc_session_t s2;
        int rc = weft_ipc_register(&reg, "r9-aba", WEFT_IPC_TRANSPORT_NAMED, 64,
                                   4, 0, &s2);
        CHECK(rc == 0 && s2.epoch == 2, "R9d re-registration succeeds (gen+1)");
        weft_ipc_unregister(&s2);
        weft_ipc_heal(&reg, 60000, &h2);
    }

    // ------------------------------------------------------------------ R10
    {
        weft_ipc_session_t s;
        weft_ipc_register(&reg, "r10-mon", WEFT_IPC_TRANSPORT_NAMED, 64, 4, 0,
                          &s);
        weft_ipc_registry_t ro;
        CHECK(weft_ipc_registry_open(&ro, 0, 1) == 0,
              "R10a monitor (read-only) view opens");
        weft_ipc_discovered_t f[2];
        CHECK(weft_ipc_discover(&ro, 1000, "r10-mon", f, 2) == 1,
              "R10b monitor discovers");
        weft_ipc_session_t sbad;
        CHECK(weft_ipc_register(&ro, "nope", WEFT_IPC_TRANSPORT_NAMED, 64, 4, 0,
                                &sbad) == WEFT_IPC_ERR_INVALID,
              "R10c monitor refuses registration");
        CHECK(weft_ipc_consumer_attach(&ro, s.session_id) ==
                  WEFT_IPC_ERR_INVALID,
              "R10d monitor refuses consumer attach");
        weft_ipc_heal_report_t hr;
        CHECK(weft_ipc_heal(&ro, 1000, &hr) == WEFT_IPC_ERR_READONLY,
              "R10e monitor refuses healing (CAS would fault)");
        weft_ipc_registry_close(&ro);
        weft_ipc_unregister(&s);
        weft_ipc_heal(&reg, 60000, &hr);
    }

    // ------------------------------------------------------------------ R11
    {
        weft_shm_memfd_t m;
        CHECK(weft_shm_memfd_create(64, 4, &m) == 0, "R11a session for heal");
        weft_fanout_t w;
        memset(&w, 0, sizeof w);
        weft_fanout_attach_writer(&w, m.map.ring, weft_shm_ring_bytes(&m.map),
                                  64, 4);
        for (uint32_t i = 1; i <= 20; i++) {
            uint8_t* cur = weft_fanout_begin(&w);
            for (uint32_t j = 0; j < 64; j++) cur[j] = weft_pat(i, j);
            weft_fanout_publish(&w);
        }
        weft_ring_health_t after = WEFT_RING_HEALTHY;
        weft_ring_health_t before = weft_ipc_heal_ring(&m.map, &after);
        CHECK(before == WEFT_RING_HEALTHY && after == WEFT_RING_HEALTHY,
              "R11b healthy ring passes through");
        // Corrupt: future seq (latestSeq beyond possible monotonic state).
        _Atomic uint64_t* ctrl = (_Atomic uint64_t*)m.map.ring;
        atomic_store_explicit(ctrl, 999999, memory_order_release);
        before = weft_ipc_heal_ring(&m.map, &after);
        CHECK(before != WEFT_RING_HEALTHY && after == WEFT_RING_HEALTHY,
              "R11c corrupted ring detected + Axis-3 recovered");
        weft_fanout_destroy(&w);
        weft_shm_memfd_destroy(&m);
    }

    weft_ipc_registry_close(&reg);
    printf("=== R-series: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
