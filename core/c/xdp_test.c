// xdp_test.c — X-series conformance gates (RFC-0016 §4: AF_XDP ingestion).
//
// The honest two-leg design (the uring_rx/gpu_ring pattern):
//   * EVERY host runs the state-machine gates: the pre-bracketed fill-ring
//     protocol (begin -> post -> NIC-DMA-shaped write -> publish_desc) is
//     driven over a real page-aligned WFSH session with SYNTHETIC rx
//     descriptors — the exact transitions weft_xdp_next() performs on
//     kernels with CONFIG_XDP_SOCKETS. Misorder, padding, truncation, and
//     telescoping are all proven here, kernel or no kernel.
//   * EVERY Linux host runs the delegation gates: real UDP datagrams
//     through the uring ladder (the never-a-regression road).
//   * Hosts WITH AF_XDP get the SETUP leg live (probe >= setup); the
//     sandbox kernel ships without CONFIG_XDP_SOCKETS (EAFNOSUPPORT) and
//     says so honestly.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fanout.h"
#include "uring_rx.h"
#include "weft_dmabuf.h"
#include "xdp_rx.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, name, fmt, ...)                                        \
    do {                                                                   \
        if (cond) {                                                        \
            printf("  PASS %s\n", name);                                   \
            g_pass++;                                                      \
        } else {                                                           \
            printf("  FAIL %s — " fmt "\n", name, ##__VA_ARGS__);          \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

static void pkt_fill(uint8_t* dst, uint32_t seq, size_t n) {
    for (size_t i = 0; i < n; i++)
        dst[i] = (uint8_t)(0x5Au ^ (seq + (uint32_t)i));
}

int main(void) {
    printf("# X-series: weft_xdp_rx conformance (RFC-0016 s4)\n");

    const size_t pb = 512;
    const unsigned slots = 8;

    // ---- X1: capability probe honesty -----------------------------------
    printf("## X1 capability probe\n");
    {
        char line[160];
        weft_xdp_report(line, sizeof(line));
        printf("  %s\n", line);
        weft_xdp_mode_t m = weft_xdp_probe();
        CHECK(m == WEFT_XDP_MODE_NONE || m == WEFT_XDP_MODE_SETUP,
              "probe reports a real rung", "%d", (int)m);
        if (m == WEFT_XDP_MODE_NONE) {
            printf("  (this kernel ships without CONFIG_XDP_SOCKETS — the "
                   "xsk legs are DECLARED; state machine + delegation run)\n");
        }
        CHECK(strcmp(weft_xdp_mode_str(m), m == WEFT_XDP_MODE_NONE ? "none" : "setup") == 0,
              "mode_str agrees with the probe", "'%s'", weft_xdp_mode_str(m));
    }

    // ---- X2: the state machine (synthetic descriptors, real ring) -------
    printf("## X2 pre-bracketed state machine\n");
    {
        int fd = memfd_create("weft-xdp-sm", 0);
        CHECK(fd >= 0, "session substrate created", "fd=%d", fd);
        ftruncate(fd, (off_t)weft_dmabuf_span_bytes(pb, slots));
        weft_dmabuf_ring_t r;
        memset(&r, 0, sizeof(r));
        CHECK(weft_dmabuf_ring_bind_fd(&r, fd, pb, slots) == 0,
              "page-aligned WFSH session bound", "errno=%d", errno);

        weft_fanout_t f;
        memset(&f, 0, sizeof(f));
        CHECK(weft_fanout_attach_writer(&f, r.ring,
                  weft_fanout_ring_bytes(pb, slots), pb, slots) == 0,
              "writer over the session", "?");
        weft_fanout_reader_t rd;
        CHECK(weft_fanout_reader_init(&rd, r.ring,
                  weft_fanout_ring_bytes(pb, slots), pb, slots) == 0,
              "reader over the session", "?");

        // The SETUP-mode machine without kernel xsk: the public struct
        // carries the same transitions next() drives on capable hosts
        // (fill_prod NULL = the documented injection path).
        weft_xdp_rx_t rx;
        memset(&rx, 0, sizeof(rx));
        rx.xsk_fd = -1;
        rx.in_flight_umem = UINT64_MAX;
        rx.mode = WEFT_XDP_MODE_SETUP;
        rx.fan = &f;
        rx.payload_bytes = pb;
        rx.slot_count = slots;
        rx.session_base = r.base;
        rx.session_span = r.span_bytes;

        // X2.1: begin_fill opens the bracket at slot 0, posts its offset
        const uint64_t off0 = weft_xdp_begin_fill(&rx);
        CHECK(off0 == 64 + 16 + 8 * slots, "slot-0 umem offset matches the "
              "ring layout (64 hdr + ctrl + 0*pb)", "%llu",
              (unsigned long long)off0);
        uint8_t* slot0 = r.ring + 16 + 8 * slots;
        CHECK(f.w_seq == 1 && f.w_slot == 0,
              "bracket opened: seq=1 slot=0", "seq=%llu slot=%u",
              (unsigned long long)f.w_seq, f.w_slot);

        // X2.2: a NIC-shaped DMA (short packet), then publish_desc
        pkt_fill(slot0, 1, 300);
        uint64_t seq = weft_xdp_publish_desc(&rx, off0, 300);
        CHECK(seq == 1, "descriptor closed the bracket: seq 1", "%llu",
              (unsigned long long)seq);
        const weft_fanout_claim_t* c = weft_fanout_claim(&rd);
        CHECK(c->fresh && c->seq == 1, "reader claims frame 1", "%d/%llu",
              c->fresh, (unsigned long long)c->seq);
        uint8_t expect[512];
        memset(expect, 0, sizeof(expect));
        pkt_fill(expect, 1, 300);
        CHECK(memcmp(weft_fanout_view(&rd), expect, pb) == 0,
              "frame 1 bit-exact + tail PADDED to zero (short packet)", "?");

        // X2.3: misordered descriptor refused; bracket stays open
        const uint64_t off1 = weft_xdp_begin_fill(&rx);
        CHECK(off1 == off0 + pb, "slot-1 umem offset advanced by pb", "%llu",
              (unsigned long long)off1);
        CHECK(weft_xdp_publish_desc(&rx, off0, 8) == 0,
              "stale-address descriptor refused", "?");
        weft_xdp_stats_t st;
        weft_xdp_stats(&rx, &st);
        CHECK(st.misordered == 1, "misorder counted", "%llu",
              (unsigned long long)st.misordered);

        // X2.4: the correct descriptor closes slot 1
        pkt_fill(r.ring + 16 + 8 * slots + pb, 2, pb);
        CHECK(weft_xdp_publish_desc(&rx, off1, (uint32_t)pb) == 2,
              "slot-1 descriptor published: seq 2", "?");

        // X2.5: a stream of synthetic frames, telescoping exact
        uint64_t last_seen = 0, drops = 0, n_claims = 0;
        int exact = 1;
        for (uint32_t s = 3; s <= 100; s++) {
            const uint64_t off = weft_xdp_begin_fill(&rx);
            uint8_t* cur = r.ring + 16 + 8 * slots +
                           (size_t)((s - 1) % slots) * pb;
            const uint32_t len = (s % 3 == 0) ? 200 : (uint32_t)pb;  // mix pads
            pkt_fill(cur, s, len < pb ? len : pb);
            if (weft_xdp_publish_desc(&rx, off, len) != s) exact = 0;
            if ((s % 7) == 0) {
                const weft_fanout_claim_t* cc = weft_fanout_claim(&rd);
                if (cc->fresh) {
                    if (cc->seq <= last_seen) exact = 0;
                    drops += cc->dropped;
                    last_seen = cc->seq;
                    n_claims++;
                }
            }
        }
        CHECK(exact, "98 synthetic frames published in-order", "%d", exact);
        // The RFC-0004 telescoping identity: sum(dropped) == lastSeq -
        // freshClaims. Total fresh claims = 14 here + frame 1 (claimed in
        // X2.2): 98 - 15 = 83 exactly.
        CHECK(last_seen == 98 && drops == last_seen - n_claims - 1,
              "telescoping exact: drops = lastSeq - freshClaims (98-15)",
              "last=%llu claims=%llu drops=%llu",
              (unsigned long long)last_seen, (unsigned long long)n_claims,
              (unsigned long long)drops);

        // X2.6: the honesty record
        weft_xdp_stats(&rx, &st);
        CHECK(st.frames == 100 && st.publishes == 100,
              "stats: 100 frames, 100 publishes", "%llu/%llu",
              (unsigned long long)st.frames, (unsigned long long)st.publishes);
        CHECK(st.fill_posts == 100, "stats: 100 fill posts (one per bracket)",
              "%llu", (unsigned long long)st.fill_posts);
        CHECK(st.padded > 0 && st.misordered == 1,
              "stats: pads + the misorder recorded", "padded=%llu",
              (unsigned long long)st.padded);
        // truncation accounting
        const uint64_t offT = weft_xdp_begin_fill(&rx);
        pkt_fill(r.ring + 16 + 8 * slots + (size_t)(100 % slots) * pb, 101, pb);
        weft_xdp_publish_desc(&rx, offT, (uint32_t)(pb + 64));
        weft_xdp_stats(&rx, &st);
        CHECK(st.truncated == 1, "stats: oversize packet truncated-counted",
              "%llu", (unsigned long long)st.truncated);

        // X2.7: descriptor with no open bracket
        CHECK(weft_xdp_publish_desc(&rx, off0, 8) == 0,
              "no-bracket descriptor refused", "?");
        weft_xdp_stats(&rx, &st);
        CHECK(st.misordered == 2, "both misorders counted", "%llu",
              (unsigned long long)st.misordered);

        weft_xdp_detach(&rx);
        CHECK(rx.xsk_fd == -1 && rx.mode == WEFT_XDP_MODE_NONE,
              "detach resets the session", "%d", (int)rx.mode);
        weft_fanout_reader_destroy(&rd);
        weft_fanout_destroy(&f);
        weft_dmabuf_ring_free(&r);
        close(fd);
    }

    // ---- X3: the uring delegation (real datagrams, the fallback road) ---
    printf("## X3 uring delegation with real traffic\n");
    {
        int fd = memfd_create("weft-xdp-deleg", 0);
        ftruncate(fd, (off_t)weft_dmabuf_span_bytes(pb, slots));
        weft_dmabuf_ring_t r;
        memset(&r, 0, sizeof(r));
        weft_dmabuf_ring_bind_fd(&r, fd, pb, slots);
        weft_fanout_t f;
        memset(&f, 0, sizeof(f));
        weft_fanout_attach_writer(&f, r.ring,
                  weft_fanout_ring_bytes(pb, slots), pb, slots);
        weft_fanout_reader_t rd;
        weft_fanout_reader_init(&rd, r.ring,
                  weft_fanout_ring_bytes(pb, slots), pb, slots);

        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0,
              "datagram socketpair", "%s", strerror(errno));

        weft_xdp_rx_t rx;
        memset(&rx, 0, sizeof(rx));
        int arc = weft_xdp_attach(&rx, &f, sv[1]);
        if (weft_xdp_probe() == WEFT_XDP_MODE_NONE) {
            CHECK(arc == 0 && rx.mode == WEFT_XDP_MODE_URING,
                  "no AF_XDP kernel: delegated to the uring ladder", "%d/%d",
                  arc, (int)rx.mode);
        } else {
            CHECK(arc == 0, "attached (xsk or delegated)", "%d", arc);
        }

        // a producer thread sends 500 real datagrams
        pid_t pid = fork();
        if (pid == 0) {
            uint8_t pkt[512];
            for (uint32_t s = 1; s <= 500; s++) {
                pkt_fill(pkt, s, 512);
                if (send(sv[0], pkt, 512, 0) < 0) _exit(1);
            }
            _exit(0);
        }
        uint64_t published = 0;
        uint32_t guard = 0;
        while (published < 500 && guard++ < 20000) {
            const uint64_t seq = weft_xdp_next(&rx);
            if (seq != 0) published = seq;
        }
        int st2 = 0;
        waitpid(pid, &st2, 0);
        CHECK(WIFEXITED(st2) && WEXITSTATUS(st2) == 0,
              "producer exited clean", "st=%d", st2);
        CHECK(published == 500, "500 datagrams ingested -> 500 frames",
              "%llu", (unsigned long long)published);

        weft_xdp_stats_t st;
        weft_xdp_stats(&rx, &st);
        if (rx.mode == WEFT_XDP_MODE_URING) {
            CHECK(st.delegated >= 490,
                  "delegation counted the frames", "%llu",
                  (unsigned long long)st.delegated);
        }

        const weft_fanout_claim_t* c = weft_fanout_claim(&rd);
        CHECK(c->fresh && c->seq == 500, "reader claims the last frame",
              "%d/%llu", c->fresh, (unsigned long long)c->seq);
        uint8_t expect[512];
        pkt_fill(expect, 500, 512);
        CHECK(memcmp(weft_fanout_view(&rd), expect, 512) == 0,
              "delegated frame bit-exact", "?");

        weft_xdp_detach(&rx);
        weft_xdp_detach(&rx);  // idempotent
        weft_fanout_reader_destroy(&rd);
        weft_fanout_destroy(&f);
        weft_dmabuf_ring_free(&r);
        close(fd);
        close(sv[0]);
        close(sv[1]);
    }

    // ---- X4: refusal discipline ------------------------------------------
    printf("## X4 refusal discipline\n");
    {
        // no AF_XDP + no fallback fd -> honest refusal
        weft_fanout_t f;
        memset(&f, 0, sizeof(f));
        CHECK(weft_fanout_init(&f, pb, slots) == 0, "malloc'd ring", "?");
        weft_xdp_rx_t rx;
        memset(&rx, 0, sizeof(rx));
        if (weft_xdp_probe() == WEFT_XDP_MODE_NONE) {
            CHECK(weft_xdp_attach(&rx, &f, -1) == -1,
                  "no AF_XDP + no fallback fd: refused", "?");
            CHECK(rx.stats.last_errno == EAFNOSUPPORT,
                  "refusal errno recorded (EAFNOSUPPORT)", "%d",
                  rx.stats.last_errno);
        } else {
            printf("  (AF_XDP present: malloc-ring xsk attempt falls back — "
                   "the delegation below is the same check)\n");
        }
        // malloc'd ring + fallback fd -> DELEGATES (the never-regression road:
        // uring needs no page alignment)
        int sv[2];
        socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
        weft_xdp_rx_t rx2;
        memset(&rx2, 0, sizeof(rx2));
        CHECK(weft_xdp_attach(&rx2, &f, sv[1]) == 0,
              "malloc ring + fd: delegated (not refused)", "?");
        CHECK(rx2.mode == WEFT_XDP_MODE_URING,
              "delegation mode recorded", "%d", (int)rx2.mode);
        weft_xdp_detach(&rx2);
        weft_fanout_destroy(&f);
        close(sv[0]);
        close(sv[1]);

        // NULL discipline
        CHECK(weft_xdp_attach(NULL, NULL, -1) == -1, "NULL refused", "?");
        CHECK(weft_xdp_next(NULL) == 0, "NULL next safe", "?");
        weft_xdp_detach(NULL);
    }

    printf("verdict: %s (%d passed, %d failed)\n",
           g_fail == 0 ? "PASS" : "FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
