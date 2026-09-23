// weft_xdp_driver.h — RFC-0019 §4: eBPF/XDP line-rate kernel-bypass
// ingestion into WCR1 memory (tools layer, C driver module).
//
// WHY EXISTS: the io_uring road still pays one syscall per batch and one
// kernel copy per datagram. The XDP road removes both: an eBPF filter
// attached at the NIC driver classifies incoming .weft frames at 100
// Gbps line rate (BEFORE the TCP/IP stack), and AF_XDP sockets hand the
// driver's page fragments to user space through shared-memory rings —
// the NIC's own DMA lands the frame DIRECTLY in the WCR1 UMEM chunk.
// The receiving CPU sees no interrupt, no syscall, no copy: the rx ring
// descriptor is the only evidence a frame arrived.
//
// THE THREE MIRRORS (the load-bearing invariant): the kernel-side BPF
// source (bpf/weft_xdp_filter.c), the embedded bytecode this driver
// ASSEMBLES at init (no BPF toolchain needed on the node), and the
// user-space validator weft_wcf1_validate() all parse the same WCF1
// fields at the same offsets. The CL-X gates drive the user-space pair
// against golden vectors on every run; a drift is a gate failure.
//
// CAPABILITY LADDER (probed once; every rung degrades — Law 4):
//   NONE    no AF_XDP family in the kernel (EAFNOSUPPORT — the CI
//           sandbox state) OR no CAP_BPF/CAP_NET_ADMIN for prog load:
//           init REFUSES by name and the fabric routes to io_uring
//   SETUP   xsk + UMEM_REG + fill/rx rings functional (bind may still
//           be privileged; the driver reports which sub-step refused)
//   LIVE    filter loaded + attached + XSKMAP bound (needs the caps on
//           a CONFIG_XDP_SOCKETS kernel — the hardware-runner leg)
//
// PLACEMENT LAW (the UMEM contract, stricter than the WCR1 floor): the
// XDP road requires chunk0_offset and chunk_size to be PAGE multiples
// (4096) — the steering ring is a dedicated cluster region (Engineer 1's
// DMA-facing road), not a bridged fanout session. RDMA and io_uring
// accept the WFSH bridge; XDP refuses it BY NAME (chunk0_align rung) —
// honest narrowness beats fragile generality (the unaligned-chunk
// encoding exists, and is exactly the kind of per-kernel mine the
// mandate's honesty rule exists to refuse).
//
// LAW 1: the rx path allocates nothing — rings are pre-mapped, the free
//        chunk stack is pre-allocated, batches land in caller storage.
// LAW 2: rx is deadline-bounded; need_wakeup syscalls only when the
//        kernel asks for them (XDP_USE_NEED_WAKEUP).
// LAW 3: tools layer; core/c untouched.
// LAW 4: every refusal (caps, family, geometry, filter load, attach) is
//        named, counted, and never a fake pass.

#ifndef WEFT_XDP_DRIVER_H
#define WEFT_XDP_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include <linux/bpf.h>     // struct bpf_insn, union bpf_attr (stable uapi)

#include "weft_wcr1.h"
#include "weft_cluster_core.h"
#include "weft_cluster_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capability ladder
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_XDP_CAP_NONE  = 0,  ///< no family / no caps — honest refusal
    WEFT_XDP_CAP_SETUP = 1,  ///< xsk + UMEM + rings functional
    WEFT_XDP_CAP_LIVE  = 2,  ///< filter attached + steering bound
} weft_xdp_cap_t;

/// Probe (cached): capabilities + AF_XDP family presence + a real
/// BPF_MAP_CREATE attempt (EPERM -> PERMS refusal; the honest rung).
weft_xdp_cap_t weft_xdp_probe(char* detail, size_t detail_len);

/// One capability line for evidence logs.
size_t weft_xdp_report(char* buf, size_t buflen);

// ---------------------------------------------------------------------------
// Config + session
// ---------------------------------------------------------------------------

typedef struct weft_xdp_config {
    const char* ifname;       ///< "eth0"; NULL = "lo" (tests)
    uint32_t    cluster_id;   ///< steering key (XDP filter immediate)
    uint32_t    schema_id;    ///< 0 = any schema (filter block omitted)
    uint16_t    udp_port;     ///< WEFT_CLUSTER_UDP_PORT default
    uint32_t    queue_id;     ///< NIC RX queue -> xsks_map key
    uint32_t    fill_depth;   ///< fill ring depth (power of two)
    uint32_t    comp_depth;   ///< completion ring depth (power of two)
    uint32_t    rx_depth;     ///< rx ring depth (power of two)
    uint32_t    xdp_flags;    ///< extra attach flags (XDP_FLAGS_SKB_MODE..)
    uint64_t    poll_timeout_ns; ///< bounded waits (default 50 ms)
} weft_xdp_config_t;

