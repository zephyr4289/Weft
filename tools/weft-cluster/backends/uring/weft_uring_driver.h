// weft_uring_driver.h — RFC-0019 §5: Linux io_uring batched UDP transport
// with registered buffers (tools layer, C driver module).
//
// WHY EXISTS: RDMA clusters are the target, but commodity cloud nodes
// have no HCA — and the mandate's fallback must still beat the per-frame
// syscall wall. io_uring batches submissions and completions through
// kernel-shared rings (one syscall per BATCH, not per frame) and pins
// the WCR1 chunks ONCE as registered buffers (IORING_REGISTER_BUFFERS)
// so the data path never pays page-pin or copy-setup costs. This module
// is that engine: setup-path binds a connected UDP socket, the steady
// path is submit-harvest loops over pre-mapped SQ/CQ rings.
//
// NO LIBURING (the uring_rx precedent, five reasons, see RFC-0012):
// direct SYS_io_uring_setup/enter/register syscalls against the stable
// uapi — zero link-time deps, every refusal checked and named.
//
// CAPABILITY LADDER (probed at runtime; every rung degrades, Law 4):
//   NONE     io_uring_setup blocked (ENOSYS/EPERM/seccomp) — refuse
//   RING     setup + enter + mmap functional (LIVE on the 5.10 sandbox)
//   FIXED    + IORING_REGISTER_BUFFERS over the WCR1 chunks (pinning
//            moves off the data path; RLIMIT_MEMLOCK is the honest gate)
//   ZC       + IORING_OP_SEND_ZC (kernel >= 5.19): TX without the
//            kernel skb copy. On 5.10 the engine PROBES it, gets
//            -EINVAL on the first CQE, and STICKILY downgrades to
//            WRITE_FIXED — labeled [FALLBACK-COPY] in stats (one skb
//            copy remains; the syscall batching and pinning still hold).
//
// LAW 1: the steady path (send_frame/recv_arm/poll_events) performs
//        ZERO allocations — SQEs are ring slots, in-flight state is a
//        pre-allocated table, events land in caller storage. The CL-U
//        gates arm the malloc-audit interposer around ping-pong loops.
// LAW 2: every wait carries a deadline. Kernel 5.10 lacks
//        IORING_ENTER_EXT_ARG timeouts, so poll_events busy-polls with
//        sched_yield() under a wall-clock deadline (bounded); on >= 5.11
//        the EXT_ARG road is the documented improvement (RFC-0019 §5.4).
// LAW 3: tools layer; core/c untouched (core/c/uring_rx stays the
//        ingestion module; this is the cluster TRANSPORT engine).
// LAW 4: every downgrade is sticky, named, and counted — the bench
//        prints the negotiated rung, never a generic "ok".

#ifndef WEFT_URING_DRIVER_H
#define WEFT_URING_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include "weft_wcr1.h"
#include "weft_cluster_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capability ladder
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_URING_CAP_NONE  = 0,  ///< setup blocked — refuse
    WEFT_URING_CAP_RING  = 1,  ///< rings functional
    WEFT_URING_CAP_FIXED = 2,  ///< + registered buffers live
    WEFT_URING_CAP_ZC    = 3,  ///< + SEND_ZC live (kernel >= 5.19)
} weft_uring_cap_t;

weft_uring_cap_t weft_uring_probe(char* detail, size_t detail_len);
size_t weft_uring_report(char* buf, size_t buflen);

// ---------------------------------------------------------------------------
// Config + session
// ---------------------------------------------------------------------------

typedef struct weft_uring_config {
    uint32_t sq_entries;       ///< SQ depth (power of two; default 128)
    uint64_t poll_timeout_ns;  ///< default bounded-wait deadline (50 ms)
    int      want_zc;          ///< probe SEND_ZC first (default 1)
    int      want_fixed;       ///< probe registered buffers (default 1)
} weft_uring_config_t;

weft_uring_config_t weft_uring_config_default(void);

/// Negotiated rungs, exposed after first use (the honesty record).
typedef enum {
    WEFT_URING_TX_ZC    = 0,   ///< IORING_OP_SEND_ZC + fixed buffer
    WEFT_URING_TX_FIXED = 1,   ///< IORING_OP_WRITE_FIXED (one skb copy)
    WEFT_URING_TX_PLAIN = 2,   ///< IORING_OP_SEND (kernel pin+copy)
} weft_uring_tx_mode_t;

typedef enum {
    WEFT_URING_RX_FIXED = 0,   ///< IORING_OP_READ_FIXED into a chunk
    WEFT_URING_RX_PLAIN = 1,   ///< IORING_OP_READ (kernel pin+copy)
} weft_uring_rx_mode_t;

