// uring_test.c — U-series battery for the RFC 0012 kernel-bypass ingestion
//
//   U1  capability ladder report — sanity bounds + live mode line
//   U2  datagram->frame fidelity: 20k interleaved packets, payload bytes
//       verify per claim, drop accounting telescopes EXACTLY
//   U3  concurrent torture: feeder thread (max rate) + ingestion thread +
//       claiming reader — 100k packets, zero torn frames accepted,
//       telescoping exact, frame count exact
//   U4  size honesty: short datagram zero-fills the slot tail (padded),
//       oversized datagram keeps the head (truncated) — both counted
//   U5  session lifecycle: fd flags restored on detach, re-attach works
//
// The bracket proof (kernel-as-filler) runs identically in SYSCALL and RING
// modes — whichever the host negotiates; the mode line in the output IS the
// evidence of which rung was exercised (Law 4).
//
// Build (see core/c/Makefile): make uring-test
//                               make uring-test-asan
//                               make uring-test-tsan

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include "uring_rx.h"
#include "fanout.h"
#include "weft.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);         \
            g_failures++;                                                   \
        } else {                                                            \
            fprintf(stdout, "ok: %s\n", msg);                               \
        }                                                                    \
    } while (0)

static uint32_t tword(uint32_t seq, uint32_t w) {
    return weft_mix32(seq * 2654435761u + w);
}

enum { PB = 256, M = 8 };

// ---------------------------------------------------------------------------
// U1 — capability ladder
// ---------------------------------------------------------------------------

static void u1_probe(void) {
    char report[256];
    weft_uring_report(report, sizeof(report));
    fputs(report, stdout);
    const weft_uring_mode_t m = weft_uring_probe();
    CHECK(m == WEFT_URING_MODE_SYSCALL || m == WEFT_URING_MODE_RING ||
          m == WEFT_URING_MODE_FIXED || m == WEFT_URING_MODE_NONE,
          "U1 probe returns a declared mode");
    CHECK(m >= WEFT_URING_MODE_SYSCALL,
          "U1 SYSCALL fallback is universal (mode >= syscall)");
    printf("  U1 negotiated rung on this host: %s\n", weft_uring_mode_str(m));
}

// ---------------------------------------------------------------------------
// U2 — datagram->frame fidelity (interleaved batches, single thread)
// ---------------------------------------------------------------------------

static int drained_frames(weft_uring_rx_t* rx, int want) {
    int got = 0;
    while (got < want) {
        if (weft_uring_next(rx) != 0) got++;
    }
    return got;
}

static void u2_fidelity(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "U2 socketpair created");

    weft_fanout_t f;
    CHECK(weft_fanout_init(&f, PB, M) == 0, "U2 fanout init");
    weft_uring_rx_t rx;
    CHECK(weft_uring_attach(&rx, &f, sv[0]) == 0, "U2 uring attach");
    printf("  U2 session mode: %s\n", weft_uring_mode_str(rx.mode));

    weft_fanout_reader_t r;
    CHECK(weft_fanout_reader_init(&r, f.ring, weft_fanout_ring_bytes(PB, M), PB, M) == 0,
          "U2 reader init");

    // Interleave: 2000 batches of 10 sends + 10 drains. send() on sv[1] stays
    // blocking-safe because the consumer drains within each batch.
    uint32_t pkt[PB / 4];
    const uint32_t TOTAL = 20000;
    for (uint32_t b = 0; b < TOTAL / 10; b++) {
        for (int k = 0; k < 10; k++) {
            uint32_t seq = b * 10 + (uint32_t)k + 1;
            for (size_t w = 0; w < PB / 4; w++) pkt[w] = tword(seq, (uint32_t)w);
            CHECK(send(sv[1], pkt, PB, 0) == (ssize_t)PB, "U2 send full datagram");
        }
        (void)drained_frames(&rx, 10);
    }

    weft_uring_stats_t st;
    weft_uring_stats(&rx, &st);
    CHECK(st.frames == TOTAL, "U2 every datagram became exactly one frame");
    CHECK(st.aborted == 0, "U2 zero aborted brackets");
    CHECK(st.padded == 0 && st.truncated == 0, "U2 exact-size datagrams: no pad/trunc");

    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    CHECK(c->fresh && c->seq == TOTAL && c->dropped == TOTAL - 1,
          "U2 latest claim: exact seq and drop accounting");
    const uint32_t* view = (const uint32_t*)weft_fanout_view(&r);
    int ok = 1;
    for (size_t w = 0; w < PB / 4; w++) {
        if (view[w] != tword(TOTAL, (uint32_t)w)) { ok = 0; break; }
    }
    CHECK(ok, "U2 final frame payload byte-verifies (kernel-as-filler intact)");
    weft_fanout_stats_t rs;
    weft_fanout_reader_stats(&r, &rs);
    CHECK(rs.fresh + rs.drops == TOTAL,
          "U2 telescoping EXACT (fresh + drops == lastSeq)");

    weft_fanout_reader_destroy(&r);
    weft_uring_detach(&rx);
    weft_fanout_destroy(&f);
    close(sv[0]); close(sv[1]);
}

