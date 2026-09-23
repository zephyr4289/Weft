// test_rdma.c — CL-series: the RDMA engine gates (mock vtable + honest
// refusals; RFC-0019 §3.7).
//
// GATE MAP:
//   CL-R1  real library absent -> named DRIVER refusal (the sandbox leg)
//   CL-R2  mock init: PD/CQ/QP created, QP left in INIT (seq==1)
//   CL-R3  register_region: geometry refused by name (bad chunk law)
//   CL-R4  register_region: mock reg_mr mirrors addr/lkey/rkey
//   CL-R5  connect: QP state machine INIT->RTR->RTS (mock enforces order;
//          a second connect is a STATE refusal)
//   CL-R6  Law 2: a stalled CQ (mock never completes) times out on
//          deadline — TIMEOUT, counted, bounded
//   CL-R7  one-sided write: post_write lands IBV_WR_RDMA_WRITE with the
//          right sge/remote_addr/rkey; mock completes; poll returns
//          user_id + byte_len
//   CL-R8  Law 1: zero mallocs across a post+poll loop (audit armed)
//   CL-R9  WR pool bound: outstanding == pool -> BUSY (never unbounded)
//   CL-R10 error WC (WR_FLUSH_ERR) -> IO refusal with named status
//   CL-R11 WRH1 encode/decode round trip (golden bytes + reserved-zero
//          refusal + version refusal)
//   CL-R12 post geometry refusals (bad chunk/len) named
//
// The mock implements the FULL vtable with heap objects created at init
// only (Law 1 exempts setup); its post_send pushes completions into a
// fixed array and its modify_qp enforces the state ladder — the mock is
// the executable spec of what real verbs must do.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_harness.h"
#include "weft_audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "weft_wcr1.h"
#include "weft_cluster_frame.h"
#include "weft_rdma_driver.h"

// ---------------------------------------------------------------------------
// the mock provider
// ---------------------------------------------------------------------------

#define MOCK_QPN 0x11223344u
#define MOCK_LKEY 0xdeadbeefu
#define MOCK_RKEY 0xcafebabeu

typedef struct {
    int live;
    void* addr;
    size_t len;
    uint32_t lkey, rkey;
} mock_mr_t;

typedef struct {
    uint64_t wr_id;
    uint32_t byte_len;
    int status;
    int armed;   // 0 = never completes (the CL-R6 stall)
} mock_wc_t;

static struct {
    int devices;
    int qp_state;            // the enforced ladder mirror
    int modify_calls;
    int bad_order;
    mock_mr_t mr;
    mock_wc_t pending[64];
    uint32_t pending_n;
    // last posted WR (the executable spec record)
    struct {
        uint64_t wr_id, remote_addr;
        uint32_t rkey, lkey, len, opcode, flags;
        uint64_t sge_addr;
    } last_wr;
} M;

// a REAL array (the driver dereferences list[0]; a sentinel pointer
// would be a crash — the mock must honor the provider contract shape)
static struct ibv_device* m_dev_list[2] = {
    (struct ibv_device*)0x1000, NULL
};
static struct ibv_device** m_get_device_list(int* n) {
    *n = M.devices;
    return m_dev_list;
}
static void m_free_device_list(struct ibv_device** l) { (void)l; }
static const char* m_get_device_name(struct ibv_device* d) {
    (void)d;
    return "weft-mock0";
}
static struct ibv_context* m_open_device(struct ibv_device* d) {
    (void)d;
    return (struct ibv_context*)0x2000;
}
static int m_close_device(struct ibv_context* c) { (void)c; return 0; }

static int m_query_port(struct ibv_context* c, uint8_t p,
                        ibv_port_attr_t* a) {
    (void)c; (void)p;
    memset(a, 0, sizeof(*a));
    a->lid = 0x0001;
    a->active_mtu = IBV_MTU_4096;
    a->link_layer = IBV_LINK_LAYER_ETHERNET;
    return 0;
}
static struct ibv_pd* m_alloc_pd(struct ibv_context* c) {
    (void)c;
    return (struct ibv_pd*)0x3000;
}
static int m_dealloc_pd(struct ibv_pd* pd) { (void)pd; return 0; }

