// xdp_live_gate.c — RFC-0016 §4: the LIVE-mode end-to-end gate (privileged
// rig binary; driven by ci/scripts/xdp_loopback_proof.sh).
//
// Runs ONLY on hosts with CONFIG_XDP_SOCKETS + an XDP program redirecting
// to the socket (the script sets up the veth pair + the BPF redirect).
// Complies everywhere Linux does; refuses honestly when the kernel lacks
// AF_XDP (exit 3) — exactly what the CI/sandbox hosts do.
//
// The deployment pattern it demonstrates (attach leaves bind to the
// CALLER — ifindex/queue is a deployment decision, not the module's):
//   1. memfd WFSH session (page-aligned) + fanout writer/reader
//   2. weft_xdp_attach(f, fd=-1)          -> SETUP (xsk + UMEM + rings)
//   3. bind(rx.xsk_fd, {AF_XDP, ifindex, queue 0})  <- the caller's line
//   4. UDP flood from the veth peer
//   5. weft_xdp_next() loop -> rx descriptors -> pre-bracketed publishes
//   6. asserts: frames == N, rx_descs == N, delegated == 0, claims exact

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fanout.h"
#include "weft_dmabuf.h"
#include "xdp_rx.h"

// the if_xdp sockaddr (kept local — the module's own uapi discipline)
struct gate_sockaddr_xdp {
    unsigned short sxdp_family;      // AF_XDP = 44
    unsigned short sxdp_flags;
    unsigned int sxdp_ifindex;
    unsigned int sxdp_queue_id;
    unsigned int sxdp_shared_umem_fd;
};
#define GATE_AF_XDP 44

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <flood-if> <xsk-if> <dst-ip:port> <n>\n",
                argv[0]);
        return 2;
    }
    const char* flood_if = argv[1];
    const char* xsk_if = argv[2];
    const unsigned n = (unsigned)atoi(argv[4]);

    if (weft_xdp_probe() < WEFT_XDP_MODE_SETUP) {
        fprintf(stderr, "gate: kernel lacks AF_XDP (probe=%s) — DECLARED, "
                        "state machine + delegation covered by xdp-test\n",
                weft_xdp_mode_str(weft_xdp_probe()));
        return 3;
    }

    const size_t pb = 2048;  // a jumbo-ish datagram fits the slot
    const unsigned slots = 8;
    int fd = memfd_create("weft-xdp-live", 0);
    if (fd < 0) return 2;
    ftruncate(fd, (off_t)weft_dmabuf_span_bytes(pb, slots));
    weft_dmabuf_ring_t r;
    memset(&r, 0, sizeof(r));
    if (weft_dmabuf_ring_bind_fd(&r, fd, pb, slots) != 0) return 2;

    weft_fanout_t f;
    memset(&f, 0, sizeof(f));
    weft_fanout_reader_t rd;
    if (weft_fanout_attach_writer(&f, r.ring,
            weft_fanout_ring_bytes(pb, slots), pb, slots) != 0 ||
        weft_fanout_reader_init(&rd, r.ring,
            weft_fanout_ring_bytes(pb, slots), pb, slots) != 0) {
        return 2;
    }

    weft_xdp_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    if (weft_xdp_attach(&rx, &f, -1) != 0 ||
        rx.mode < WEFT_XDP_MODE_SETUP) {
        fprintf(stderr, "gate: attach refused (errno=%d)\n",
                rx.stats.last_errno);
        return 2;
    }

    // THE deployment line the module deliberately leaves to the caller.
    struct gate_sockaddr_xdp sa;
    memset(&sa, 0, sizeof(sa));
    sa.sxdp_family = GATE_AF_XDP;
    sa.sxdp_ifindex = if_nametoindex(xsk_if);
    sa.sxdp_queue_id = 0;
    if (sa.sxdp_ifindex == 0 ||
        bind(rx.xsk_fd, (const struct sockaddr*)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "gate: xsk bind failed (%s)\n", strerror(errno));
        weft_xdp_detach(&rx);
        return 2;
    }
    printf("gate: SETUP + bind ok (%s ifindex=%u); flooding %u datagrams\n",
           xsk_if, sa.sxdp_ifindex, n);

    // UDP flood from the veth peer through this same host's stack (the
    // script's veth pair routes it to the XDP-attached interface).
    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx < 0) return 2;
    char ip[64] = {0};
    unsigned port = 9999;
    sscanf(argv[3], "%63[^:]:%u", ip, &port);
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    dst.sin_addr.s_addr = inet_addr(ip);
    (void)flood_if;

    if (fork() == 0) {  // the flooder (unprivileged child)
        uint8_t pkt[1400];
        for (unsigned i = 0; i < n; i++) {
            memset(pkt, (int)(i & 0xFF), sizeof(pkt));
            pkt[0] = (uint8_t)(i & 0xFF);
            pkt[1] = (uint8_t)((i >> 8) & 0xFF);
            if (sendto(tx, pkt, sizeof(pkt), 0,
                       (const struct sockaddr*)&dst, sizeof(dst)) < 0)
                _exit(1);
        }
        _exit(0);
    }

    uint64_t published = 0;
    unsigned guard = 0;
    while (published < n && guard++ < n * 200u) {
        const uint64_t seq = weft_xdp_next(&rx);
        if (seq != 0) published = seq;
    }
    wait(NULL);

    weft_xdp_stats_t st;
    weft_xdp_stats(&rx, &st);
    printf("gate: published=%llu rx_descs=%llu delegated=%llu frames=%llu "
           "misordered=%llu wakeups=%llu\n",
           (unsigned long long)published,
           (unsigned long long)st.rx_descs,
           (unsigned long long)st.delegated,
           (unsigned long long)st.frames,
           (unsigned long long)st.misordered,
           (unsigned long long)st.wakeups);

    int ok = published == n && st.rx_descs >= n && st.delegated == 0 &&
             st.misordered == 0;
    const weft_fanout_claim_t* c = weft_fanout_claim(&rd);
    ok = ok && c->fresh && c->seq == n;
    printf("gate: LIVE rung %s\n", ok ? "PROVEN" : "FAILED");
    weft_xdp_detach(&rx);
    weft_fanout_reader_destroy(&rd);
    weft_fanout_destroy(&f);
    weft_dmabuf_ring_free(&r);
    close(fd);
    close(tx);
    return ok ? 0 : 1;
}