// ---------------------------------------------------------------------------
// U3 — concurrent torture
// ---------------------------------------------------------------------------

enum { U3_TOTAL = 100000 };

typedef struct {
    int fd;
    uint32_t total;
    uint64_t sent;
} u3_feeder_t;

static void* u3_feeder_entry(void* arg) {
    u3_feeder_t* fe = (u3_feeder_t*)arg;
    uint32_t pkt[PB / 4];
    for (uint32_t seq = 1; seq <= fe->total; seq++) {
        for (size_t w = 0; w < PB / 4; w++) pkt[w] = tword(seq, (uint32_t)w);
        ssize_t n;
        do {
            n = send(fe->fd, pkt, PB, 0);
        } while (n < 0 && errno == EINTR);
        fe->sent++;
    }
    return NULL;
}

typedef struct {
    weft_uring_rx_t* rx;
    uint32_t total;
} u3_consumer_t;

static void* u3_consumer_entry(void* arg) {
    u3_consumer_t* co = (u3_consumer_t*)arg;
    while (co->rx->stats.frames < (uint64_t)co->total) {
        (void)weft_uring_next(co->rx);
    }
    return NULL;
}

typedef struct {
    weft_fanout_reader_t* r;
    _Atomic int* consumer_done;
    uint64_t torn, fresh, drops, last, reads;
} u3_reader_t;

static void* u3_reader_entry(void* arg) {
    u3_reader_t* ra = (u3_reader_t*)arg;
    for (;;) {
        const weft_fanout_claim_t* c = weft_fanout_claim(ra->r);
        ra->reads++;
        if (c->fresh) {
            ra->fresh++;
            ra->drops += c->dropped;
            ra->last = c->seq;
            // Torn-acceptance detector: every word must match the pattern
            // for the seq the claim record says we hold.
            const uint32_t* view = (const uint32_t*)weft_fanout_view(ra->r);
            for (size_t w = 0; w < PB / 4; w++) {
                if (view[w] != tword((uint32_t)c->seq, (uint32_t)w)) {
                    ra->torn++;
                    break;
                }
            }
        }
        // Exit discriminator (same discipline as the T-series harness): done
        // AND not-fresh AND genuinely caught up (advisory latestSeq read).
        if (atomic_load_explicit(ra->consumer_done, memory_order_acquire) && !c->fresh) {
            const uint64_t latest =
                atomic_load_explicit(ra->r->ctrl, memory_order_acquire);
            if (latest == ra->last) break;
        }
    }
    return NULL;
}

static void u3_torture(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "U3 socketpair created");

    weft_fanout_t f;
    CHECK(weft_fanout_init(&f, PB, M) == 0, "U3 fanout init");
    weft_uring_rx_t rx;
    CHECK(weft_uring_attach(&rx, &f, sv[0]) == 0, "U3 uring attach");
    printf("  U3 session mode: %s\n", weft_uring_mode_str(rx.mode));

    weft_fanout_reader_t r;
    CHECK(weft_fanout_reader_init(&r, f.ring, weft_fanout_ring_bytes(PB, M), PB, M) == 0,
          "U3 reader init");

    _Atomic int consumer_done;
    atomic_init(&consumer_done, 0);

    u3_feeder_t fe = { .fd = sv[1], .total = U3_TOTAL, .sent = 0 };
    u3_consumer_t co = { .rx = &rx, .total = U3_TOTAL };
    u3_reader_t ra = { .r = &r, .consumer_done = &consumer_done,
                       .torn = 0, .fresh = 0, .drops = 0, .last = 0, .reads = 0 };

    pthread_t tfe, tco, trd;
    CHECK(pthread_create(&tfe, NULL, u3_feeder_entry, &fe) == 0, "U3 feeder thread");
    CHECK(pthread_create(&tco, NULL, u3_consumer_entry, &co) == 0, "U3 consumer thread");
    CHECK(pthread_create(&trd, NULL, u3_reader_entry, &ra) == 0, "U3 reader thread");
    pthread_join(tfe, NULL);
    pthread_join(tco, NULL);
    atomic_store(&consumer_done, 1);
    pthread_join(trd, NULL);

    weft_uring_stats_t st;
    weft_uring_stats(&rx, &st);
    CHECK(st.frames == U3_TOTAL, "U3 every datagram became exactly one frame");
    CHECK(st.aborted == 0, "U3 zero aborted brackets under concurrency");
    weft_fanout_stats_t rs;
    weft_fanout_reader_stats(&r, &rs);
    CHECK(ra.torn == 0, "U3 ZERO torn frames accepted");
    CHECK(ra.fresh + ra.drops == ra.last && ra.last == U3_TOTAL,
          "U3 telescoping EXACT to the final frame");

    weft_fanout_reader_destroy(&r);
    weft_uring_detach(&rx);
    weft_fanout_destroy(&f);
    close(sv[0]); close(sv[1]);
}