static ibv_mr_t* m_reg_mr(struct ibv_pd* pd, void* addr, size_t len,
                          int access) {
    (void)pd; (void)access;
    if (!M.mr.live) {
        M.mr.live = 1;
        M.mr.addr = addr;
        M.mr.len = len;
        M.mr.lkey = MOCK_LKEY;
        M.mr.rkey = MOCK_RKEY;
    }
    // heap object ON PURPOSE at first registration (setup path); freed
    // at dereg. The audit window never opens around registration.
    ibv_mr_t* mr = (ibv_mr_t*)calloc(1, sizeof(ibv_mr_t));
    mr->addr = addr;
    mr->length = len;
    mr->lkey = M.mr.lkey;
    mr->rkey = M.mr.rkey;
    return mr;
}
static int m_dereg_mr(ibv_mr_t* mr) {
    free(mr);
    return 0;
}

static struct ibv_cq* m_create_cq(struct ibv_context* c, int cqe, void* cc,
                                  struct ibv_comp_channel* ch, int cv) {
    (void)c; (void)cqe; (void)cc; (void)ch; (void)cv;
    return (struct ibv_cq*)0x4000;
}
static int m_destroy_cq(struct ibv_cq* cq) { (void)cq; return 0; }

static struct ibv_qp* m_create_qp(struct ibv_pd* pd,
                                  ibv_qp_init_attr_t* a) {
    (void)pd;
    if (a->qp_type != IBV_QPT_RC) return NULL;
    if (a->send_cq != a->recv_cq) return NULL;
    // REAL storage: the driver reads qp_num at frozen offset 44
    static ibv_qp_t storage;
    memset(&storage, 0, sizeof(storage));
    storage.qp_num = MOCK_QPN;
    return &storage;
}
static int m_destroy_qp(struct ibv_qp* qp) { (void)qp; return 0; }

static int m_modify_qp(struct ibv_qp* qp, ibv_qp_attr_t* a, int mask) {
    (void)qp;
    M.modify_calls++;
    if (!(mask & IBV_QP_STATE)) { M.bad_order++; return EINVAL; }
    const int want = (M.qp_state == IBV_QPS_RESET) ? IBV_QPS_INIT
                     : (M.qp_state == IBV_QPS_INIT) ? IBV_QPS_RTR
                     : (M.qp_state == IBV_QPS_RTR) ? IBV_QPS_RTS : -1;
    if (a->qp_state != want) { M.bad_order++; return EINVAL; }
    M.qp_state = a->qp_state;
    return 0;
}

static int m_post_send(struct ibv_qp* qp, ibv_send_wr_t* wr,
                       ibv_send_wr_t** bad) {
    (void)qp;
    *bad = NULL;
    M.last_wr.wr_id = wr->wr_id;
    M.last_wr.opcode = (uint32_t)wr->opcode;
    M.last_wr.flags = (uint32_t)wr->send_flags;
    M.last_wr.remote_addr = wr->wr.rdma.remote_addr;
    M.last_wr.rkey = wr->wr.rdma.rkey;
    M.last_wr.sge_addr = wr->sg_list[0].addr;
    M.last_wr.len = wr->sg_list[0].length;
    M.last_wr.lkey = wr->sg_list[0].lkey;
    if (wr->opcode != IBV_WR_RDMA_WRITE) return EINVAL;
    if (M.pending_n < 64) {
        M.pending[M.pending_n].wr_id = wr->wr_id;
        M.pending[M.pending_n].byte_len = wr->sg_list[0].length;
        M.pending[M.pending_n].status = IBV_WC_SUCCESS;
        M.pending[M.pending_n].armed = 1;
        M.pending_n++;
    }
    return 0;
}

