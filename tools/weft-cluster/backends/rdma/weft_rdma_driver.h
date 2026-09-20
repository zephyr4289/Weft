// weft_rdma_driver.h — RFC-0019 §3: RoCEv2 / InfiniBand one-sided RDMA
// engine over Engineer 1's WCR1 region (tools layer, C driver module).
//
// WHY EXISTS: the cluster's latency budget is sub-microsecond
// cross-server memory synchronization. Kernel sockets — even the
// io_uring road — pay a full network-stack traversal plus a receiver
// wakeup per message. RDMA one-sided writes remove BOTH: the sender's
// NIC DMAs payload bytes directly into the RECEIVER'S physical memory
// (registered as an ibv memory region), and the receiver's CPU is never
// interrupted — no interrupt, no syscall, no wakeup. The WCR1 ring on
// node B "just advances" as if a local writer had published. This module
// is that engine:
//
//     weft_rdma_driver_init()          dlopen libibverbs, PD/CQ/QP setup
//     weft_rdma_register_region()      ibv_reg_mr over the WCR1 span
//     weft_rdma_handshake_*()          WRH1 wire exchange (qpn/rkey/addr)
//     weft_rdma_connect()              QP: INIT -> RTR -> RTS
//     weft_rdma_post_write()           IBV_WR_RDMA_WRITE (one-sided)
//     weft_rdma_poll()                 bounded ibv_poll_cq (Law 2)
//
// DYNAMIC LOADER DISCIPLINE (the mandate's explicit rule): libibverbs is
// loaded with dlopen at runtime — ZERO link-time dependency. When
// libibverbs.so.1 is absent (commodity cloud instances, the CI sandbox)
// init REFUSES HONESTLY with WEFT_CLUSTER_E_DRIVER and the fabric
// cascade routes to io_uring/loopback — never a crash, never a fake
// pass. When the library loads but no HCA is present,
// WEFT_CLUSTER_E_HW_ABSENT. Both rungs are gated (CL-R1/CL-R2).
//
// LAW 1 (zero hot-path allocations): the WR pool, SGEs, CQ scratch and
//        the stats block are pre-allocated at INIT; post_write/poll
//        touch only that storage. The CL-R8 gate arms the malloc-audit
//        interposer around a post+poll loop and asserts ZERO.
// LAW 2 (bounded latency): every poll carries a caller deadline; the
//        driver never blocks in the kernel (no get_cq_event — busy poll
//        with a deadline), and QP retry policy is bounded (rnr_retry=3,
//        never the unbounded 7). CL-R6 times out a stalled CQ on purpose.
// LAW 3: tools layer; core/c untouched (kernel-freeze gate).
// LAW 4: named refusals (driver/hw/perms/state), QP transition order
//        enforced INIT->RTR->RTS (out-of-order is WEFT_CLUSTER_E_STATE,
//        CL-R5), and the mock battery proves the state machine without
//        hardware. The REAL-hardware loopback leg is DECLARED here and
//        runs on RDMA-capable runners (D-32 §hardware checklist).
//
// Honesty boundary (this sandbox): no rdma-core, no HCA — every gate
// that needs the real library runs through the MOCK vtable (the same
// table-shape discipline as the ORT mock of RFC-0017 §5), plus the two
// refusal gates against the real (absent) library. RDMA NIC numbers are
// HARDWARE-DEFERRED; the harness (weft-cluster-bench --transport rdma)
// ships ready.

#ifndef WEFT_RDMA_DRIVER_H
#define WEFT_RDMA_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include "weft_wcr1.h"
#include "weft_cluster_core.h"
#include "weft_rdma_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Candidate shared objects, in order (house dlopen rule).
#define WEFT_RDMA_SONAME "libibverbs.so.1"

/// WRH1 — the out-of-band handshake wire format (128 bytes, LE):
///   0  magic "WRH1"      4  version u16 (=1)   6  hdr_len u16 (=128)
///   8  flags u32        12  lid u16            14  mtu u16 (ibv_mtu)
///  16  qpn u32          20  psn u32            24  rkey u32
///  28  pad u32          32  mr_addr u64        40  mr_len u64
///  48  gid[16]          64  chunk_size u32     68  chunk_count u32
///  72  node_id u16      74  reserved u16       76..127 reserved zero
#define WEFT_WRH1_BYTES 128u
#define WEFT_WRH1_VERSION 1u

