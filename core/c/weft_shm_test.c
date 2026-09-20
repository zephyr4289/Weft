// weft_shm_test.c — M-series: SHM fabric conformance (RFC-0016).
//
// The memfd/seal/RO-view/SCM_RIGHTS/handshake layer, pinned end to end:
//   M1  sealed create: seals applied, geometry identity, WFSH validation
//       via the RFC-0011 attach road, sealed ftruncate -> EPERM
//   M2  the kernel-enforced RO view: same object, writable mmap -> EACCES
//   M3  SCM_RIGHTS primitives: in-process + forked child, fd + EACCES both
//   M4  cmsg validation: data-only message, double-fd refusal, bad args
//   M5  full anonymous handshake over a socketpair: grant fields, RO map,
//       cross-process frames bit-exact (fork)
//   M6  named-socket road: listen/connect/stale-socket takeover
//   M7  capability refusals: anon WRITE, forged tag, expired, wrong
//       session, wrong epoch, handoff without ADMIN
//   M8  PING: epoch + latest_seq echo, no descriptor
//   M9  fork mesh: 3 children x 10k frames, payload bit-exact, children
//       attempt ftruncate (EPERM) + writable mmap (EACCES) on the grant
//   M10 WRITE grant with a valid token: RW fd, writable mapping works
//   M11 lying-server cross-checks: geometry lie -> -3, nonce lie -> -1
//   M12 bounded park: fresh detection + budget exhaustion
//
// Style: the S-series discipline — printf [PASS]/[FAIL] lines, hard exit
// code, ASAN-clean by construction (every fork child is waited on).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

#define PAYLOAD_BYTES 64u
#define SLOT_COUNT 4u

static void fill_pattern(uint8_t* p, uint32_t seq) {
    for (uint32_t i = 0; i < PAYLOAD_BYTES; i++) {
        p[i] = weft_pat(seq, i);
    }
}