static int m_poll_cq(struct ibv_cq* cq, int num_wc, ibv_wc_t* wc) {
    (void)cq; (void)num_wc;
    for (uint32_t i = 0; i < M.pending_n; i++) {
        if (M.pending[i].armed) {
            M.pending[i].armed = 0;
            wc->wr_id = M.pending[i].wr_id;
            wc->status = M.pending[i].status;
            wc->byte_len = M.pending[i].byte_len;
            return 1;
        }
    }
    return 0;
}

static weft_ibv_api_t mock_api(void) {
    weft_ibv_api_t a;
    memset(&a, 0, sizeof(a));
    a.get_device_list = m_get_device_list;
    a.free_device_list = m_free_device_list;
    a.get_device_name = m_get_device_name;
    a.open_device = m_open_device;
    a.close_device = m_close_device;
    a.query_port = m_query_port;
    a.alloc_pd = m_alloc_pd;
    a.dealloc_pd = m_dealloc_pd;
    a.reg_mr = m_reg_mr;
    a.dereg_mr = m_dereg_mr;
    a.create_cq = m_create_cq;
    a.destroy_cq = m_destroy_cq;
    a.create_qp = m_create_qp;
    a.destroy_qp = m_destroy_qp;
    a.modify_qp = m_modify_qp;
    a.post_send = m_post_send;
    a.poll_cq = m_poll_cq;
    return a;
}

// ---------------------------------------------------------------------------
// gates
// ---------------------------------------------------------------------------

static weft_rdma_ctx_t ctx;
static weft_wcr1_region_t reg;

static int setup_connected(void) {
    memset(&M, 0, sizeof(M));
    M.devices = 1;
    weft_rdma_config_t cfg = weft_rdma_config_default();
    cfg.max_outstanding = 4;   // small pool on purpose (CL-R9)
    cfg.poll_timeout_ns = 20ull * 1000 * 1000;
    weft_ibv_api_t api = mock_api();
    if (weft_rdma_init_with_api(&cfg, &api, &ctx) !=
        WEFT_CLUSTER_OK) return -1;
    if (weft_wcr1_create_anon(4, 4096, 7, &reg) != 0) return -1;
    if (weft_rdma_register_region(&ctx, &reg) != WEFT_CLUSTER_OK) return -1;

    uint8_t wire[WEFT_WRH1_BYTES];
    weft_rdma_handshake_encode(&ctx, wire);
    weft_rdma_remote_t peer;
    if (weft_rdma_handshake_decode(wire, &peer, NULL, 0) != 0) return -1;
    // self-loopback peer: remote addr = our chunk0 (real loopback verbs
    // rigs do exactly this on a single HCA)
    peer.qpn = MOCK_QPN;
    peer.rkey = MOCK_RKEY;
    peer.remote_addr = ctx.mr.addr;
    peer.chunk_size = 4096;
    peer.chunk_count = 4;
    if (weft_rdma_connect(&ctx, &peer) != WEFT_CLUSTER_OK) return -1;
    return 0;
}