typedef struct weft_rdma_config {
    const char* device_name;   ///< NULL = first device with a valid port
    uint8_t      port_num;     ///< HCA port (default 1)
    int          gid_index;    ///< -1 = auto (first non-link-local RoCE gid)
    uint32_t     cq_depth;     ///< CQEs (default 64)
    uint32_t     qp_depth;     ///< max_send_wr (default 64)
    uint32_t     max_outstanding; ///< WR pool size (default 32; Law 1 bound)
    uint64_t     poll_timeout_ns; ///< default write-sync deadline (50 ms)
    uint16_t     handshake_port;  ///< WRH1 TCP port (default 47912)
} weft_rdma_config_t;

weft_rdma_config_t weft_rdma_config_default(void);

/// A registered memory region (the local side of the contract).
typedef struct {
    uint64_t addr;      ///< region base + chunk0_offset (chunk 0 address)
    uint64_t length;    ///< registered span from chunk 0
    uint32_t lkey;      ///< local key (send side)
    uint32_t rkey;      ///< remote key (exported to the peer)
    uint8_t  registered;
} weft_rdma_mr_t;

/// The peer's WRH1-decoded endpoint.
typedef struct {
    uint32_t qpn;
    uint32_t psn;
    uint32_t rkey;
    uint64_t remote_addr;   ///< peer chunk-0 address (their geometry)
    uint16_t lid;
    uint16_t mtu;           ///< ibv_mtu enum value
    uint8_t  gid[16];
    uint16_t node_id;
    uint32_t chunk_size;    ///< peer WCR1 chunk geometry (placement law)
    uint32_t chunk_count;
} weft_rdma_remote_t;

/// Advisory stats (AXIOM T) — the honesty record for the write pipeline.
typedef struct {
    uint64_t posts;        ///< post_send calls accepted
    uint64_t completions;  ///< successful poll_cq harvests
    uint64_t timeouts;     ///< Law-2 deadline expiries
    uint64_t wc_errors;    ///< completions with bad status
    uint64_t bytes;        ///< payload bytes posted
    uint32_t max_poll_iters; ///< worst observed poll spin (determinism)
    uint32_t qp_seq;       ///< modify_qp call count (state machine proof)
} weft_rdma_stats_t;

typedef struct weft_rdma_ctx {
    weft_rdma_config_t cfg;
    weft_ibv_api_t     api;        ///< resolved or injected vtable
    void*              dl;         ///< dlopen handle (NULL when injected)
    int                injected;   ///< 1 = mock table (tests)
    // provider objects
    struct ibv_context* dev;
    struct ibv_pd*      pd;
    struct ibv_cq*      cq;
    struct ibv_qp*      qp;
    uint32_t            qpn;
    ibv_port_attr_t     port_attr;
    uint8_t             gid[16];
    int                 gid_index_used;
    int                 qp_state;   ///< last modify_qp state (mirror)
    // registration + connection
    weft_rdma_mr_t     mr;
    void*              mr_handle;    ///< provider ibv_mr (opaque mirror)
    uint16_t           node_id;      ///< WCR1 identity (register_region)
    uint32_t           chunk_size;   ///< local WCR1 geometry (registered)
    uint32_t           chunk_count;
    weft_rdma_remote_t peer;
    int                connected;
    // WR pool (pre-allocated at init — Law 1)
    struct wr_slot {
        ibv_send_wr_t wr;
        ibv_sge_t     sge;
        uint64_t      user_id;
        uint8_t       in_use;
    }*                 wr_pool;
    uint32_t           pool_size;
    uint32_t           pool_hint;
    uint32_t           outstanding;
    weft_rdma_stats_t  stats;
    char               err[192];
} weft_rdma_ctx_t;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/// Cheap probe: dlopen attempt + device count. NO resources are kept.
/// Returns WEFT_CLUSTER_OK when a device is visible, or the honest
/// refusal (DRIVER / HW_ABSENT / IO). `detail` (may be NULL) gets a
/// one-line evidence string.
weft_cluster_status_t weft_rdma_probe(char* detail, size_t detail_len);