typedef struct {
    uint64_t submits, enters, completions;
    uint64_t tx_bytes, tx_notifs, rx_frames;
    uint64_t timeouts, eagain, downgrades;
    int      last_errno;
    int      tx_mode, rx_mode, registered, zc;
} weft_uring_stats_t;

/// A harvested completion (caller storage).
typedef struct {
    uint64_t user_data;
    int32_t  res;         ///< bytes, or -errno
    uint8_t  kind;        ///< 0 = tx done, 1 = tx zc-notif, 2 = rx
} weft_uring_ev_t;

typedef struct weft_uring_ctx {
    weft_uring_config_t cfg;
    weft_uring_cap_t    cap;
    int                 ring_fd;
    int                 udp_fd;
    // rings (kernel-shared; the uring_rx layout)
    void*    sq_map;  void* cq_map;  void* sqes_map;
    size_t   sq_map_len, cq_map_len, sqes_map_len;
    uint32_t ring_entries;
    uint32_t *sq_head, *sq_tail, *sq_array, *sq_mask;
    uint32_t *cq_head, *cq_tail, *cq_mask;
    void*    cqes;
    uint32_t sq_cached_head;
    uint32_t in_flight;       ///< submitted-not-harvested (bound: entries)
    // registration
    struct iovec* reg_iov;    ///< per-chunk (setup alloc; Law 1 exempts)
    uint32_t      reg_count;
    weft_wcr1_region_t region;
    int           region_ok;
    // negotiated modes (sticky downgrades)
    weft_uring_tx_mode_t tx_mode;
    weft_uring_rx_mode_t rx_mode;
    weft_uring_stats_t stats;
    struct {
        int      pending;      ///< a ZC-op failure awaits resubmission
        uint32_t chunk, off, len;
        uint64_t user_data;
    } zc_retry;
    char err[192];
} weft_uring_ctx_t;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/// Ring setup only (probe + mmap + rings). Socket + region are separate
/// explicit steps — a bench fabric can hold several contexts.
weft_cluster_status_t weft_uring_driver_init(const weft_uring_config_t* cfg,
                                             weft_uring_ctx_t* ctx);

/// UDP socket bound to INADDR_ANY:port (0 = ephemeral; port back out).
weft_cluster_status_t weft_uring_bind_socket(weft_uring_ctx_t* ctx,
                                             uint16_t* io_port);

/// connect() the UDP socket to the peer (send/write both work after).
weft_cluster_status_t weft_uring_connect_peer(weft_uring_ctx_t* ctx,
                                              const char* ipv4,
                                              uint16_t port);

/// Register the region's chunks as fixed buffers (up to 1024 — the
/// IORING_MAX_REGISTERED_BUFFERS uapi bound; more chunks is a named
/// refusal). Pins once; RLIMIT_MEMLOCK refusals are honestly reported
/// and the engine degrades to TX_PLAIN/RX_PLAIN (sticky, counted).
weft_cluster_status_t weft_uring_register_region(weft_uring_ctx_t* ctx,
                                                 const weft_wcr1_region_t* r);

/// Queue one frame send: `len` bytes from chunk `chunk`+`off`.
/// SQE space is checked; a full SQ is flushed first (bounded).
weft_cluster_status_t weft_uring_send_frame(weft_uring_ctx_t* ctx,
                                            uint32_t chunk, uint32_t off,
                                            uint32_t len, uint64_t user_data);

/// Submit everything queued (one enter call — the batch point).
weft_cluster_status_t weft_uring_flush(weft_uring_ctx_t* ctx);

/// Arm one receive into chunk `chunk` (RX_SQE queued; flush to submit).
weft_cluster_status_t weft_uring_recv_arm(weft_uring_ctx_t* ctx,
                                          uint32_t chunk,
                                          uint64_t user_data);

/// Bounded harvest: CQEs until `min_events` seen, queue drained, or the
/// deadline (Law 2). Events land in caller storage; returns count.
weft_cluster_status_t weft_uring_poll_events(weft_uring_ctx_t* ctx,
                                             uint64_t timeout_ns,
                                             uint32_t min_events,
                                             weft_uring_ev_t* out,
                                             uint32_t cap, uint32_t* out_n);

void weft_uring_driver_shutdown(weft_uring_ctx_t* ctx);

const weft_uring_stats_t* weft_uring_stats(const weft_uring_ctx_t* ctx);
const char* weft_uring_last_error(const weft_uring_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif // WEFT_URING_DRIVER_H
