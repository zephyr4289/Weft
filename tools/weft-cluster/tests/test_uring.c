// test_uring.c — CL-series: the io_uring engine gates (RFC-0019 §5.6).
//
// GATE MAP:
//   CL-U1  probe: rings LIVE (the 5.10 sandbox road) or honest skip
//   CL-U2  init + config validation (pow2 law)
//   CL-U3  ephemeral socket bind
//   CL-U4  registered buffers over a small region (under RLIMIT_MEMLOCK)
//   CL-U5  REAL loopback ping-pong through the engine (two contexts,
//          WCF1 end-to-end, payload bit-identical)
//   CL-U6  ZC ladder: SEND_ZC refused on < 5.19 -> sticky downgrade,
//          labeled [FALLBACK-COPY] (or ZC live on newer kernels)
//   CL-U7  Law 2: empty poll hits the deadline -> TIMEOUT, counted
//   CL-U8  Law 1: zero allocations across a send/flush/poll cycle
//   CL-U9  geometry + registration-bound refusals named
//   CL-U10 the rx user_data kind-tag law

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_harness.h"
#include "weft_audit.h"

#include <stdio.h>
#include <string.h>

#include "weft_wcr1.h"
#include "weft_cluster_frame.h"
#include "weft_uring_driver.h"

int main(void) {
    // -- CL-U1: probe ---------------------------------------------------------
    char d[256];
    const weft_uring_cap_t cap = weft_uring_probe(d, sizeof(d));
    if (cap < WEFT_URING_CAP_RING) {
        SKIP("CL-U1..U10 (io_uring)",
             "io_uring not available on this runner — probe says so, "
             "honestly");
        TEST_EXIT();
    }
    GATE("CL-U1 rings LIVE (probe)", 1);
    GATE("CL-U1 probe detail mentions register rung",
         strstr(d, "register") != NULL);

    // -- CL-U2: init + config law ----------------------------------------------
    weft_uring_ctx_t A, B;
    {
        weft_uring_config_t bad = weft_uring_config_default();
        bad.sq_entries = 100;   // not pow2
        weft_uring_ctx_t t;
        GATEI("CL-U2 non-pow2 entries refused",
              weft_uring_driver_init(&bad, &t), WEFT_CLUSTER_E_INVALID_ARG);

        weft_uring_config_t cfg = weft_uring_config_default();
        cfg.sq_entries = 64;
        cfg.poll_timeout_ns = 100ull * 1000 * 1000;  // 100 ms
        GATEI("CL-U2 init OK", weft_uring_driver_init(&cfg, &A),
              WEFT_CLUSTER_OK);
        GATEI("CL-U2 second ctx OK", weft_uring_driver_init(&cfg, &B),
              WEFT_CLUSTER_OK);
    }

    // -- CL-U3: sockets ----------------------------------------------------------
    uint16_t port_b = 0, port_a = 0;
    GATEI("CL-U3 bind A ephemeral",
          weft_uring_bind_socket(&A, &port_a), WEFT_CLUSTER_OK);
    GATEI("CL-U3 bind B ephemeral",
          weft_uring_bind_socket(&B, &port_b), WEFT_CLUSTER_OK);
    GATE("CL-U3 distinct ports", port_a != port_b && port_a != 0 &&
                                 port_b != 0);

    // -- CL-U4: registration (small region under the memlock floor) -------------
    weft_wcr1_region_t ra, rb;
    GATEI("CL-U4 region A created", weft_wcr1_create_anon(4, 4096, 1, &ra),
          0);
    GATEI("CL-U4 region B created", weft_wcr1_create_anon(4, 4096, 2, &rb),
          0);
    const weft_cluster_status_t sta = weft_uring_register_region(&A, &ra);
    const weft_cluster_status_t stb = weft_uring_register_region(&B, &rb);
    if (sta == WEFT_CLUSTER_OK && stb == WEFT_CLUSTER_OK) {
        GATEI("CL-U4 both regions registered",
              weft_uring_stats(&A)->registered +
              weft_uring_stats(&B)->registered, 2);
    } else {
        // RLIMIT_MEMLOCK floor: the honest [FALLBACK-COPY] rung — the
        // engine still works, the label is the assertion
        GATE("CL-U4 registration refused -> labeled fallback",
             strstr(weft_uring_last_error(&A), "FALLBACK-COPY") != NULL);
    }

    // -- CL-U5: the REAL ping-pong ---------------------------------------------
    {
        // A connects to B; B connects back to A (echo road)
        GATEI("CL-U5 A->B peer", weft_uring_connect_peer(&A, "127.0.0.1",
                                                         port_b),
              WEFT_CLUSTER_OK);
        GATEI("CL-U5 B->A peer", weft_uring_connect_peer(&B, "127.0.0.1",
                                                         port_a),
              WEFT_CLUSTER_OK);

        // frame in A chunk 1
        weft_wcf1_t* h = (weft_wcf1_t*)weft_wcr1_chunk(&ra, 1);
        const char msg[] = "weft-cluster ping frame 0001";
        weft_wcf1_prepare(h, 1, 42, 1, sizeof(msg) - 1, 1, 2,
                          WEFT_WCF_F_ECHO | WEFT_WCF_F_TS, 4096,
                          weft_now_ns());
        memcpy((uint8_t*)h + 64, msg, sizeof(msg) - 1);

        // arm B's rx (chunk 0), then A sends
        GATEI("CL-U5 B rx armed",
              weft_uring_recv_arm(&B, 0, 0x7000B), WEFT_CLUSTER_OK);
        GATEI("CL-U5 B flush", weft_uring_flush(&B), WEFT_CLUSTER_OK);
        GATEI("CL-U5 A sends",
              weft_uring_send_frame(&A, 1, 0, 64 + sizeof(msg) - 1,
                                    0x7000A), WEFT_CLUSTER_OK);
        GATEI("CL-U5 A flush", weft_uring_flush(&A), WEFT_CLUSTER_OK);

        // settle the sender first: on kernels without SEND_ZC the first
        // TX CQE is the -EINVAL verdict — the engine downgrades and
        // RESUBMITS the shadowed frame (the discovery costs a syscall,
        // never the frame)
        weft_uring_ev_t ev[8];
        uint32_t n = 0;
        GATEI("CL-U5 A settles the ZC verdict",
              weft_uring_poll_events(&A, 100ull * 1000 * 1000, 1, ev, 8,
                                     &n), WEFT_CLUSTER_OK);

        GATEI("CL-U5 B harvests rx",
              weft_uring_poll_events(&B, 500ull * 1000 * 1000, 1, ev, 8,
                                     &n), WEFT_CLUSTER_OK);
        int got_rx = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (ev[i].kind == 2 && ev[i].res > 0) got_rx = 1;
        }
        GATE("CL-U5 B received the frame (kind=rx, res>0)", got_rx);

        // B validates + echoes from its chunk 1
        const weft_wcf1_t* rh = (const weft_wcf1_t*)weft_wcr1_chunk(&rb, 0);
        const weft_wcf1_refusal_t rr = weft_wcf1_validate(
            rh, 4096, 1, 0, 4096, NULL, NULL);
        GATEI("CL-U5 B-side WCF1 valid", rr, WEFT_WCF1_OK);
        GATE("CL-U5 payload bit-identical",
             memcmp((const uint8_t*)rh + 64, msg, sizeof(msg) - 1) == 0);

        memcpy(weft_wcr1_chunk(&rb, 1), rh, 64 + rh->payload_len);
        GATEI("CL-U5 A rx armed",
              weft_uring_recv_arm(&A, 2, 0x7000C), WEFT_CLUSTER_OK);
        GATEI("CL-U5 A flush", weft_uring_flush(&A), WEFT_CLUSTER_OK);
        GATEI("CL-U5 B echoes",
              weft_uring_send_frame(&B, 1, 0, 64 + rh->payload_len, 0xE),
              WEFT_CLUSTER_OK);
        GATEI("CL-U5 B flush", weft_uring_flush(&B), WEFT_CLUSTER_OK);
        n = 0;
        GATEI("CL-U5 B settles the ZC verdict",
              weft_uring_poll_events(&B, 100ull * 1000 * 1000, 1, ev, 8,
                                     &n), WEFT_CLUSTER_OK);
        n = 0;
        GATEI("CL-U5 A harvests echo",
              weft_uring_poll_events(&A, 500ull * 1000 * 1000, 1, ev, 8,
                                     &n), WEFT_CLUSTER_OK);
        int got_echo = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (ev[i].kind == 2 && ev[i].res > 0) got_echo = 1;
        }
        GATE("CL-U5 A received the echo", got_echo);
        const weft_wcf1_t* eh =
            (const weft_wcf1_t*)weft_wcr1_chunk(&ra, 2);
        GATE("CL-U5 echo payload bit-identical",
             memcmp((const uint8_t*)eh + 64, msg, sizeof(msg) - 1) == 0);
    }

    // -- CL-U6: the ZC ladder verdict -------------------------------------------
    {
        const weft_uring_stats_t* st = weft_uring_stats(&A);
        if (st->tx_mode == (int)WEFT_URING_TX_ZC) {
            GATE("CL-U6 SEND_ZC live (kernel >= 5.19)", 1);
        } else {
            GATE("CL-U6 SEND_ZC downgraded (sticky) with label",
                 st->downgrades >= 1 &&
                 strstr(weft_uring_last_error(&A), "FALLBACK-COPY") !=
                     NULL);
            GATEI("CL-U6 landed on WRITE_FIXED", st->tx_mode,
                  WEFT_URING_TX_FIXED);
        }
    }

    // -- CL-U7: Law-2 deadline ----------------------------------------------------
    {
        weft_uring_ev_t ev[4];
        uint32_t n = 0;
        const uint64_t t0 = weft_now_ns();
        const weft_cluster_status_t st = weft_uring_poll_events(
            &A, 10ull * 1000 * 1000, 1, ev, 4, &n);
        const uint64_t dt = weft_now_ns() - t0;
        GATEI("CL-U7 empty poll -> TIMEOUT", st, WEFT_CLUSTER_E_TIMEOUT);
        GATE("CL-U7 deadline honored (>= 8ms, < 5s)",
             dt >= 8ull * 1000 * 1000 && dt < 5ull * 1000 * 1000 * 1000);
        GATEI("CL-U7 timeout counted", weft_uring_stats(&A)->timeouts >= 1,
              1);
    }

    // -- CL-U8: Law-1 audit window -------------------------------------------------
    {
        weft_wcf1_t* h = (weft_wcf1_t*)weft_wcr1_chunk(&ra, 3);
        weft_wcf1_prepare(h, 1, 42, 9, 128, 1, 2, 0, 4096, weft_now_ns());
        weft_audit_reset();
        weft_audit_arm(1);
        for (int i = 0; i < 8; i++) {
            weft_uring_send_frame(&A, 3, 0, 192, 0x900 + (uint64_t)i);
            weft_uring_flush(&A);
            weft_uring_ev_t ev[4];
            uint32_t n = 0;
            weft_uring_poll_events(&A, 50ull * 1000 * 1000, 1, ev, 4, &n);
        }
        weft_audit_arm(0);
        weft_audit_report("CL-U8 uring send+flush+poll x8");
        GATEI("CL-U8 zero heap allocations on the steady path",
              weft_audit_count(), 0);
    }

    // -- CL-U9: geometry + bound refusals --------------------------------------------
    {
        GATEI("CL-U9 bad chunk refused",
              weft_uring_send_frame(&A, 99, 0, 64, 1),
              WEFT_CLUSTER_E_INVALID_ARG);
        GATEI("CL-U9 oversize len refused",
              weft_uring_send_frame(&A, 0, 0, 999999, 1),
              WEFT_CLUSTER_E_INVALID_ARG);
        GATEI("CL-U9 zero len refused",
              weft_uring_send_frame(&A, 0, 0, 0, 1),
              WEFT_CLUSTER_E_INVALID_ARG);
        // registration bound: > 1024 chunks is a named refusal
        weft_uring_ctx_t C;
        weft_uring_config_t cfg = weft_uring_config_default();
        cfg.sq_entries = 64;
        weft_uring_driver_init(&cfg, &C);
        weft_wcr1_region_t big;
        if (weft_wcr1_create_anon(1200, 4096, 3, &big) == 0) {
            GATEI("CL-U9 >1024 chunks refused",
                  weft_uring_register_region(&C, &big),
                  WEFT_CLUSTER_E_UNSUPPORTED);
            weft_wcr1_destroy(&big);
        }
        weft_uring_driver_shutdown(&C);
    }

    // -- CL-U10: the kind-tag law --------------------------------------------------------
    {
        GATEI("CL-U10 rx user_data bit62 refused",
              weft_uring_recv_arm(&A, 0, 1ull << 62),
              WEFT_CLUSTER_E_INVALID_ARG);
    }

    weft_uring_driver_shutdown(&A);
    weft_uring_driver_shutdown(&B);
    weft_wcr1_destroy(&ra);
    weft_wcr1_destroy(&rb);
    TEST_EXIT();
}