weft_xdp_config_t weft_xdp_config_default(void);

/// Advisory stats (AXIOM T) — the honesty record of the ingress road.
typedef struct {
    uint64_t rx_descs;        ///< rx-ring descriptors observed
    uint64_t steered;         ///< frames passing WCF1 validation
    uint64_t refused_frames;  ///< WCF1 refusals (defense in depth)
    uint64_t recycled;        ///< chunks returned to the fill ring
    uint64_t wakeups;         ///< need_wakeup recvfrom() kicks
    uint64_t fills;           ///< fill-ring entries posted
    int      last_errno;      ///< most recent refusal errno
} weft_xdp_stats_t;

/// One received frame (borrowed pointers into the region — valid until
/// the chunk is recycled).
typedef weft_cluster_rx_ent_t weft_xdp_rx_ent_t;

typedef struct weft_xdp_socket {
    weft_xdp_config_t  cfg;
    weft_wcr1_region_t region;      ///< borrowed
    weft_xdp_cap_t     cap;
    int                xsk_fd;
    int                map_fd;      ///< XSKMAP
    int                prog_fd;     ///< XDP prog
    int                link_fd;     ///< bpf_link
    unsigned           ifindex;
    int                copy_mode;   ///< 1 = XDP_COPY fallback ([FALLBACK-COPY])
    // rings (kernel-shared; pre-mapped at init)
    uint32_t* rx_prod, * rx_cons, * rx_flags;
    uint64_t* rx_desc;              ///< xdp_desc array (addr,len,opts)
    uint32_t* fr_prod, * fr_cons;   ///< fill ring
    uint64_t* fr_desc;
    uint32_t* cr_prod, * cr_cons;   ///< completion ring (TX road)
    uint64_t* cr_desc;
    uint32_t  ring_mask;
    size_t    ring_len_fr, ring_len_cr, ring_len_rx;
    // free chunk stack (pre-allocated; the recycle currency)
    uint32_t* free_stack;
    uint32_t  free_count;
    weft_xdp_stats_t stats;
    char      err[192];
} weft_xdp_socket_t;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/// Init: capability probe -> xsk socket -> UMEM_REG over the region ->
/// ring mmaps -> bind (queue). The FILTER is a separate, explicit step
/// (weft_xdp_load_and_attach) so a node can run ring-only SETUP mode.
weft_cluster_status_t weft_xdp_driver_init(const weft_xdp_config_t* cfg,
                                           const weft_wcr1_region_t* r,
                                           weft_xdp_socket_t* s);

/// Assemble the embedded filter bytecode for a config (PURE — the
/// CL-X structural gates call it directly). `insns` is caller storage
/// (<= 64 entries needed); the XSKMAP fd is embedded via BPF_PSEUDO_
/// MAP_FD at the two load sites. Returns insn count or -1 (config
/// refused / storage too small).
int weft_xdp_build_filter(const weft_xdp_config_t* cfg,
                          struct bpf_insn* insns, uint32_t max, int map_fd);

/// Load + attach: BPF_MAP_CREATE(XSKMAP) -> BPF_PROG_LOAD(XDP, the
/// assembled bytecode, license "GPL") -> MAP_UPDATE(queue -> xsk fd)
/// -> BPF_LINK_CREATE(ifindex). Each refusal is named (PERMS leads on
/// unprivileged runners; the gate asserts exactly that).
weft_cluster_status_t weft_xdp_load_and_attach(weft_xdp_socket_t* s);

/// Post n free chunks to the fill ring (bounded by free_count).
weft_cluster_status_t weft_xdp_populate_fill(weft_xdp_socket_t* s,
                                             uint32_t n);

/// Bounded rx: harvest descriptors until the ring drains or the
/// deadline passes; each frame is WCF1-validated (defense in depth —
/// the BPF filter already matched, this is the second opinion) and
/// returned in caller storage. Refused frames are auto-recycled and
/// counted per rung in stats.refusal_histogram (indexed by
/// weft_wcf1_refusal_t).
weft_cluster_status_t weft_xdp_rx(weft_xdp_socket_t* s,
                                  uint64_t timeout_ns,
                                  weft_xdp_rx_ent_t* out, uint32_t cap,
                                  uint32_t* out_n);

/// Return chunks to the fill ring (after the consumer is done with the
/// payload). Bounded by the free-stack capacity (chunk_count).
weft_cluster_status_t weft_xdp_recycle(weft_xdp_socket_t* s,
                                       const uint32_t* chunks, uint32_t n);

void weft_xdp_driver_shutdown(weft_xdp_socket_t* s);

const weft_xdp_stats_t* weft_xdp_stats(const weft_xdp_socket_t* s);
const char* weft_xdp_last_error(const weft_xdp_socket_t* s);

#ifdef __cplusplus
}
#endif

#endif // WEFT_XDP_DRIVER_H
