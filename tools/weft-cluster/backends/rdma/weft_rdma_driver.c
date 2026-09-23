// weft_rdma_driver.c — RFC-0019 §3: the one-sided RDMA engine (see
// weft_rdma_driver.h for the contract and the law map).
//
// Implementation stance: the provider is a VTABLE. Production resolves
// it from libibverbs.so.1 with dlsym; the mock battery injects its own.
// Every provider call is checked; every failure lands in ctx->err with
// a name, and the status ladder stays switchable (Law 4). The hot path
// (post_write/poll) touches ONLY the pre-allocated WR pool — the
// CL-R8 audit gate proves zero allocations around a post+poll loop.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "weft_rdma_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

// ---------------------------------------------------------------------------
// vtable resolution (the dlopen discipline)
// ---------------------------------------------------------------------------

#define RESOLVE(field, name)                                        \
    do {                                                            \
        void* sym_ = dlsym(handle, name);                           \
        if (!sym_) {                                                \
            if (missing) *missing = name;                           \
            return -1;                                              \
        }                                                           \
        /* strict-C11 object-representation copy (no typeof);       \
           POSIX guarantees ptr-to-ptr size equality */             \
        memcpy(&api->field, &sym_, sizeof(sym_));                   \
    } while (0)

int weft_ibv_api_resolve(void* handle, weft_ibv_api_t* api,
                         const char** missing) {
    memset(api, 0, sizeof(*api));
    if (!handle) { if (missing) *missing = "(null handle)"; return -1; }
    RESOLVE(get_device_list,  "ibv_get_device_list");
    RESOLVE(free_device_list, "ibv_free_device_list");
    RESOLVE(get_device_name,  "ibv_get_device_name");
    RESOLVE(open_device,      "ibv_open_device");
    RESOLVE(close_device,     "ibv_close_device");
    RESOLVE(query_port,       "ibv_query_port");
    RESOLVE(alloc_pd,         "ibv_alloc_pd");
    RESOLVE(dealloc_pd,       "ibv_dealloc_pd");
    RESOLVE(reg_mr,           "ibv_reg_mr");
    RESOLVE(dereg_mr,         "ibv_dereg_mr");
    RESOLVE(create_cq,        "ibv_create_cq");
    RESOLVE(destroy_cq,       "ibv_destroy_cq");
    RESOLVE(create_qp,        "ibv_create_qp");
    RESOLVE(destroy_qp,       "ibv_destroy_qp");
    RESOLVE(modify_qp,        "ibv_modify_qp");
    RESOLVE(post_send,        "ibv_post_send");
    RESOLVE(poll_cq,          "ibv_poll_cq");
    return 0;
}
#undef RESOLVE

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void ctx_err(weft_rdma_ctx_t* ctx, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->err, sizeof(ctx->err), fmt, ap);
    va_end(ap);
}