// ---------------------------------------------------------------------------
// U4 — size honesty
// ---------------------------------------------------------------------------

static void u4_sizes(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "U4 socketpair created");
    weft_fanout_t f;
    CHECK(weft_fanout_init(&f, PB, M) == 0, "U4 fanout init");
    weft_uring_rx_t rx;
    CHECK(weft_uring_attach(&rx, &f, sv[0]) == 0, "U4 uring attach");
    weft_fanout_reader_t r;
    CHECK(weft_fanout_reader_init(&r, f.ring, weft_fanout_ring_bytes(PB, M), PB, M) == 0,
          "U4 reader init");

    uint32_t pkt[PB / 4];
    for (size_t w = 0; w < PB / 4; w++) pkt[w] = tword(1, (uint32_t)w);

    // Short datagram (128 B into a 256 B slot): tail must be ZERO-filled —
    // no cross-frame byte leakage through the fixed-geometry slot.
    CHECK(send(sv[1], pkt, 128, 0) == 128, "U4 short datagram sent");
    CHECK(weft_uring_next(&rx) == 1, "U4 short datagram published as frame 1");
    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    CHECK(c->fresh && c->seq == 1, "U4 short frame claimable");
    const uint32_t* view = (const uint32_t*)weft_fanout_view(&r);
    int head_ok = 1, tail_zero = 1;
    for (size_t w = 0; w < 128 / 4; w++) if (view[w] != tword(1, (uint32_t)w)) { head_ok = 0; }
    for (size_t w = 128 / 4; w < PB / 4; w++) if (view[w] != 0) { tail_zero = 0; }
    CHECK(head_ok, "U4 short frame head bytes verify");
    CHECK(tail_zero, "U4 short frame tail is zeroed (no stale-byte leakage)");

    // Oversized datagram (512 B into a 256 B slot): head kept, counted.
    uint32_t big[512 / 4];
    for (size_t w = 0; w < 512 / 4; w++) big[w] = tword(2, (uint32_t)w);
    CHECK(send(sv[1], big, 512, 0) == 512, "U4 oversized datagram sent");
    CHECK(weft_uring_next(&rx) == 2, "U4 oversized datagram published as frame 2");
    c = weft_fanout_claim(&r);
    CHECK(c->fresh && c->seq == 2, "U4 oversized frame claimable");
    view = (const uint32_t*)weft_fanout_view(&r);
    int keep_ok = 1;
    for (size_t w = 0; w < PB / 4; w++) {
        if (view[w] != tword(2, (uint32_t)w)) { keep_ok = 0; break; }
    }
    CHECK(keep_ok, "U4 oversized frame keeps the first payload_bytes");

    weft_uring_stats_t st;
    weft_uring_stats(&rx, &st);
    CHECK(st.padded == 1 && st.truncated == 1,
          "U4 padded/truncated counted exactly (MSG_TRUNC accounting)");

    weft_fanout_reader_destroy(&r);
    weft_uring_detach(&rx);
    weft_fanout_destroy(&f);
    close(sv[0]); close(sv[1]);
}

// ---------------------------------------------------------------------------
// U5 — lifecycle
// ---------------------------------------------------------------------------

static void u5_lifecycle(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "U5 socketpair created");
    const int before = fcntl(sv[0], F_GETFL, 0);
    weft_fanout_t f;
    CHECK(weft_fanout_init(&f, PB, M) == 0, "U5 fanout init");
    weft_uring_rx_t rx;
    CHECK(weft_uring_attach(&rx, &f, sv[0]) == 0, "U5 attach 1");
    const int during = fcntl(sv[0], F_GETFL, 0);
    CHECK((during & O_NONBLOCK) != 0, "U5 fd non-blocking during the session");
    weft_uring_detach(&rx);
    const int after = fcntl(sv[0], F_GETFL, 0);
    CHECK(after == before, "U5 fd flags restored on detach");
    CHECK(weft_uring_attach(&rx, &f, sv[0]) == 0, "U5 re-attach works");
    weft_uring_detach(&rx);
    CHECK(weft_uring_next(&rx) == 0, "U5 detached session is a safe no-op");
    weft_fanout_destroy(&f);
    close(sv[0]); close(sv[1]);
}

int main(void) {
    u1_probe();
    u2_fidelity();
    u4_sizes();
    u5_lifecycle();
    u3_torture();

    if (g_failures == 0) {
        printf("U-SERIES: ALL PASS\n");
        return 0;
    }
    printf("U-SERIES: %d FAILURE(S)\n", g_failures);
    return 1;
}