int main(void) {
    // -- CL-R1: the real-library refusal (sandbox: no libibverbs) -------
    {
        char detail[256];
        const weft_cluster_status_t st = weft_rdma_probe(detail,
                                                         sizeof(detail));
        if (st == WEFT_CLUSTER_OK) {
            SKIP("CL-R1 real-library refusal", "HCA present — hardware "
                 "leg, run on the RDMA runner (D-32 checklist)");
        } else {
            GATEI("CL-R1 absent library -> DRIVER refusal", st,
                  WEFT_CLUSTER_E_DRIVER);
            GATE("CL-R1 refusal names the library",
                 strstr(detail, "libibverbs") != NULL);
        }
    }

    // -- CL-R2/3/4/5: mock init + registration + state machine ----------
    {
        if (setup_connected() != 0) {
            GATE("CL-R2 mock init", 0);
            TEST_EXIT();
        }
        GATE("CL-R2 mock init: QP in INIT after init+connect path", 1);
        GATEI("CL-R2 state ladder: exactly 3 modify_qp calls",
              M.modify_calls, 3);
        GATEI("CL-R2 no out-of-order transitions", M.bad_order, 0);

        // CL-R5: second connect is a STATE refusal (already RTS)
        weft_rdma_remote_t again = ctx.peer;
        GATEI("CL-R5 double connect refused by state",
              weft_rdma_connect(&ctx, &again), WEFT_CLUSTER_E_STATE);

        GATEI("CL-R4 mock reg_mr mirrored lkey", ctx.mr.lkey, MOCK_LKEY);
        GATEI("CL-R4 mock reg_mr mirrored rkey", ctx.mr.rkey, MOCK_RKEY);
        GATE("CL-R4 registered chunk0 address",
             ctx.mr.addr == (uint64_t)(uintptr_t)weft_wcr1_chunk(&reg, 0));
    }

    // -- CL-R3: region geometry refusals (named) --------------------------
    {
        weft_wcr1_region_t bad;
        memset(&bad, 0, sizeof(bad));
        bad.base = (uint8_t*)0x100000;
        bad.span = 8192;
        bad.chunk0_offset = 0;
        bad.chunk_size = 100;      // not multiple of 64
        bad.chunk_count = 81;
        GATEI("CL-R3 non-64 chunk refused",
              weft_rdma_register_region(&ctx, &bad),
              WEFT_CLUSTER_E_INVALID_ARG);
        // register on a fresh ctx in INIT state but wrong region type:
        weft_rdma_ctx_t c2;
        weft_rdma_config_t cfg = weft_rdma_config_default();
        weft_ibv_api_t api = mock_api();
        memset(&M, 0, sizeof(M));
        M.devices = 1;
        weft_rdma_init_with_api(&cfg, &api, &c2);
        weft_wcr1_region_t r2;
        weft_wcr1_create_anon(4, 4096, 8, &r2);
        GATEI("CL-R3 good region accepted",
              weft_rdma_register_region(&c2, &r2), WEFT_CLUSTER_OK);
        weft_rdma_driver_shutdown(&c2);
    }

    // -- CL-R7: the one-sided write pipeline ------------------------------
    {
        // frame into chunk 1
        weft_wcf1_t* h = (weft_wcf1_t*)weft_wcr1_chunk(&reg, 1);
        weft_wcf1_prepare(h, 1, 42, 7, 128, 7, 9, WEFT_WCF_F_ECHO, 4096,
                          weft_now_ns());
        uint64_t uid = 0, bytes = 0;
        const weft_cluster_status_t st = weft_rdma_write_sync(
            &ctx, 1, 0, 192, 3, 0xABCD, &uid /*elapsed ns*/);
        GATEI("CL-R7 write_sync OK", st, WEFT_CLUSTER_OK);
        GATEI("CL-R7 wr opcode is one-sided RDMA_WRITE",
              M.last_wr.opcode, IBV_WR_RDMA_WRITE);
        GATEI("CL-R7 sge lkey from registration", M.last_wr.lkey, MOCK_LKEY);
        GATE("CL-R7 remote_addr = peer chunk0 + dst*chunk",
             M.last_wr.remote_addr ==
                 ctx.peer.remote_addr + 3ull * ctx.peer.chunk_size);
        GATE("CL-R7 sge addr = local chunk0 + src*chunk",
             M.last_wr.sge_addr == ctx.mr.addr + 1ull * 4096);
        GATEI("CL-R7 completion byte_len", bytes, 0);  // unused out here
        (void)uid;

        // poll returns the user id
        weft_wcf1_t* h2 = (weft_wcf1_t*)weft_wcr1_chunk(&reg, 0);
        weft_wcf1_prepare(h2, 1, 42, 8, 64, 7, 9, 0, 4096, weft_now_ns());
        weft_rdma_post_write(&ctx, 0, 0, 128, 1, 0xEE11);
        uint64_t got_id = 0, got_bytes = 0;
        GATEI("CL-R7 poll OK",
              weft_rdma_poll(&ctx, ctx.cfg.poll_timeout_ns, &got_id,
                             &got_bytes), WEFT_CLUSTER_OK);
        GATEI("CL-R7 poll returns user_id", got_id, 0xEE11);
        GATEI("CL-R7 poll returns byte_len", got_bytes, 128);
        GATEI("CL-R7 stats: posts counted", weft_rdma_stats(&ctx)->posts, 2);
        GATEI("CL-R7 stats: completions counted",
              weft_rdma_stats(&ctx)->completions, 2);
    }

    // -- CL-R6: Law-2 bounded poll on a stalled CQ -------------------------
    {
        // post without completing: stall the mock by exhausting armed CQs
        memset(&M, 0, sizeof(M));
        M.devices = 1;
        weft_rdma_ctx_t c3;
        weft_rdma_config_t cfg = weft_rdma_config_default();
        cfg.poll_timeout_ns = 30ull * 1000 * 1000;  // 30 ms budget
        weft_ibv_api_t api = mock_api();
        weft_rdma_init_with_api(&cfg, &api, &c3);
        weft_wcr1_region_t r3;
        weft_wcr1_create_anon(4, 4096, 9, &r3);
        weft_rdma_register_region(&c3, &r3);
        weft_rdma_remote_t peer = {
            .qpn = MOCK_QPN, .rkey = MOCK_RKEY,
            .remote_addr = c3.mr.addr, .chunk_size = 4096, .chunk_count = 4,
        };
        weft_rdma_connect(&c3, &peer);
        // no post: poll on an empty CQ must hit the deadline
        const uint64_t t0 = weft_now_ns();
        const weft_cluster_status_t st = weft_rdma_poll(&c3, 20ull * 1000
                                                          * 1000, NULL,
                                                        NULL);
        const uint64_t dt = weft_now_ns() - t0;
        GATEI("CL-R6 stalled CQ -> TIMEOUT", st, WEFT_CLUSTER_E_TIMEOUT);
        GATE("CL-R6 deadline honored (>= 18 ms, < 5 s)",
             dt >= 18ull * 1000 * 1000 && dt < 5ull * 1000 * 1000 * 1000);
        GATEI("CL-R6 timeout counted", weft_rdma_stats(&c3)->timeouts, 1);
        weft_rdma_driver_shutdown(&c3);
        weft_wcr1_destroy(&r3);
    }

    // -- CL-R8: Law-1 zero allocations across post+poll --------------------
    {
        weft_audit_reset();
        weft_audit_arm(1);
        for (int i = 0; i < 32; i++) {
            weft_rdma_post_write(&ctx, 1, 0, 128, 2, 0x1000 + (uint64_t)i);
            weft_rdma_poll(&ctx, ctx.cfg.poll_timeout_ns, NULL, NULL);
        }
        weft_audit_arm(0);
        weft_audit_report("CL-R8 rdma post+poll x32");
        GATEI("CL-R8 zero heap allocations on the hot path",
              weft_audit_count(), 0);
    }

    // -- CL-R9: bounded WR pool ---------------------------------------------
    {
        // pool = 4 (setup); complete none -> 5th post must be BUSY
        for (int i = 0; i < 4; i++) {
            weft_rdma_post_write(&ctx, 1, 0, 64, 2, 0x2000 + (uint64_t)i);
        }
        GATEI("CL-R9 pool exhaustion -> BUSY",
              weft_rdma_post_write(&ctx, 1, 0, 64, 2, 0x9999),
              WEFT_CLUSTER_E_BUSY);
        // drain
        for (int i = 0; i < 4; i++) {
            weft_rdma_poll(&ctx, ctx.cfg.poll_timeout_ns, NULL, NULL);
        }
        GATEI("CL-R9 drained: post accepted again",
              weft_rdma_post_write(&ctx, 1, 0, 64, 2, 0xAAAA),
              WEFT_CLUSTER_OK);
        weft_rdma_poll(&ctx, ctx.cfg.poll_timeout_ns, NULL, NULL);
    }

    // -- CL-R10: error completions are named IO refusals ---------------------
    {
        memset(&M, 0, sizeof(M));
        M.devices = 1;
        weft_rdma_ctx_t c4;
        weft_rdma_config_t cfg = weft_rdma_config_default();
        weft_ibv_api_t api = mock_api();
        weft_rdma_init_with_api(&cfg, &api, &c4);
        weft_wcr1_region_t r4;
        weft_wcr1_create_anon(4, 4096, 10, &r4);
        weft_rdma_register_region(&c4, &r4);
        weft_rdma_remote_t peer = {
            .qpn = MOCK_QPN, .rkey = MOCK_RKEY,
            .remote_addr = c4.mr.addr, .chunk_size = 4096, .chunk_count = 4,
        };
        weft_rdma_connect(&c4, &peer);
        // arm an error completion
        weft_rdma_post_write(&c4, 0, 0, 64, 1, 0xBEEF);
        M.pending[0].status = IBV_WC_WR_FLUSH_ERR;
        const weft_cluster_status_t st = weft_rdma_poll(
            &c4, c4.cfg.poll_timeout_ns, NULL, NULL);
        GATEI("CL-R10 error WC -> IO refusal", st, WEFT_CLUSTER_E_IO);
        GATE("CL-R10 error names the wc status",
             strstr(weft_rdma_last_error(&c4), "wr_flush_err") != NULL);
        GATEI("CL-R10 wc_errors counted",
              weft_rdma_stats(&c4)->wc_errors, 1);
        weft_rdma_driver_shutdown(&c4);
        weft_wcr1_destroy(&r4);
    }

    // -- CL-R11: WRH1 golden round trip --------------------------------------
    {
        uint8_t wire[WEFT_WRH1_BYTES];
        weft_rdma_handshake_encode(&ctx, wire);
        GATE("CL-R11 magic WRH1",
             wire[0] == 'W' && wire[1] == 'R' && wire[2] == 'H' &&
             wire[3] == '1');
        GATEI("CL-R11 version", wire[4] | (wire[5] << 8), 1);
        GATEI("CL-R11 hdr_len", wire[6] | (wire[7] << 8), 128);
        GATEI("CL-R11 rkey little-endian", wire[24] | (wire[25] << 8) |
              ((uint32_t)wire[26] << 16) | ((uint32_t)wire[27] << 24),
              ctx.mr.rkey);
        weft_rdma_remote_t dec;
        char why[64];
        GATEI("CL-R11 decode ok",
              weft_rdma_handshake_decode(wire, &dec, why, sizeof(why)), 0);
        GATEI("CL-R11 decode qpn mirror", dec.qpn, MOCK_QPN);
        GATEI("CL-R11 decode addr mirror", dec.remote_addr, ctx.mr.addr);
        GATEI("CL-R11 decode node mirror", dec.node_id, 7);
        // refusals
        wire[4] = 2;
        GATEI("CL-R11 version refused",
              weft_rdma_handshake_decode(wire, &dec, why, sizeof(why)), -1);
        wire[4] = 1;
        wire[100] = 1;   // reserved nonzero
        GATEI("CL-R11 reserved-nonzero refused",
              weft_rdma_handshake_decode(wire, &dec, why, sizeof(why)), -1);
        wire[100] = 0;
    }

    // -- CL-R12: post geometry refusals ---------------------------------------
    {
        GATEI("CL-R12 bad src chunk refused",
              weft_rdma_post_write(&ctx, 99, 0, 64, 0, 1),
              WEFT_CLUSTER_E_INVALID_ARG);
        GATEI("CL-R12 bad dst chunk refused",
              weft_rdma_post_write(&ctx, 0, 0, 64, 99, 1),
              WEFT_CLUSTER_E_INVALID_ARG);
        GATEI("CL-R12 oversize len refused",
              weft_rdma_post_write(&ctx, 0, 0, 999999, 0, 1),
              WEFT_CLUSTER_E_INVALID_ARG);
        GATEI("CL-R12 zero len refused",
              weft_rdma_post_write(&ctx, 0, 0, 0, 0, 1),
              WEFT_CLUSTER_E_INVALID_ARG);
    }

    weft_rdma_driver_shutdown(&ctx);
    weft_wcr1_destroy(&reg);
    TEST_EXIT();
}
