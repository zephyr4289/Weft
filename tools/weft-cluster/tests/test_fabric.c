// test_fabric.c — CL-series: the fabric selector + loopback ground-truth
// gates (RFC-0019 §6.5).
//
// GATE MAP:
//   CL-F1  probe_all returns the preference order
//   CL-F2  the chain string names all four engines with verdicts
//   CL-F3  sandbox expectation: rdma refused honestly, loopback OK
//   CL-F4  best() = the first available rung
//   CL-F5  loopback end-to-end: WCF1 frame through send -> recv_batch,
//          payload bit-identical (the ground-truth road)
//   CL-F6  loopback honesty: wrong cluster refused; FIFO overflow is a
//          bounded BUSY, never a drop
//   CL-F7  the fabric chain line for uring carries its probe detail

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_harness.h"

#include <stdio.h>
#include <string.h>

#include "weft_wcr1.h"
#include "weft_cluster_fabric.h"
#include "backends/loopback/weft_loopback.h"

int main(void) {
    // -- CL-F1..F4: the cascade ------------------------------------------------
    {
        weft_fabric_probe_t p[WEFT_FABRIC_KIND_COUNT];
        weft_fabric_probe_all(p);
        GATEI("CL-F1 preference order (rdma first)",
              p[WEFT_FABRIC_RDMA].kind, WEFT_FABRIC_RDMA);
        GATEI("CL-F1 loopback is entry 3",
              p[WEFT_FABRIC_LOOPBACK].kind, WEFT_FABRIC_LOOPBACK);
        GATE("CL-F1 loopback always available",
             p[WEFT_FABRIC_LOOPBACK].available == 1);

        char chain[1024];
        weft_fabric_chain(chain, sizeof(chain));
        GATE("CL-F2 chain names all four engines",
             strstr(chain, "rdma:") && strstr(chain, "xdp:") &&
             strstr(chain, "uring:") && strstr(chain, "loopback:"));
        GATE("CL-F2 chain carries verdicts",
             strstr(chain, "REFUSED") != NULL &&
             strstr(chain, "OK") != NULL);

        GATE("CL-F3 rdma refusal is honest on this runner",
             p[WEFT_FABRIC_RDMA].available == 1 ||
             p[WEFT_FABRIC_RDMA].status == WEFT_CLUSTER_E_DRIVER ||
             p[WEFT_FABRIC_RDMA].status == WEFT_CLUSTER_E_HW_ABSENT);

        const weft_fabric_kind_t best = weft_fabric_best();
        int ok = 0;
        for (int i = 0; i < WEFT_FABRIC_KIND_COUNT; i++) {
            if (p[i].available) {
                ok = (best == p[i].kind);
                break;
            }
        }
        GATE("CL-F4 best() is the first available rung", ok);
    }

    // -- CL-F5/F6: the loopback ground truth --------------------------------------
    {
        weft_wcr1_region_t src, dst;
        GATEI("CL-F5 src region", weft_wcr1_create_anon(4, 4096, 1, &src),
              0);
        GATEI("CL-F5 dst region", weft_wcr1_create_anon(4, 4096, 2, &dst),
              0);
        weft_loopback_ctx_t lb;
        GATEI("CL-F5 loopback init",
              weft_loopback_init(&lb, &src, &dst, 0x2A4D, 0x11),
              WEFT_CLUSTER_OK);

        const char msg[] = "cluster loopback ground truth";
        weft_wcf1_t* h = (weft_wcf1_t*)weft_wcr1_chunk(&src, 2);
        weft_wcf1_prepare(h, 0x2A4D, 0x11, 1, sizeof(msg) - 1, 1, 2, 0,
                          4096, weft_now_ns());
        memcpy((uint8_t*)h + 64, msg, sizeof(msg) - 1);

        GATEI("CL-F5 send OK",
              weft_loopback_send(&lb, 2, 1), WEFT_CLUSTER_OK);
        weft_cluster_rx_ent_t ent[4];
        uint32_t n = 0;
        GATEI("CL-F5 recv_batch OK",
              weft_loopback_recv_batch(&lb, ent, 4, &n), WEFT_CLUSTER_OK);
        GATEI("CL-F5 one frame delivered", n, 1);
        GATEI("CL-F5 dst chunk correct", ent[0].chunk_idx, 1);
        GATEI("CL-F5 payload len correct", ent[0].len, sizeof(msg) - 1);
        GATE("CL-F5 payload bit-identical",
             memcmp(ent[0].payload, msg, sizeof(msg) - 1) == 0);
        GATEI("CL-F5 copy label present ([FALLBACK-COPY])",
              weft_loopback_stats(&lb)->copy_label, 1);

        // CL-F6a: wrong cluster refused by name
        weft_wcf1_t* h2 = (weft_wcf1_t*)weft_wcr1_chunk(&src, 0);
        weft_wcf1_prepare(h2, 0x9999, 0x11, 2, 16, 1, 2, 0, 4096, 0);
        GATEI("CL-F6 wrong cluster refused",
              weft_loopback_send(&lb, 0, 0), WEFT_CLUSTER_E_INVALID_ARG);

        // CL-F6b: FIFO overflow is BUSY (bounded), never a drop
        weft_wcf1_prepare(h2, 0x2A4D, 0x11, 3, 16, 1, 2, 0, 4096, 0);
        int saw_busy = 0;
        for (int i = 0; i < 8; i++) {   // cap = 4, one slot used
            if (weft_loopback_send(&lb, 0, 2) == WEFT_CLUSTER_E_BUSY) {
                saw_busy = 1;
                break;
            }
        }
        GATE("CL-F6 FIFO overflow is a bounded BUSY", saw_busy);
        GATEI("CL-F6 overflow counted",
              weft_loopback_stats(&lb)->overflow >= 1, 1);

        weft_loopback_shutdown(&lb);
        weft_wcr1_destroy(&src);
        weft_wcr1_destroy(&dst);
    }

    // -- CL-F7: chain carries the uring probe detail --------------------------------
    {
        char chain[1024];
        weft_fabric_chain(chain, sizeof(chain));
        const char* u = strstr(chain, "uring:");
        GATE("CL-F7 uring line carries probe detail",
             u != NULL && strstr(u, "rung=") != NULL);
    }

    TEST_EXIT();
}