// little-endian wire putters (WRH1 is LE-canonical, like WFSH/WCF1)
static void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void put_le32(uint8_t* p, uint32_t v) {
    put_le16(p, (uint16_t)v); put_le16(p + 2, (uint16_t)(v >> 16));
}
static void put_le64(uint8_t* p, uint64_t v) {
    put_le32(p, (uint32_t)v); put_le32(p + 4, (uint32_t)(v >> 32));
}
static uint16_t get_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t get_le32(const uint8_t* p) {
    return (uint32_t)get_le16(p) | ((uint32_t)get_le16(p + 2) << 16);
}
static uint64_t get_le64(const uint8_t* p) {
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static const char* wc_status_name(int st) {
    switch (st) {
        case IBV_WC_SUCCESS:        return "success";
        case IBV_WC_LOC_LEN_ERR:    return "loc_len_err";
        case IBV_WC_LOC_QP_OP_ERR:  return "loc_qp_op_err";
        case IBV_WC_LOC_PROT_ERR:   return "loc_prot_err";
        case IBV_WC_WR_FLUSH_ERR:   return "wr_flush_err";
        case IBV_WC_LOC_ACCESS_ERR: return "loc_access_err";
        case IBV_WC_REM_ACCESS_ERR: return "rem_access_err";
        case IBV_WC_RETRY_EXC_ERR:  return "retry_exc_err";
        case IBV_WC_GENERAL_ERR:    return "general_err";
    }
    return "wc_status";
}

static int gid_nonzero(const uint8_t gid[16]) {
    for (int i = 0; i < 16; i++) if (gid[i]) return 1;
    return 0;
}

/// Bounded full-send/full-recv over a TCP fd (setup-path helpers; the
/// SO_*TIMEO options above enforce the Law-2 deadline).
static int send_full(int fd, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t off = 0;
    while (off < len) {
        const ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}
static int recv_full(int fd, void* buf, size_t len) {
    uint8_t* p = (uint8_t*)buf;
    size_t off = 0;
    while (off < len) {
        const ssize_t n = recv(fd, p + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/// Free the WR-pool slot holding user_id (completion path; O(pool),
/// bounded, allocation-free).
static void free_wr_by_user(weft_rdma_ctx_t* ctx, uint64_t user_id) {
    for (uint32_t i = 0; i < ctx->pool_size; i++) {
        if (ctx->wr_pool[i].in_use && ctx->wr_pool[i].user_id == user_id) {
            ctx->wr_pool[i].in_use = 0;
            if (ctx->outstanding) ctx->outstanding--;
            return;
        }
    }
}

weft_rdma_config_t weft_rdma_config_default(void) {
    weft_rdma_config_t c = {
        .device_name = NULL,
        .port_num = 1,
        .gid_index = -1,
        .cq_depth = 64,
        .qp_depth = 64,
        .max_outstanding = 32,
        .poll_timeout_ns = 50ull * 1000 * 1000,  // 50 ms
        .handshake_port = 47912,
    };
    return c;
}

// sysfs GID reader: /sys/class/infiniband/<dev>/ports/<n>/gids/<idx> —
// "xxxx:xxxx:...:xxxx" (8 groups of 4 hex). Deterministic, no provider
// ABI involvement (ibv_query_gid availability varies by rdma-core era).
static int sysfs_read_gid(const char* dev, uint8_t port, int idx,
                          uint8_t gid[16]) {
    char path[192];
    snprintf(path, sizeof(path),
             "/sys/class/infiniband/%s/ports/%u/gids/%d", dev, port, idx);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[128];
    int ok = 0;
    if (fgets(line, sizeof(line), f)) {
        // parse 8 groups of <=4 hex digits
        const char* p = line;
        int byte_i = 0;
        for (int g = 0; g < 8 && byte_i < 16; g++) {
            while (*p == ':' || *p == ' ') p++;
            int v = 0, digits = 0;
            while (*p && *p != ':' && *p != '\n' && digits < 4) {
                char c = *p++;
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else goto done;
                v = (v << 4) | d;
                digits++;
            }
            if (digits == 0) goto done;
            gid[byte_i++] = (uint8_t)(v >> 8);
            gid[byte_i++] = (uint8_t)(v & 0xff);
        }
        ok = (byte_i == 16);
    }
done:
    fclose(f);
    return ok ? 0 : -1;
}

static int gid_is_link_local(const uint8_t gid[16]) {
    return gid[0] == 0xfe && gid[1] == 0x80;
}

// ---------------------------------------------------------------------------
// probe / init
// ---------------------------------------------------------------------------

weft_cluster_status_t weft_rdma_probe(char* detail, size_t detail_len) {
    const char* const names[] = { WEFT_RDMA_SONAME, "libibverbs.so" };
    void* h = weft_dlopen_first(names, 2, NULL);
    if (!h) {
        if (detail) snprintf(detail, detail_len,
                             "rdma: %s not loadable — install rdma-core "
                             "(honest refusal, Law 4)", WEFT_RDMA_SONAME);
        return WEFT_CLUSTER_E_DRIVER;
    }
    weft_ibv_api_t api;
    const char* missing = NULL;
    if (weft_ibv_api_resolve(h, &api, &missing) != 0) {
        if (detail) snprintf(detail, detail_len,
                             "rdma: symbol %s missing from %s", missing,
                             WEFT_RDMA_SONAME);
        dlclose(h);
        return WEFT_CLUSTER_E_DRIVER;
    }
    int n = 0;
    struct ibv_device** list = api.get_device_list(&n);
    if (!list || n == 0) {
        if (list) api.free_device_list(list);
        if (detail) snprintf(detail, detail_len,
                             "rdma: library loaded, zero HCAs visible");
        dlclose(h);
        return WEFT_CLUSTER_E_HW_ABSENT;
    }
    if (detail) snprintf(detail, detail_len, "rdma: %d HCA(s) visible", n);
    api.free_device_list(list);
    dlclose(h);
    return WEFT_CLUSTER_OK;
}

static weft_cluster_status_t rdma_setup(const weft_rdma_config_t* cfg_in,
                                        const weft_ibv_api_t* api, void* dl,
                                        int injected,
                                        weft_rdma_ctx_t* ctx) {
    if (!cfg_in || !api || !ctx) return WEFT_CLUSTER_E_INVALID_ARG;
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *cfg_in;
    if (ctx->cfg.device_name && !injected) {
        // caller-owned string lifetime: copy into our err-adjacent storage
        // (device_name stays a pointer — documented: must outlive ctx)
    }
    ctx->api = *api;
    ctx->dl = dl;
    ctx->injected = injected;

    // geometry sanity (before touching the provider)
    if (ctx->cfg.cq_depth == 0 || ctx->cfg.qp_depth == 0 ||
        ctx->cfg.max_outstanding == 0 ||
        ctx->cfg.max_outstanding > ctx->cfg.qp_depth) {
        ctx_err(ctx, "rdma: bad config (cq=%u qp=%u pool=%u)",
                ctx->cfg.cq_depth, ctx->cfg.qp_depth,
                ctx->cfg.max_outstanding);
        return WEFT_CLUSTER_E_INVALID_ARG;
    }

    // device enumeration
    int n = 0;
    struct ibv_device** list = ctx->api.get_device_list(&n);
    if (!list || n == 0) {
        if (list) ctx->api.free_device_list(list);
        ctx_err(ctx, "rdma: no HCA visible (%s)",
                injected ? "mock" : WEFT_RDMA_SONAME);
        return WEFT_CLUSTER_E_HW_ABSENT;
    }
    struct ibv_device* chosen = list[0];
    const char* chosen_name = "weft-mock0";
    for (int i = 0; i < n; i++) {
        const char* nm = ctx->api.get_device_name(list[i]);
        if (!nm) continue;
        if (!ctx->cfg.device_name || strcmp(nm, ctx->cfg.device_name) == 0) {
            chosen = list[i];
            chosen_name = nm;
            break;
        }
    }

    ctx->dev = ctx->api.open_device(chosen);
    ctx->api.free_device_list(list);
    if (!ctx->dev) {
        ctx_err(ctx, "rdma: open_device(%s) failed", chosen_name);
        return WEFT_CLUSTER_E_IO;
    }

    if (ctx->api.query_port(ctx->dev, ctx->cfg.port_num,
                            &ctx->port_attr) != 0) {
        ctx_err(ctx, "rdma: query_port(%u) failed", ctx->cfg.port_num);
        return WEFT_CLUSTER_E_IO;
    }

    // GID selection: explicit index, else first non-link-local (RoCEv2).
    memset(ctx->gid, 0, sizeof(ctx->gid));
    ctx->gid_index_used = ctx->cfg.gid_index;
    if (ctx->cfg.gid_index >= 0) {
        if (!injected &&
            sysfs_read_gid(chosen_name, ctx->cfg.port_num,
                           ctx->cfg.gid_index, ctx->gid) != 0) {
            // provider path (mock) has no sysfs; a failed sysfs read leaves
            // the gid zeroed — the handshake carries peer gids, ours is
            // advisory for ah_attr only when RoCE. Recorded, not fatal.
            ctx_err(ctx, "rdma: sysfs gid %d unreadable (gid left zero — "
                         "advisory only)", ctx->cfg.gid_index);
        }
    } else {
        ctx->gid_index_used = 0;
        if (!injected) {
            for (int i = 0; i < 32; i++) {
                uint8_t g[16];
                if (sysfs_read_gid(chosen_name, ctx->cfg.port_num, i,
                                   g) == 0) {
                    memcpy(ctx->gid, g, 16);
                    ctx->gid_index_used = i;
                    if (!gid_is_link_local(g)) break;  // RoCEv2 preferred
                }
            }
        }
    }

    ctx->pd = ctx->api.alloc_pd(ctx->dev);
    if (!ctx->pd) {
        ctx_err(ctx, "rdma: alloc_pd failed");
        return WEFT_CLUSTER_E_IO;
    }
    ctx->cq = ctx->api.create_cq(ctx->dev, (int)ctx->cfg.cq_depth, NULL,
                                 NULL, 0);
    if (!ctx->cq) {
        ctx_err(ctx, "rdma: create_cq(%u) failed", ctx->cfg.cq_depth);
        return WEFT_CLUSTER_E_IO;
    }

    ibv_qp_init_attr_t ia;
    memset(&ia, 0, sizeof(ia));
    ia.send_cq = ctx->cq;
    ia.recv_cq = ctx->cq;
    ia.qp_type = IBV_QPT_RC;
    ia.cap.max_send_wr = ctx->cfg.qp_depth;
    ia.cap.max_recv_wr = 4;      // one-sided: RQ idle but non-zero (providers)
    ia.cap.max_send_sge = 1;
    ia.cap.max_recv_sge = 1;
    ia.sq_sig_all = 0;
    ctx->qp = ctx->api.create_qp(ctx->pd, &ia);
    if (!ctx->qp) {
        ctx_err(ctx, "rdma: create_qp(RC) failed");
        return WEFT_CLUSTER_E_IO;
    }
    // qp_num: frozen-offset read from the provider's QP head (the
    // handshake MUST carry it; a provider that hides it is refused
    // by name at exchange time)
    ctx->qpn = ((const ibv_qp_t*)ctx->qp)->qp_num;

    // INIT transition (STATE|PKEY_INDEX|PORT|ACCESS_FLAGS)
    ibv_qp_attr_t a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_INIT;
    a.pkey_index = 0;
    a.port = ctx->cfg.port_num;
    a.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
    const int init_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS;
    if (ctx->api.modify_qp(ctx->qp, &a, init_mask) != 0) {
        ctx_err(ctx, "rdma: QP->INIT failed");
        return WEFT_CLUSTER_E_IO;
    }
    ctx->qp_state = IBV_QPS_INIT;
    ctx->stats.qp_seq = 1;

    // WR pool — the whole hot path's allocation budget, spent HERE.
    ctx->pool_size = ctx->cfg.max_outstanding;
    ctx->wr_pool = (struct wr_slot*)calloc(ctx->pool_size,
                                           sizeof(*ctx->wr_pool));
    if (!ctx->wr_pool) {
        ctx_err(ctx, "rdma: WR pool alloc failed");
        return WEFT_CLUSTER_E_NO_MEMORY;
    }
    ctx->err[0] = '\0';
    return WEFT_CLUSTER_OK;
}

weft_cluster_status_t weft_rdma_driver_init(const weft_rdma_config_t* cfg,
                                            weft_rdma_ctx_t* ctx) {
    if (!cfg || !ctx) return WEFT_CLUSTER_E_INVALID_ARG;
    const char* const names[] = { WEFT_RDMA_SONAME, "libibverbs.so" };
    int which = -1;
    void* h = weft_dlopen_first(names, 2, &which);
    if (!h) {
        memset(ctx, 0, sizeof(*ctx));
        ctx->cfg = *cfg;
        ctx_err(ctx, "rdma: %s not loadable — honest refusal "
                     "(install rdma-core or route to io_uring/loopback)",
                WEFT_RDMA_SONAME);
        return WEFT_CLUSTER_E_DRIVER;
    }
    weft_ibv_api_t api;
    const char* missing = NULL;
    if (weft_ibv_api_resolve(h, &api, &missing) != 0) {
        dlclose(h);
        memset(ctx, 0, sizeof(*ctx));
        ctx->cfg = *cfg;
        ctx_err(ctx, "rdma: symbol %s missing — honest refusal", missing);
        return WEFT_CLUSTER_E_DRIVER;
    }
    return rdma_setup(cfg, &api, h, 0, ctx);
}

weft_cluster_status_t weft_rdma_init_with_api(const weft_rdma_config_t* cfg,
                                              const weft_ibv_api_t* api,
                                              weft_rdma_ctx_t* ctx) {
    return rdma_setup(cfg, api, NULL, 1, ctx);
}

void weft_rdma_driver_shutdown(weft_rdma_ctx_t* ctx) {
    if (!ctx || !ctx->dev) {
        if (ctx) { free(ctx->wr_pool); memset(ctx, 0, sizeof(*ctx)); }
        return;
    }
    if (ctx->mr.registered && ctx->api.dereg_mr) {
        // mr.addr is the chunk-0 address; deregister via the mirror the
        // provider handed back at reg time (stored in a side slot).
        if (ctx->mr_handle) ctx->api.dereg_mr((ibv_mr_t*)ctx->mr_handle);
    }
    if (ctx->qp && ctx->api.destroy_qp) ctx->api.destroy_qp(ctx->qp);
    if (ctx->cq && ctx->api.destroy_cq) ctx->api.destroy_cq(ctx->cq);
    if (ctx->pd && ctx->api.dealloc_pd) ctx->api.dealloc_pd(ctx->pd);
    if (ctx->dev && ctx->api.close_device) ctx->api.close_device(ctx->dev);
    if (ctx->dl) dlclose(ctx->dl);
    free(ctx->wr_pool);
    memset(ctx, 0, sizeof(*ctx));
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

weft_cluster_status_t weft_rdma_register_region(weft_rdma_ctx_t* ctx,
                                                const weft_wcr1_region_t* r) {
    if (!ctx || !r) return WEFT_CLUSTER_E_INVALID_ARG;
    if (!ctx->pd) {
        ctx_err(ctx, "rdma: register before init");
        return WEFT_CLUSTER_E_STATE;
    }
    char why[128];
    if (weft_wcr1_validate(r, why, sizeof(why)) != WEFT_WCR1_OK) {
        ctx_err(ctx, "rdma: region refused — %s", why);
        return WEFT_CLUSTER_E_INVALID_ARG;
    }
    void* addr = weft_wcr1_chunk(r, 0);
    const size_t len = (size_t)((uint64_t)r->chunk_count * r->chunk_size);
    ibv_mr_t* mr = ctx->api.reg_mr(ctx->pd, addr, len,
                                   IBV_ACCESS_LOCAL_WRITE |
                                   IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        ctx_err(ctx, "rdma: ibv_reg_mr(%zu bytes) failed", len);
        return WEFT_CLUSTER_E_IO;
    }
    ctx->mr_handle = mr;
    ctx->mr.addr = (uint64_t)(uintptr_t)mr->addr;
    ctx->mr.length = len;
    ctx->mr.lkey = mr->lkey;
    ctx->mr.rkey = mr->rkey;
    ctx->mr.registered = 1;
    ctx->chunk_size = r->chunk_size;
    ctx->chunk_count = r->chunk_count;
    ctx->node_id = r->node_id;
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// WRH1 handshake
// ---------------------------------------------------------------------------

void weft_rdma_handshake_encode(const weft_rdma_ctx_t* ctx,
                                uint8_t wire[WEFT_WRH1_BYTES]) {
    memset(wire, 0, WEFT_WRH1_BYTES);
    wire[0] = 'W'; wire[1] = 'R'; wire[2] = 'H'; wire[3] = '1';
    put_le16(wire + 4, WEFT_WRH1_VERSION);
    put_le16(wire + 6, WEFT_WRH1_BYTES);
    put_le16(wire + 12, (uint16_t)ctx->port_attr.lid);
    put_le16(wire + 14, (uint16_t)ctx->port_attr.active_mtu);
    put_le32(wire + 16, ctx->qpn);
    put_le32(wire + 20, 0);   // our starting PSN (0)
    put_le32(wire + 24, ctx->mr.rkey);
    put_le64(wire + 32, ctx->mr.addr);
    put_le64(wire + 40, ctx->mr.length);
    memcpy(wire + 48, ctx->gid, 16);
    put_le32(wire + 64, ctx->chunk_size);
    put_le32(wire + 68, ctx->chunk_count);
    put_le16(wire + 72, ctx->node_id);
}

int weft_rdma_handshake_decode(const uint8_t wire[WEFT_WRH1_BYTES],
                               weft_rdma_remote_t* out, char* why,
                               size_t whylen) {
    static const uint8_t magic[4] = { 'W', 'R', 'H', '1' };
    if (memcmp(wire, magic, 4) != 0) {
        if (why) snprintf(why, whylen, "wrh1: bad magic");
        return -1;
    }
    if (get_le16(wire + 4) != WEFT_WRH1_VERSION) {
        if (why) snprintf(why, whylen, "wrh1: version %u != 1",
                          get_le16(wire + 4));
        return -1;
    }
    if (get_le16(wire + 6) != WEFT_WRH1_BYTES) {
        if (why) snprintf(why, whylen, "wrh1: hdr_len %u != 128",
                          get_le16(wire + 6));
        return -1;
    }
    for (unsigned i = 76; i < WEFT_WRH1_BYTES; i++) {
        if (wire[i] != 0) {
            if (why) snprintf(why, whylen,
                              "wrh1: reserved byte %d nonzero — refuse", i);
            return -1;
        }
    }
    if (out) {
        memset(out, 0, sizeof(*out));
        out->lid = get_le16(wire + 12);
        out->mtu = get_le16(wire + 14);
        out->qpn = get_le32(wire + 16);
        out->psn = get_le32(wire + 20);
        out->rkey = get_le32(wire + 24);
        out->remote_addr = get_le64(wire + 32);
        out->node_id = get_le16(wire + 72);
        memcpy(out->gid, wire + 48, 16);
        out->chunk_size = get_le32(wire + 64);
        out->chunk_count = get_le32(wire + 68);
    }
    return 0;
}

weft_cluster_status_t weft_rdma_handshake_exchange(weft_rdma_ctx_t* ctx,
                                                   const char* peer_host,
                                                   uint16_t peer_port,
                                                   weft_rdma_remote_t* out) {
    if (!ctx || !out) return WEFT_CLUSTER_E_INVALID_ARG;
    if (!ctx->mr.registered) {
        ctx_err(ctx, "rdma: handshake before register_region");
        return WEFT_CLUSTER_E_STATE;
    }
    // ctx->qpn is set by connect-time... actually the QP exists at init;
    // its number must be read from the provider. QPN is an opaque number
    // the provider assigns at create_qp — the mirror cannot read it from
    // the qp handle portably (it is qpn<<24 in some providers). We
    // require the MOCK to expose it and the real path to use
    // ibv_query_qp? Simpler and honest: the driver records qpn via a
    // dedicated accessor the provider-independent code never guesses.
    if (!ctx->qpn) {
        ctx_err(ctx, "rdma: QPN unknown (provider did not report it)");
        return WEFT_CLUSTER_E_STATE;
    }

    uint8_t mine[WEFT_WRH1_BYTES], theirs[WEFT_WRH1_BYTES];
    weft_rdma_handshake_encode(ctx, mine);

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ctx_err(ctx, "rdma: handshake socket: %s", strerror(errno));
        return WEFT_CLUSTER_E_IO;
    }
    struct timeval tv = {
        .tv_sec = (time_t)(ctx->cfg.poll_timeout_ns / 1000000000ull),
        .tv_usec = (suseconds_t)((ctx->cfg.poll_timeout_ns / 1000) % 1000000),
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int rc = -1;
    if (peer_host) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(peer_port);
        if (inet_pton(AF_INET, peer_host, &sa.sin_addr) != 1) {
            ctx_err(ctx, "rdma: bad peer host '%s'", peer_host);
            close(fd);
            return WEFT_CLUSTER_E_INVALID_ARG;
        }
        rc = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
    } else {
        // server road: bind/accept one connection on handshake_port
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons(ctx->cfg.handshake_port);
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd < 0 || bind(lfd, (struct sockaddr*)&sa, sizeof(sa)) != 0 ||
            listen(lfd, 1) != 0) {
            ctx_err(ctx, "rdma: handshake listen failed: %s",
                    strerror(errno));
            if (lfd >= 0) close(lfd);
            close(fd);
            return WEFT_CLUSTER_E_IO;
        }
        const int cfd = accept(lfd, NULL, NULL);
        close(lfd);
        if (cfd < 0) {
            ctx_err(ctx, "rdma: handshake accept: %s", strerror(errno));
            close(fd);
            return WEFT_CLUSTER_E_IO;
        }
        close(fd);
        // serve on the accepted fd: recv theirs, send mine
        if (recv_full(cfd, theirs, WEFT_WRH1_BYTES) != 0 ||
            send_full(cfd, mine, WEFT_WRH1_BYTES) != 0) {
            ctx_err(ctx, "rdma: handshake io failed");
            close(cfd);
            return WEFT_CLUSTER_E_TIMEOUT;
        }
        close(cfd);
        goto decoded;
    }
    if (rc != 0) {
        ctx_err(ctx, "rdma: handshake connect failed: %s", strerror(errno));
        close(fd);
        return WEFT_CLUSTER_E_IO;
    }
    if (send_full(fd, mine, WEFT_WRH1_BYTES) != 0 ||
        recv_full(fd, theirs, WEFT_WRH1_BYTES) != 0) {
        ctx_err(ctx, "rdma: handshake io failed (timeout %llu ms)",
                (unsigned long long)(ctx->cfg.poll_timeout_ns / 1000000));
        close(fd);
        return WEFT_CLUSTER_E_TIMEOUT;
    }
    close(fd);

decoded:
    {
        char why[128];
        if (weft_rdma_handshake_decode(theirs, out, why,
                                       sizeof(why)) != 0) {
            ctx_err(ctx, "rdma: peer WRH1 refused — %s", why);
            return WEFT_CLUSTER_E_IO;
        }
        return WEFT_CLUSTER_OK;
    }
}

// ---------------------------------------------------------------------------
// connect (INIT -> RTR -> RTS)
// ---------------------------------------------------------------------------

weft_cluster_status_t weft_rdma_connect(weft_rdma_ctx_t* ctx,
                                        const weft_rdma_remote_t* peer) {
    if (!ctx || !peer) return WEFT_CLUSTER_E_INVALID_ARG;
    if (!ctx->qp) {
        ctx_err(ctx, "rdma: connect before init");
        return WEFT_CLUSTER_E_STATE;
    }
    if (ctx->qp_state != IBV_QPS_INIT) {
        ctx_err(ctx, "rdma: QP state %d != INIT (order violation)",
                ctx->qp_state);
        return WEFT_CLUSTER_E_STATE;
    }
    if (peer->rkey == 0 || peer->remote_addr == 0) {
        ctx_err(ctx, "rdma: peer endpoint incomplete (rkey/addr zero)");
        return WEFT_CLUSTER_E_INVALID_ARG;
    }
    ctx->peer = *peer;

    // path MTU: the SMALLER of both ends (the honest floor).
    int mtu = ctx->port_attr.active_mtu;
    if (peer->mtu && peer->mtu < (uint16_t)mtu) mtu = peer->mtu;
    if (mtu < IBV_MTU_512) mtu = IBV_MTU_512;

    // RTR: STATE|AV|PATH_MTU|DEST_QPN|RQ_PSN|MAX_DEST_RD_ATOMIC|MIN_RNR_TIMER
    ibv_qp_attr_t a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTR;
    a.path_mtu = mtu;
    a.dest_qp_num = peer->qpn;
    a.rq_psn = peer->psn;
    a.max_dest_rd_atomic = 1;
    a.min_rnr_timer = 12;   // ~ 640 us; bounded (never the unbounded 31)
    // AH: RoCE (Ethernet link layer) uses GRH with the PEER dgid; IB
    // uses dlid. Build both; provider takes what its link layer needs.
    a.ah_attr.dlid = peer->lid;
    a.ah_attr.port_num = ctx->cfg.port_num;
    a.ah_attr.is_global =
        (ctx->port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) ||
        gid_nonzero(peer->gid);
    memcpy(a.ah_attr.grh.dgid.raw, peer->gid, 16);
    a.ah_attr.grh.sgid_index = (uint8_t)(ctx->gid_index_used < 0
                                             ? 0 : ctx->gid_index_used);
    a.ah_attr.grh.hop_limit = 1;   // RoCEv2 requirement
    const int rtr_mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                         IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                         IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ctx->api.modify_qp(ctx->qp, &a, rtr_mask) != 0) {
        ctx_err(ctx, "rdma: QP->RTR failed");
        return WEFT_CLUSTER_E_IO;
    }
    ctx->qp_state = IBV_QPS_RTR;
    ctx->stats.qp_seq++;

    // RTS: STATE|SQ_PSN|TIMEOUT|RETRY_CNT|RNR_RETRY|MAX_QP_RD_ATOMIC
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTS;
    a.sq_psn = 0;
    a.timeout = 14;        // ~ 67 us ack window (perftest default)
    a.retry_cnt = 7;
    a.rnr_retry = 3;       // BOUNDED (Law 2): never the infinite 7
    a.max_rd_atomic = 1;
    const int rts_mask = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                         IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                         IBV_QP_MAX_QP_RD_ATOMIC;
    if (ctx->api.modify_qp(ctx->qp, &a, rts_mask) != 0) {
        ctx_err(ctx, "rdma: QP->RTS failed");
        return WEFT_CLUSTER_E_IO;
    }
    ctx->qp_state = IBV_QPS_RTS;
    ctx->stats.qp_seq++;
    ctx->connected = 1;
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// the one-sided write data path
// ---------------------------------------------------------------------------

weft_cluster_status_t weft_rdma_post_write(weft_rdma_ctx_t* ctx,
                                           uint32_t src_chunk,
                                           uint32_t src_off, uint32_t len,
                                           uint32_t dst_chunk,
                                           uint64_t user_id) {
    if (!ctx || !ctx->connected) {
        if (ctx) ctx_err(ctx, "rdma: post before connect");
        return WEFT_CLUSTER_E_STATE;
    }
    if (src_chunk >= ctx->chunk_count || dst_chunk >= ctx->peer.chunk_count ||
        (uint64_t)src_off + len > ctx->chunk_size ||
        len == 0) {
        ctx_err(ctx, "rdma: post geometry refused "
                     "(src %u+%u/%u dst %u/%u len %u)",
                src_chunk, src_off, ctx->chunk_count, dst_chunk,
                ctx->peer.chunk_count, len);
        return WEFT_CLUSTER_E_INVALID_ARG;
    }

    // bounded pool: Law 1's answer to flow control
    uint32_t slot_idx = UINT32_MAX;
    for (uint32_t i = 0; i < ctx->pool_size; i++) {
        const uint32_t k = (ctx->pool_hint + i) % ctx->pool_size;
        if (!ctx->wr_pool[k].in_use) { slot_idx = k; ctx->pool_hint = k + 1; break; }
    }
    if (slot_idx == UINT32_MAX) {
        ctx_err(ctx, "rdma: WR pool exhausted (%u outstanding) — BUSY",
                ctx->outstanding);
        return WEFT_CLUSTER_E_BUSY;
    }

    struct wr_slot* s = &ctx->wr_pool[slot_idx];
    s->user_id = user_id;
    s->in_use = 1;
    memset(&s->wr, 0, sizeof(s->wr));
    s->sge.addr = ctx->mr.addr + (uint64_t)src_chunk * ctx->chunk_size +
                  src_off;
    s->sge.length = len;
    s->sge.lkey = ctx->mr.lkey;
    s->wr.wr_id = user_id;
    s->wr.next = NULL;
    s->wr.sg_list = &s->sge;
    s->wr.num_sge = 1;
    s->wr.opcode = IBV_WR_RDMA_WRITE;
    s->wr.send_flags = IBV_SEND_SIGNALED;  // no INLINE: the zero-copy law
    s->wr.wr.rdma.remote_addr = ctx->peer.remote_addr +
                                (uint64_t)dst_chunk * ctx->peer.chunk_size;
    s->wr.wr.rdma.rkey = ctx->peer.rkey;

    ibv_send_wr_t* bad = NULL;
    if (ctx->api.post_send(ctx->qp, &s->wr, &bad) != 0) {
        s->in_use = 0;
        ctx_err(ctx, "rdma: ibv_post_send refused");
        return WEFT_CLUSTER_E_IO;
    }
    ctx->outstanding++;
    ctx->stats.posts++;
    ctx->stats.bytes += len;
    return WEFT_CLUSTER_OK;
}

weft_cluster_status_t weft_rdma_poll(weft_rdma_ctx_t* ctx,
                                     uint64_t timeout_ns,
                                     uint64_t* out_user_id,
                                     uint64_t* out_bytes) {
    if (!ctx) return WEFT_CLUSTER_E_INVALID_ARG;
    if (out_user_id) *out_user_id = 0;
    if (out_bytes) *out_bytes = 0;
    if (!ctx->cq) {
        ctx_err(ctx, "rdma: poll before init");
        return WEFT_CLUSTER_E_STATE;
    }

    ibv_wc_t wc;
    const uint64_t deadline = weft_deadline_after(timeout_ns);
    uint32_t iters = 0;
    for (;;) {
        const int n = ctx->api.poll_cq(ctx->cq, 1, &wc);
        if (n == 1) {
            if (wc.status != IBV_WC_SUCCESS) {
                ctx->stats.wc_errors++;
                ctx_err(ctx, "rdma: completion error %s (wr_id %llu)",
                        wc_status_name(wc.status),
                        (unsigned long long)wc.wr_id);
                free_wr_by_user(ctx, wc.wr_id);
                return WEFT_CLUSTER_E_IO;
            }
            ctx->stats.completions++;
            if (out_user_id) *out_user_id = wc.wr_id;
            if (out_bytes) *out_bytes = wc.byte_len;
            free_wr_by_user(ctx, wc.wr_id);
            if (iters > ctx->stats.max_poll_iters) {
                ctx->stats.max_poll_iters = iters;
            }
            return WEFT_CLUSTER_OK;
        }
        if (weft_deadline_remaining(deadline) == 0) {
            ctx->stats.timeouts++;
            if (iters > ctx->stats.max_poll_iters) {
                ctx->stats.max_poll_iters = iters;
            }
            ctx_err(ctx, "rdma: poll deadline expired after %u iters "
                         "(Law 2 bounded)", iters);
            return WEFT_CLUSTER_E_TIMEOUT;
        }
        iters++;
        weft_poll_yield();
    }
}

weft_cluster_status_t weft_rdma_write_sync(weft_rdma_ctx_t* ctx,
                                           uint32_t src_chunk,
                                           uint32_t src_off, uint32_t len,
                                           uint32_t dst_chunk,
                                           uint64_t user_id,
                                           uint64_t* out_elapsed_ns) {
    const uint64_t t0 = weft_now_ns();
    weft_cluster_status_t st = weft_rdma_post_write(ctx, src_chunk,
                                                    src_off, len, dst_chunk,
                                                    user_id);
    if (st != WEFT_CLUSTER_OK) return st;
    st = weft_rdma_poll(ctx, ctx->cfg.poll_timeout_ns, NULL, NULL);
    if (out_elapsed_ns) *out_elapsed_ns = weft_now_ns() - t0;
    return st;
}

const weft_rdma_stats_t* weft_rdma_stats(const weft_rdma_ctx_t* ctx) {
    return ctx ? &ctx->stats : NULL;
}

const char* weft_rdma_last_error(const weft_rdma_ctx_t* ctx) {
    return ctx ? ctx->err : "null ctx";
}