/// Full driver init (dlopen + resolve + PD/CQ/QP in INIT state).
weft_cluster_status_t weft_rdma_driver_init(const weft_rdma_config_t* cfg,
                                            weft_rdma_ctx_t* ctx);

/// TEST SEAM: init with an injected vtable (no dlopen). The mock battery
/// (test_rdma.c) drives the full pipeline through this door; production
/// code never touches it.
weft_cluster_status_t weft_rdma_init_with_api(const weft_rdma_config_t* cfg,
                                              const weft_ibv_api_t* api,
                                              weft_rdma_ctx_t* ctx);

/// Teardown (idempotent; deregisters the MR if still registered).
void weft_rdma_driver_shutdown(weft_rdma_ctx_t* ctx);

// ---------------------------------------------------------------------------
// Registration + connection (the WRH1 protocol)
// ---------------------------------------------------------------------------

/// ibv_reg_mr the region [chunk 0, span) with LOCAL_WRITE|REMOTE_WRITE
/// (the receiver-side requirement for one-sided writes; the sender-side
/// registration covers its own SGE addresses — same call, same law).
weft_cluster_status_t weft_rdma_register_region(weft_rdma_ctx_t* ctx,
                                                const weft_wcr1_region_t* r);

/// Encode our side of the WRH1 handshake (pure; golden-tested).
void weft_rdma_handshake_encode(const weft_rdma_ctx_t* ctx,
                                uint8_t wire[WEFT_WRH1_BYTES]);

/// Decode a peer wire block (pure; validates magic/version/hdr_len and
/// reserved-zero). Returns 0/-1 with `why` naming the refusal.
int weft_rdma_handshake_decode(const uint8_t wire[WEFT_WRH1_BYTES],
                               weft_rdma_remote_t* out, char* why,
                               size_t whylen);

/// Out-of-band WRH1 exchange over TCP (both directions, bounded by the
/// config poll_timeout_ns via SO_RCVTIMEO — Law 2 applies to setup too).
/// `peer_host` may be NULL on the "server" side to accept one connection
/// on the handshake port instead of connecting out.
weft_cluster_status_t weft_rdma_handshake_exchange(weft_rdma_ctx_t* ctx,
                                                   const char* peer_host,
                                                   uint16_t peer_port,
                                                   weft_rdma_remote_t* out);

/// Transition the QP to RTS (INIT -> RTR -> RTS; per-connection once).
weft_cluster_status_t weft_rdma_connect(weft_rdma_ctx_t* ctx,
                                        const weft_rdma_remote_t* peer);

// ---------------------------------------------------------------------------
// The one-sided write data path (Law 1: zero allocations)
// ---------------------------------------------------------------------------

/// Post one IBV_WR_RDMA_WRITE: `len` bytes from local chunk
/// `src_chunk`+`src_off` to remote address peer.remote_addr +
/// dst_chunk*chunk_size. `user_id` rides the WR and returns from poll.
/// WEFT_CLUSTER_E_BUSY when the pool is exhausted (bounded, never
/// queued unboundedly — the Law-1 answer to flow control).
weft_cluster_status_t weft_rdma_post_write(weft_rdma_ctx_t* ctx,
                                           uint32_t src_chunk,
                                           uint32_t src_off, uint32_t len,
                                           uint32_t dst_chunk,
                                           uint64_t user_id);

/// Bounded poll for ONE completion (deadline = now + timeout_ns).
/// Returns OK (+out_user_id/out_bytes) / TIMEOUT / IO (WC error).
weft_cluster_status_t weft_rdma_poll(weft_rdma_ctx_t* ctx,
                                     uint64_t timeout_ns,
                                     uint64_t* out_user_id,
                                     uint64_t* out_bytes);

/// post_write + poll in one call (bench convenience; same laws).
weft_cluster_status_t weft_rdma_write_sync(weft_rdma_ctx_t* ctx,
                                           uint32_t src_chunk,
                                           uint32_t src_off, uint32_t len,
                                           uint32_t dst_chunk,
                                           uint64_t user_id,
                                           uint64_t* out_elapsed_ns);

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

const weft_rdma_stats_t* weft_rdma_stats(const weft_rdma_ctx_t* ctx);
const char* weft_rdma_last_error(const weft_rdma_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif // WEFT_RDMA_DRIVER_H