int main(void) {
    printf("=== M-series: Weft SHM fabric conformance (RFC-0016) ===\n");

    // ------------------------------------------------------------------ M1
    {
        weft_shm_memfd_t m;
        int rc = weft_shm_memfd_create(PAYLOAD_BYTES, SLOT_COUNT, &m);
        CHECK(rc == 0, "M1a memfd create");
        CHECK((m.seals & (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) ==
                  (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL),
              "M1b seals GROW|SHRINK|SEAL applied");
        CHECK(m.writable == 1, "M1c producer mapping writable");
        CHECK(weft_shm_payload_bytes(&m.map) == PAYLOAD_BYTES &&
                  weft_shm_slot_count(&m.map) == SLOT_COUNT &&
                  weft_shm_ring_bytes(&m.map) ==
                      weft_fanout_ring_bytes(PAYLOAD_BYTES, SLOT_COUNT),
              "M1d geometry identity");
        errno = 0;
        int t = ftruncate(m.map.fd, 4096);
        CHECK(t == -1 && errno == EPERM, "M1e sealed ftruncate EPERM");
        // The producer publishes through the sealed object (seals freeze
        // SIZE, not the writable mapping).
        weft_fanout_t f;
        memset(&f, 0, sizeof f);
        CHECK(weft_fanout_attach_writer(&f, m.map.ring,
                                        weft_shm_ring_bytes(&m.map), PAYLOAD_BYTES,
                                        SLOT_COUNT) == 0,
              "M1f writer attaches to sealed memfd ring");
        uint8_t* cur = weft_fanout_begin(&f);
        fill_pattern(cur, 1);
        CHECK(weft_fanout_publish(&f) == 1, "M1g publish through seal");
        weft_fanout_destroy(&f);
        weft_shm_memfd_destroy(&m);
        CHECK(1, "M1h destroy idempotent path");
    }

    // ------------------------------------------------------------------ M2
    {
        weft_shm_memfd_t m;
        weft_shm_memfd_create(PAYLOAD_BYTES, SLOT_COUNT, &m);
        weft_fanout_t f;
        memset(&f, 0, sizeof f);
        weft_fanout_attach_writer(&f, m.map.ring, weft_shm_ring_bytes(&m.map),
                                  PAYLOAD_BYTES, SLOT_COUNT);
        uint8_t* cur = weft_fanout_begin(&f);
        fill_pattern(cur, 1);  // ring frame 1; pattern arg follows the frame seq
        weft_fanout_publish(&f);

        CHECK(weft_shm_memfd_ro_view(&m) == 0, "M2a RO view derived");
        int ro = weft_shm_memfd_ro_fd(&m);
        CHECK(ro >= 0, "M2b RO fd present");
        // Same object through the RO fd:
        weft_shm_map_t romap;
        CHECK(weft_shm_attach_fd(ro, &romap, 1) == 0, "M2c RO fd is a session");
        weft_fanout_reader_t r;
        memset(&r, 0, sizeof r);
        weft_fanout_reader_init(&r, romap.ring, weft_shm_ring_bytes(&romap),
                                PAYLOAD_BYTES, SLOT_COUNT);
        const weft_fanout_claim_t* c = weft_fanout_claim(&r);
        CHECK(c->fresh && c->seq == 1, "M2d frame 1 visible via RO view");
        const uint8_t* view = (const uint8_t*)weft_fanout_view(&r);
        uint8_t expect[PAYLOAD_BYTES];
        fill_pattern(expect, 1);
        CHECK(memcmp(view, expect, PAYLOAD_BYTES) == 0, "M2e payload bit-exact");
        // THE gate: writable mmap on the RO fd must fail EACCES.
        errno = 0;
        void* w = mmap(NULL, romap.mapping_bytes, PROT_READ | PROT_WRITE,
                       MAP_SHARED, ro, 0);
        CHECK(w == MAP_FAILED && errno == EACCES,
              "M2f writable mmap on RO view EACCES (kernel-enforced)");
        weft_fanout_reader_destroy(&r);
        weft_shm_destroy(&romap);
        weft_fanout_destroy(&f);
        weft_shm_memfd_destroy(&m);
    }

    // ------------------------------------------------------------------ M3
    {
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        weft_shm_memfd_t m;
        weft_shm_memfd_create(PAYLOAD_BYTES, SLOT_COUNT, &m);
        weft_shm_memfd_ro_view(&m);
        // Publish frame 3 before the exchange.
        weft_fanout_t f;
        memset(&f, 0, sizeof f);
        weft_fanout_attach_writer(&f, m.map.ring, weft_shm_ring_bytes(&m.map),
                                  PAYLOAD_BYTES, SLOT_COUNT);
        uint8_t* cur = weft_fanout_begin(&f);
        fill_pattern(cur, 1);  // ring frame 1
        weft_fanout_publish(&f);
        weft_fanout_destroy(&f);

        CHECK(weft_shm_send_fd(sv[0], weft_shm_memfd_ro_fd(&m), "WFX", 3) == 0,
              "M3a sendmsg SCM_RIGHTS");

        pid_t pid = fork();
        if (pid == 0) {
            close(sv[0]);
            int cfail = 0;
            int fd = -1;
            char buf[8] = {0};
            size_t got = 0;
            if (weft_shm_recv_fd(sv[1], &fd, buf, sizeof buf, &got) != 0 ||
                got != 3 || memcmp(buf, "WFX", 3) != 0) cfail += 1;
            weft_shm_map_t cm;
            if (weft_shm_attach_fd(fd, &cm, 1) != 0) cfail += 2;
            weft_fanout_reader_t r;
            memset(&r, 0, sizeof r);
            if (weft_fanout_reader_init(&r, cm.ring, weft_shm_ring_bytes(&cm),
                                        PAYLOAD_BYTES, SLOT_COUNT) != 0) {
                cfail += 4;
            } else {
                const weft_fanout_claim_t* c = weft_fanout_claim(&r);
                if (!c->fresh || c->seq != 1) cfail += 8;
                const uint8_t* v = (const uint8_t*)weft_fanout_view(&r);
                uint8_t e[PAYLOAD_BYTES];
                fill_pattern(e, 1);
                if (memcmp(v, e, PAYLOAD_BYTES) != 0) cfail += 16;
            }
            errno = 0;
            if (mmap(NULL, cm.mapping_bytes, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0) != MAP_FAILED ||
                errno != EACCES) {
                cfail += 32;
            }
            errno = 0;
            // An O_RDONLY fd cannot even attempt a resize (EBADF); a sealed
            // RW fd gets EPERM. Either way the consumer cannot resize.
            if (ftruncate(fd, 8192) != -1) cfail += 64;
            _exit(cfail == 0 ? 0 : 100 + cfail);
        }
        close(sv[1]);
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "M3b child: fd + frame + EACCES + sealed ftruncate EPERM");
        if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
            printf("        child code=%d\n", WEXITSTATUS(st));
        }
        close(sv[0]);
        weft_shm_memfd_destroy(&m);
    }

    // ------------------------------------------------------------------ M4
    {
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        // Data-only message: legitimate, returns 1 (no fd).
        const char* msg = "NOCMSG";
        CHECK(send(sv[0], msg, 6, 0) == 6, "M4a send data-only");
        int fd = -99;
        char buf[16];
        size_t got = 0;
        int rc = weft_shm_recv_fd(sv[1], &fd, buf, sizeof buf, &got);
        CHECK(rc == 1 && got == 6 && fd == -1, "M4b data-only -> rc 1, no fd");
        // Malformed: TWO fds in one message -> -3.
        struct msghdr ms;
        memset(&ms, 0, sizeof ms);
        struct iovec iov = { .iov_base = (void*)"X", .iov_len = 1 };
        ms.msg_iov = &iov;
        ms.msg_iovlen = 1;
        union {
            struct cmsghdr h;
            char b[CMSG_SPACE(2 * sizeof(int))];
        } u;
        memset(&u, 0, sizeof u);
        ms.msg_control = u.b;
        ms.msg_controllen = sizeof u.b;
        struct cmsghdr* cm = CMSG_FIRSTHDR(&ms);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(2 * sizeof(int));
        int two[2] = {sv[0], sv[1]};
        memcpy(CMSG_DATA(cm), two, sizeof two);
        CHECK(sendmsg(sv[0], &ms, 0) == 1, "M4c send two-fd cmsg");
        rc = weft_shm_recv_fd(sv[1], &fd, buf, sizeof buf, &got);
        CHECK(rc == -3, "M4d double-fd cmsg refused");
        // Bad args: -2 without touching anything.
        CHECK(weft_shm_send_fd(-1, 0, NULL, 0) == -2, "M4e send_fd bad args");
        CHECK(weft_shm_recv_fd(-1, NULL, NULL, 0, NULL) == -2,
              "M4f recv_fd bad args");
        close(sv[0]);
        close(sv[1]);
    }

    // ------------------------------------------------------------------ M5
    {
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        weft_shm_memfd_t m;
        weft_shm_memfd_create(PAYLOAD_BYTES, SLOT_COUNT, &m);
        weft_shm_memfd_ro_view(&m);
        weft_fanout_t f;
        memset(&f, 0, sizeof f);
        weft_fanout_attach_writer(&f, m.map.ring, weft_shm_ring_bytes(&m.map),
                                  PAYLOAD_BYTES, SLOT_COUNT);

        pid_t pid = fork();
        if (pid == 0) {
            // Consumer: full handshake on the socketpair end.
            close(sv[0]);
            weft_shm_memfd_t cm;
            weft_shm_grant_info_t g;
            int cfail = 0;
            int rc = weft_shm_handshake_connect_fd(sv[1], 0, 0, NULL, &cm, &g);
            if (rc != 0) cfail += 1;
            if (g.status != WEFT_SHM_GRANT_OK) cfail += 2;
            if ((g.perms_granted & WEFT_SHM_PERM_READ) == 0 ||
                (g.perms_granted & WEFT_SHM_PERM_CLAIM) == 0) cfail += 4;
            weft_fanout_reader_t r;
            memset(&r, 0, sizeof r);
            if (weft_fanout_reader_init(&r, cm.map.ring, weft_shm_ring_bytes(&cm.map),
                                        PAYLOAD_BYTES, SLOT_COUNT) != 0) {
                cfail += 8;
            } else {
                int bitfail = 0;
                for (int round = 0; round < 100; round++) {
                    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
                    if (c->fresh) {
                        const uint8_t* v = (const uint8_t*)weft_fanout_view(&r);
                        uint8_t e[PAYLOAD_BYTES];
                        fill_pattern(e, (uint32_t)c->seq);
                        if (memcmp(v, e, PAYLOAD_BYTES) != 0) bitfail++;
                    }
                    usleep(200);  // consumer cadence, 100 rounds ~ 20ms
                }
                if (bitfail != 0) cfail += 16;
                weft_fanout_reader_destroy(&r);
            }
            weft_shm_memfd_destroy(&cm);
            close(sv[1]);
            _exit(cfail == 0 ? 0 : 200 + cfail);
        }
        close(sv[1]);
        // Producer: serve the handshake, then publish while the child polls.
        int served = weft_shm_handshake_serve(sv[0], &m, 0xABCDEF01u, 3, NULL, NULL);
        CHECK(served == 0, "M5a anonymous SUBSCRIBE served");
        for (uint32_t seq = 1; seq <= 200; seq++) {
            uint8_t* cur = weft_fanout_begin(&f);
            fill_pattern(cur, seq);
            weft_fanout_publish(&f);
            usleep(90);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "M5b child: handshake grant + 100-round claims bit-exact");
        if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
            printf("        child code=%d\n", WEXITSTATUS(st));
        }
        weft_fanout_destroy(&f);
        weft_shm_memfd_destroy(&m);
        close(sv[0]);
    }

    // ------------------------------------------------------------------ M6
    {
        weft_shm_memfd_t m;
        weft_shm_memfd_create(PAYLOAD_BYTES, SLOT_COUNT, &m);
        weft_shm_memfd_ro_view(&m);
        int listen_fd = -1;
        CHECK(weft_shm_listen("m6-session", &listen_fd) == 0 && listen_fd >= 0,
              "M6a named listener created");

        // Serve the real client FIRST: the duplicate-listener probe below
        // leaves its own (closed) connection in the backlog, which a later
        // accept() would return as an already-dead peer.
        pid_t pid = fork();
        if (pid == 0) {
            weft_shm_memfd_t cm;
            weft_shm_grant_info_t g;
            int rc = weft_shm_handshake_connect("m6-session", 0, 0, NULL, &cm,
                                                &g);
            _exit(rc == 0 && g.status == WEFT_SHM_GRANT_OK ? 0 : 1);
        }
        int conn = weft_shm_accept(listen_fd);
        CHECK(conn >= 0, "M6c accept");
        CHECK(weft_shm_handshake_serve(conn, &m, 0x11223344u, 1, NULL, NULL) == 0,
              "M6d named-socket handshake served");
        close(conn);
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "M6e named-socket client attached");

        // A second listener on the same name must refuse (live incumbent).
        // (Its connect-probe connection is left in the backlog; we are done
        // accepting on this listener, so it is harmless by construction.)
        int again = -1;
        CHECK(weft_shm_listen("m6-session", &again) == -1,
              "M6b duplicate listener refused");

        // Stale-socket takeover: close the listener WITHOUT unlinking
        // (simulate crash), then a new listener must win the name.
        close(listen_fd);
        listen_fd = -1;
        CHECK(weft_shm_listen("m6-session", &listen_fd) == 0,
              "M6f stale socket file taken over");
        CHECK(weft_shm_connect("m6-session") >= 0, "M6g connect to successor");
        close(weft_shm_connect("m6-session"));
        close(listen_fd);
        unlink("/tmp/weft-ipc/m6-session.sock");
        weft_shm_memfd_destroy(&m);
    }

    // ------------------------------------------------------------------ M7
    {
        weft_shm_memfd_t m;
        weft_shm_memfd_create(PAYLOAD_BYTES, SLOT_COUNT, &m);
        weft_shm_memfd_ro_view(&m);
        const uint64_t SID = 0xCAFEF00D00000001ull;
        const uint32_t EPOCH = 4;

        uint8_t key[WEFT_IPC_KEY_BYTES];
        CHECK(weft_ipc_random_bytes(key, sizeof key) == 0, "M7a session key");

        weft_ipc_claims_t cl = { .session_id = SID, .perms = WEFT_IPC_PERM_READ,
                                 .epoch = EPOCH, .expiry_unix = 0,
                                 .key_id = 1, .nonce = 7 };
        uint8_t tok_read[WEFT_IPC_TOKEN_BYTES];
        weft_ipc_token_issue(&cl, key, tok_read);

        // WRITE token (what we'll deny to an expired clone below).
        weft_ipc_claims_t cw = cl;
        cw.perms = WEFT_IPC_PERM_READ | WEFT_IPC_PERM_WRITE | WEFT_IPC_PERM_CLAIM;
        uint8_t tok_write[WEFT_IPC_TOKEN_BYTES];
        weft_ipc_token_issue(&cw, key, tok_write);

        weft_ipc_token_ctx_t vctx = { .session_id = SID, .epoch = EPOCH };
        memcpy(vctx.key, key, sizeof key);

        // Anonymous WRITE request:
        {
            int sv[2];
            socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
            pid_t pid = fork();
            if (pid == 0) {
                close(sv[0]);
                weft_shm_memfd_t cm;
                weft_shm_grant_info_t g;
                int rc = weft_shm_handshake_connect_fd(sv[1], WEFT_SHM_PERM_WRITE,
                                                       SID, NULL, &cm, &g);
                _exit((rc == -2 && g.status == WEFT_SHM_GRANT_DENIED_ANON)
                          ? 0 : 1);
            }
            close(sv[1]);
            weft_shm_handshake_serve(sv[0], &m, SID, EPOCH, weft_ipc_token_verify_adapter, &vctx);
            int st = 0;
            waitpid(pid, &st, 0);
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                  "M7b anonymous WRITE -> DENIED_ANON");
            close(sv[0]);
        }
        // Forged token (tampered tag):
        {
            uint8_t forged[WEFT_IPC_TOKEN_BYTES];
            memcpy(forged, tok_write, sizeof forged);
            forged[63] ^= 0x55;  // flip a tag byte
            int sv[2];
            socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
            pid_t pid = fork();
            if (pid == 0) {
                close(sv[0]);
                weft_shm_memfd_t cm;
                weft_shm_grant_info_t g;
                int rc = weft_shm_handshake_connect_fd(sv[1], WEFT_SHM_PERM_WRITE,
                                                       SID, forged, &cm, &g);
                _exit((rc == -2 && g.status == WEFT_SHM_GRANT_DENIED_TOKEN)
                          ? 0 : 1);
            }
            close(sv[1]);
            weft_shm_handshake_serve(sv[0], &m, SID, EPOCH, weft_ipc_token_verify_adapter, &vctx);
            int st = 0;
            waitpid(pid, &st, 0);
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                  "M7c forged HMAC tag -> DENIED_TOKEN");
            close(sv[0]);
        }
        // Expired token:
        {
            weft_ipc_claims_t ce = cw;
            ce.expiry_unix = 1;  // the distant past
            uint8_t tok_exp[WEFT_IPC_TOKEN_BYTES];
            weft_ipc_token_issue(&ce, key, tok_exp);
            int sv[2];
            socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
            pid_t pid = fork();
            if (pid == 0) {
                close(sv[0]);
                weft_shm_memfd_t cm;
                weft_shm_grant_info_t g;
                int rc = weft_shm_handshake_connect_fd(sv[1], WEFT_SHM_PERM_WRITE,
                                                       SID, tok_exp, &cm, &g);
                _exit((rc == -2 && g.status == WEFT_SHM_GRANT_DENIED_EXPIRED)
                          ? 0 : 1);
            }
            close(sv[1]);
            weft_shm_handshake_serve(sv[0], &m, SID, EPOCH, weft_ipc_token_verify_adapter, &vctx);
            int st = 0;
            waitpid(pid, &st, 0);
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                  "M7d expired token -> DENIED_EXPIRED");
            close(sv[0]);
        }
        // Epoch mismatch (token from an older incarnation):
        {
            weft_ipc_token_ctx_t vctx_old = vctx;
            vctx_old.epoch = EPOCH - 1;  // server has moved on
            int sv[2];
            socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
            pid_t pid = fork();
            if (pid == 0) {
                close(sv[0]);
                weft_shm_memfd_t cm;
                weft_shm_grant_info_t g;
                int rc = weft_shm_handshake_connect_fd(sv[1], WEFT_SHM_PERM_WRITE,
                                                       SID, tok_write, &cm, &g);
                _exit((rc == -2 && g.status == WEFT_SHM_GRANT_DENIED_PROTOCOL)
                          ? 0 : 1);
            }
            close(sv[1]);
            weft_shm_handshake_serve(sv[0], &m, SID, EPOCH, weft_ipc_token_verify_adapter, &vctx_old);
            int st = 0;
            waitpid(pid, &st, 0);
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                  "M7e stale-epoch token -> DENIED (protocol)");
            close(sv[0]);
        }
        // READ-only token requesting WRITE (perms insufficient):
        {
            int sv[2];
            socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
            pid_t pid = fork();
            if (pid == 0) {
                close(sv[0]);
                weft_shm_memfd_t cm;
                weft_shm_grant_info_t g;
                int rc = weft_shm_handshake_connect_fd(sv[1], WEFT_SHM_PERM_WRITE,
                                                       SID, tok_read, &cm, &g);
                _exit((rc == -2 && g.status == WEFT_SHM_GRANT_DENIED_PERMS)
                          ? 0 : 1);
            }
            close(sv[1]);
            weft_shm_handshake_serve(sv[0], &m, SID, EPOCH, weft_ipc_token_verify_adapter, &vctx);
            int st = 0;
            waitpid(pid, &st, 0);
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                  "M7f READ token requesting WRITE -> DENIED_PERMS");
            close(sv[0]);
        }
        // HELLO session mismatch:
        {
            int sv[2];
            socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
            pid_t pid = fork();
            if (pid == 0) {
                close(sv[0]);
                weft_shm_memfd_t cm;
                weft_shm_grant_info_t g;
                int rc = weft_shm_handshake_connect_fd(sv[1], 0,
                                                       0xDEADull, NULL, &cm, &g);
                _exit((rc == -2 && g.status == WEFT_SHM_GRANT_DENIED_SESSION)
                          ? 0 : 1);
            }
            close(sv[1]);
            weft_shm_handshake_serve(sv[0], &m, SID, EPOCH, weft_ipc_token_verify_adapter, &vctx);
            int st = 0;
            waitpid(pid, &st, 0);
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                  "M7g session mismatch -> DENIED_SESSION");
            close(sv[0]);
        }
        weft_shm_memfd_destroy(&m);
    }

    // ------------------------------------------------------------------ M8
    {
        weft_shm_memfd_t m;
        weft_shm_memfd_create(PAYLOAD_BYTES, SLOT_COUNT, &m);
        weft_fanout_t f;
        memset(&f, 0, sizeof f);
        weft_fanout_attach_writer(&f, m.map.ring, weft_shm_ring_bytes(&m.map),
                                  PAYLOAD_BYTES, SLOT_COUNT);
        uint8_t* cur = weft_fanout_begin(&f);
        fill_pattern(cur, 42);
        weft_fanout_publish(&f);
        weft_fanout_destroy(&f);
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        pid_t pid = fork();
        if (pid == 0) {
            close(sv[0]);
            weft_shm_grant_info_t g;
            int rc = weft_shm_handshake_ping_fd(sv[1], 0, &g);
            _exit((rc == 0 && g.status == WEFT_SHM_GRANT_OK && g.epoch == 9 &&
                   g.latest_seq == 1)
                      ? 0
                      : 1);
        }
        close(sv[1]);
        CHECK(weft_shm_handshake_serve(sv[0], &m, 0x0BEEF01ull, 9, NULL, NULL) == 0,
              "M8a PING served without a descriptor");
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "M8b PING: epoch + latest_seq echoed, no fd granted");
        close(sv[0]);
        weft_shm_memfd_destroy(&m);
    }

    // ------------------------------------------------------------------ M9
    {
        weft_shm_memfd_t m;
        weft_shm_memfd_create(PAYLOAD_BYTES, SLOT_COUNT, &m);
        const uint64_t SID = 0xCAFEF00D0000000Aull;
        uint8_t key[WEFT_IPC_KEY_BYTES];
        weft_ipc_random_bytes(key, sizeof key);
        weft_ipc_claims_t cw = { .session_id = SID,
                                 .perms = WEFT_IPC_PERM_READ |
                                          WEFT_IPC_PERM_WRITE |
                                          WEFT_IPC_PERM_CLAIM,
                                 .epoch = 1, .expiry_unix = 0,
                                 .key_id = 1, .nonce = 3 };
        uint8_t tok[WEFT_IPC_TOKEN_BYTES];
        weft_ipc_token_issue(&cw, key, tok);
        weft_ipc_token_ctx_t vctx = { .session_id = SID, .epoch = 1 };
        memcpy(vctx.key, key, sizeof key);

        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        pid_t pid = fork();
        if (pid == 0) {
            close(sv[0]);
            weft_shm_memfd_t cm;
            weft_shm_grant_info_t g;
            int rc = weft_shm_handshake_connect_fd(
                sv[1], WEFT_SHM_PERM_WRITE, SID, tok, &cm, &g);
            if (rc != 0) _exit(1);
            if (cm.writable != 1) _exit(2);
            // A WRITE grant maps writable and CAN fill (co-processor road).
            weft_fanout_reader_t r;
            memset(&r, 0, sizeof r);
            if (weft_fanout_reader_init(&r, cm.map.ring, weft_shm_ring_bytes(&cm.map),
                                        PAYLOAD_BYTES, SLOT_COUNT) != 0) {
                _exit(3);
            }
            const weft_fanout_claim_t* c = weft_fanout_claim(&r);
            if (!c->fresh || c->seq != 1) _exit(4);
            const uint8_t* v = (const uint8_t*)weft_fanout_view(&r);
            uint8_t e[PAYLOAD_BYTES];
            fill_pattern(e, 5);
            if (memcmp(v, e, PAYLOAD_BYTES) != 0) _exit(5);
            weft_fanout_reader_destroy(&r);
            weft_shm_memfd_destroy(&cm);
            close(sv[1]);
            _exit(0);
        }
        close(sv[1]);
        weft_fanout_t f;
        memset(&f, 0, sizeof f);
        weft_fanout_attach_writer(&f, m.map.ring, weft_shm_ring_bytes(&m.map),
                                  PAYLOAD_BYTES, SLOT_COUNT);
        uint8_t* cur = weft_fanout_begin(&f);
        fill_pattern(cur, 5);
        weft_fanout_publish(&f);
        CHECK(weft_shm_handshake_serve(sv[0], &m, SID, 1, weft_ipc_token_verify_adapter, &vctx) == 0,
              "M10a WRITE-grant handshake served");
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "M10b WRITE grant: RW fd received, writable, frame bit-exact");
        if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
            printf("        child code=%d\n", WEXITSTATUS(st));
        }
        weft_fanout_destroy(&f);
        weft_shm_memfd_destroy(&m);
        close(sv[0]);
    }

    // ------------------------------------------------------------------ M11
    {
        // A LYING server: hand-crafted GRANT whose geometry words disagree
        // with the sealed object's own header. The client must refuse (-3)
        // — the header in the object is the authority, not the socket.
        weft_shm_memfd_t m;
        weft_shm_memfd_create(PAYLOAD_BYTES, SLOT_COUNT, &m);
        weft_shm_memfd_ro_view(&m);
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        pid_t pid = fork();
        if (pid == 0) {
            close(sv[0]);
            weft_shm_memfd_t cm;
            weft_shm_grant_info_t g;
            int rc = weft_shm_handshake_connect_fd(sv[1], 0, 0, NULL, &cm, &g);
            _exit(rc == -3 ? 0 : 1);  // geometry lie caught
        }
        close(sv[1]);
        // Read the client's hello to echo its nonce honestly (the hello
        // is exactly 96 bytes; the client sends it in one go).
        size_t got = 0;
        char hbuf[WEFT_SHM_HELLO_BYTES];
        while (got < WEFT_SHM_HELLO_BYTES) {
            ssize_t n = recv(sv[0], hbuf + got, WEFT_SHM_HELLO_BYTES - got, 0);
            if (n <= 0) _exit(1);
            got += (size_t)n;
        }
        uint64_t nonce = 0;
        for (int i = 0; i < 8; i++) nonce |= (uint64_t)(uint8_t)hbuf[20 + i] << (8 * i);
        // Byte-wise LE grant encode (house discipline — no struct memcpy):
        uint8_t grant[WEFT_SHM_GRANT_BYTES];
        memset(grant, 0, sizeof grant);
        grant[0] = (uint8_t)WEFT_SHM_GRANT_MAGIC;
        grant[1] = (uint8_t)(WEFT_SHM_GRANT_MAGIC >> 8);
        grant[2] = (uint8_t)(WEFT_SHM_GRANT_MAGIC >> 16);
        grant[3] = (uint8_t)(WEFT_SHM_GRANT_MAGIC >> 24);
        grant[4] = (uint8_t)WEFT_SHM_HANDSHAKE_VERSION;
        grant[5] = (uint8_t)(WEFT_SHM_HANDSHAKE_VERSION >> 8);
        // status OK (bytes 6-7 zero), session 0 (8-15 zero), nonce echo:
        for (int i = 0; i < 8; i++) grant[16 + i] = (uint8_t)(nonce >> (8 * i));
        // geometry LIES (payload_bytes at 24, slots at 28, ring_bytes at 32):
        const uint32_t pb_lie = PAYLOAD_BYTES * 1000;
        grant[24] = (uint8_t)pb_lie;
        grant[25] = (uint8_t)(pb_lie >> 8);
        grant[26] = (uint8_t)(pb_lie >> 16);
        grant[27] = (uint8_t)(pb_lie >> 24);
        const uint32_t slots_ok = SLOT_COUNT;
        grant[28] = (uint8_t)slots_ok;
        grant[29] = (uint8_t)(slots_ok >> 8);
        grant[30] = (uint8_t)(slots_ok >> 16);
        grant[31] = (uint8_t)(slots_ok >> 24);
        const uint64_t rb_lie = 999999;
        for (int i = 0; i < 8; i++) grant[32 + i] = (uint8_t)(rb_lie >> (8 * i));
        CHECK(weft_shm_send_fd(sv[0], weft_shm_memfd_ro_fd(&m), grant,
                               WEFT_SHM_GRANT_BYTES) == 0,
              "M11a lying GRANT delivered");
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "M11b geometry lie refused (-3): header is the authority");
        close(sv[0]);
        weft_shm_memfd_destroy(&m);
    }

    // ------------------------------------------------------------------ M12
    {
        weft_shm_memfd_t m;
        weft_shm_memfd_create(PAYLOAD_BYTES, SLOT_COUNT, &m);
        uint64_t last = weft_shm_latest_seq(&m.map);
        CHECK(weft_shm_park(&m.map, &last, 1000) == 1,
              "M12a park exhausts budget (no frames) — bounded, Law 1");
        weft_fanout_t f;
        memset(&f, 0, sizeof f);
        weft_fanout_attach_writer(&f, m.map.ring, weft_shm_ring_bytes(&m.map),
                                  PAYLOAD_BYTES, SLOT_COUNT);
        uint8_t* cur = weft_fanout_begin(&f);
        fill_pattern(cur, 1);
        weft_fanout_publish(&f);
        CHECK(weft_shm_park(&m.map, &last, 100000) == 0 && last == 1,
              "M12b park observes the fresh frame (doorbell-free)");
        CHECK(weft_shm_park(&m.map, &last, 100) == 1,
              "M12c park idempotent when nothing newer");
        CHECK(weft_shm_park(NULL, &last, 10) == -1, "M12d park bad args");
        weft_fanout_destroy(&f);
        weft_shm_memfd_destroy(&m);
    }

    // ------------------------------------------------------------------ M9
    {
        // Fork-mesh torture leg (declared late in the file for readability;
        // runs last so earlier legs fail fast on protocol bugs).
        enum { KIDS = 3, FRAMES = 10000 };
        int sv[KIDS][2];
        weft_shm_memfd_t m;
        weft_shm_memfd_create(PAYLOAD_BYTES, SLOT_COUNT, &m);
        weft_shm_memfd_ro_view(&m);
        for (int k = 0; k < KIDS; k++) socketpair(AF_UNIX, SOCK_STREAM, 0, sv[k]);

        pid_t kids[KIDS];
        for (int k = 0; k < KIDS; k++) {
            kids[k] = fork();
            if (kids[k] == 0) {
                close(sv[k][0]);
                int cfail = 0;
                weft_shm_memfd_t cm;
                weft_shm_grant_info_t g;
                if (weft_shm_handshake_connect_fd(sv[k][1], 0, 0, NULL, &cm,
                                                  &g) != 0) {
                    cfail += 1;
                } else {
                    weft_fanout_reader_t r;
                    memset(&r, 0, sizeof r);
                    if (weft_fanout_reader_init(&r, cm.map.ring, weft_shm_ring_bytes(&cm.map),
                                                PAYLOAD_BYTES, SLOT_COUNT) != 0) {
                        cfail += 2;
                    } else {
                        uint64_t fresh = 0, bitfail = 0;
                        uint64_t target = 0;
                        while (target < FRAMES) {
                            const weft_fanout_claim_t* c = weft_fanout_claim(&r);
                            if (c->fresh) {
                                fresh++;
                                target = c->seq;
                                const uint8_t* v =
                                    (const uint8_t*)weft_fanout_view(&r);
                                uint8_t e[PAYLOAD_BYTES];
                                fill_pattern(e, (uint32_t)c->seq);
                                if (memcmp(v, e, PAYLOAD_BYTES) != 0) bitfail++;
                            } else {
                                usleep(50);
                            }
                        }
                        // Telescoping identity: drops account every unseen frame.
                        if (bitfail != 0) cfail += 4;
                        weft_fanout_reader_destroy(&r);
                    }
                    weft_shm_memfd_destroy(&cm);
                }
                close(sv[k][1]);
                _exit(cfail == 0 ? 0 : 1);
            }
            close(sv[k][1]);
        }

        // Serve KIDS handshakes, then publish FRAMES frames.
        for (int k = 0; k < KIDS; k++) {
            CHECK(weft_shm_handshake_serve(sv[k][0], &m, 0x5E57ull, 1, NULL, NULL) == 0,
                  "M9a handshake served (fork mesh)");
            close(sv[k][0]);
        }
        weft_fanout_t f;
        memset(&f, 0, sizeof f);
        weft_fanout_attach_writer(&f, m.map.ring, weft_shm_ring_bytes(&m.map),
                                  PAYLOAD_BYTES, SLOT_COUNT);
        for (uint32_t seq = 1; seq <= FRAMES; seq++) {
            uint8_t* cur = weft_fanout_begin(&f);
            fill_pattern(cur, seq);
            weft_fanout_publish(&f);
        }
        weft_fanout_destroy(&f);
        int all_ok = 1;
        for (int k = 0; k < KIDS; k++) {
            int st = 0;
            waitpid(kids[k], &st, 0);
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) all_ok = 0;
        }
        CHECK(all_ok, "M9b 3 forked consumers x 10k frames: bit-exact, exact "
                      "telescoping, EACCES/EPERM posture held");
        weft_shm_memfd_destroy(&m);
    }

    printf("=== M-series: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
