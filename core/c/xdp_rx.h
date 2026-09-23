// xdp_rx.h — RFC-0016 §4: AF_XDP UMEM-as-ring-slot kernel-bypass ingestion
// (C driver layer).
//
// WHY EXISTS: uring_rx made the KERNEL the bracket-filler (recv -> slot, one
// syscall); the mandate's next rung makes the NIC itself the filler — the
// network card DMAs the incoming datagram STRAIGHT into the ring slot's
// payload pages, which are registered as AF_XDP UMEM. The packet's journey
// becomes: NIC DMA -> ring slot (the ONLY copy) -> descriptor in the shared
// rx ring -> publish(). No recv(), no staging, no scratch — and in the
// busy-poll steady state, zero syscalls per frame (the rx/fill rings are
// shared memory; need_wakeup gates the rare syscall).
//
// THE PRE-BRACKETED FILL RING (the protocol that keeps I1-I8 intact):
//   The bracket's whole point is that a reader arriving mid-fill sees
//   slotSeq == 0 and takes the frozen skip path. With a NIC as the filler,
//   the DMA can land at ANY time after the buffer is handed to the driver —
//   so the buffer is handed over ONLY while the bracket is OPEN:
//
//     publish(frame s)          -- slot (s-1)%M closed, live for readers
//     begin()                   -- slot s%M INVALIDATED (SeqCst + fence: P1)
//     post fill(slot s%M)       -- the invalidated slot's payload offset
//                                  joins the fill ring; the NIC may now DMA
//     ... NIC DMAs the packet into slot s%M ...  (readers: frozen skip)
//     rx descriptor arrives     -- addr says WHICH slot; the state machine
//                                  verifies it is the in-flight slot
//     publish()                 -- stamp + latestSeq flip (Release)
//
//   Depth is ONE by construction: the frozen publish() stamps the most
//   recent begin()'s pair only (the same soundness finding uring_rx's
//   RFC-0004 depth-1 analysis reached — out-of-order multi-fill cannot be
//   expressed without breaking the frozen API, and is not needed: the
//   fill ring's depth paces the DRIVER, not the ring).
//
// UMEM GEOMETRY (the constraint that shapes attach):
//   The UMEM region IS the session span (WFSH header + ring) — the ring's
//   own pages, page-aligned (shm / dma-buf / memfd backed; a malloc'd ring
//   is refused: its base lives below page alignment and the session header
//   would sit outside any pinnable region — the same road wrap_host takes).
//   Slot payload k lives at session offset 80 + 8M + k*payload_bytes — NOT
//   chunk-aligned for any useful geometry — so the module uses UNALIGNED
//   chunk mode (kernel >= 5.7: arbitrary buffer placement inside the UMEM,
//   XSK_UNALIGNED_BUF_OFFSET_FLAG encoding; chunk_size = payload_bytes).
//
// CAPABILITY LADDER (probed once, every rung degrades, Law 4):
//   NONE     no AF_XDP in the kernel (socket() -> EAFNOSUPPORT — the
//            x86_64 CI/sandbox kernels ship without CONFIG_XDP_SOCKETS)
//            -> attach DELEGATES to uring_rx's own ladder (SYSCALL/RING/
//            FIXED), which stays fully live: the module is a drop-in
//            upgrade, never a regression
//   SETUP    xsk + UMEM_REG + fill/rx ring mmaps functional (no traffic
//            proof — bind may still need privileges)
//   LIVE     rx descriptors observed under real traffic (needs an XDP
//            program on the interface — CAP_BPF/CAP_NET_ADMIN; the
//            privileged-runner proof script drives the veth+XDP loop)
//
// LAW 1: next() is bounded — one rx-ring check, one need_wakeup gate; no
//        spins, no indefinite waits (the busy-poll discipline is the
//        caller's threading policy, not this module's).
// LAW 2: the data path allocates nothing (rings pre-mapped at attach; the
//        fill entry is written into the pre-mapped fill ring).
// LAW 3: driver layer; weft.c/weft.h untouched.
// LAW 4: every refusal (no AF_XDP, non-page-aligned ring, misordered
//        descriptor, oversize frame) is counted and reported, never silent.
//
// Honesty boundary: the sandbox kernel has no AF_XDP at all — there, the
// probe reports NONE and the uring delegation is EXECUTABLE-VERIFIED (the
// X-series gates drive real UDP loopback traffic through it). The xsk
// plumbing compiles everywhere Linux does; the SETUP/LIVE legs run on
// kernels built with CONFIG_XDP_SOCKETS (the xdp_loopback_proof.sh rig
// drives LIVE end-to-end under privileges — DECLARED here, like uring_rx's
// >= 5.19 FIXED rung and gpu_ring's real-GPU numbers).

#ifndef WEFT_XDP_RX_H
#define WEFT_XDP_RX_H

#include <stddef.h>
#include <stdint.h>

#include "fanout.h"
#include "uring_rx.h"

#if defined(__linux__)
#define WEFT_XDP_RX_LINUX 1
#else
#define WEFT_XDP_RX_LINUX 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capability ladder
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_XDP_MODE_NONE  = 0,  ///< no AF_XDP in the kernel — uring delegation
    WEFT_XDP_MODE_URING = 1,  ///< attached via uring_rx (its own ladder)
    WEFT_XDP_MODE_SETUP = 2,  ///< xsk + UMEM + rings functional
    WEFT_XDP_MODE_LIVE  = 3,  ///< rx descriptors observed (BPF attached)
} weft_xdp_mode_t;

/// Probe the ladder once (cached). socket(AF_XDP) is attempted for real;
/// NONE means the kernel lacks the family (EAFNOSUPPORT) — the documented
/// CI/sandbox state. Advisory (AXIOM T): attach re-derives its own mode.
weft_xdp_mode_t weft_xdp_probe(void);

/// Human-readable mode name ("none"/"uring"/"setup"/"live").
const char* weft_xdp_mode_str(weft_xdp_mode_t m);

/// One capability line for evidence logs.
size_t weft_xdp_report(char* buf, size_t buflen);

// ---------------------------------------------------------------------------
// Ingestion session
// ---------------------------------------------------------------------------

/// Advisory per-session statistics (AXIOM T) — the honesty record for the
/// NIC-DMA -> frame mapping.
typedef struct {
    uint64_t frames;       ///< packets accepted and published
    uint64_t bytes;        ///< payload bytes ingested (post-truncation)
    uint64_t padded;       ///< packets shorter than payload_bytes (tail zeroed
                           ///<  — the NIC writes only len bytes; the tail is
                           ///<  zeroed under the open bracket before publish)
    uint64_t truncated;    ///< packets longer than payload_bytes (head kept)
    uint64_t fill_posts;   ///< slot payloads posted to the fill ring
    uint64_t rx_descs;     ///< rx descriptors consumed (LIVE mode)
    uint64_t publishes;    ///< brackets closed by publish_desc
    uint64_t misordered;   ///< descriptors for a non-in-flight slot (refused)
    uint64_t wakeups;      ///< need_wakeup recvmsg() calls
    uint64_t delegated;    ///< frames ingested via the uring fallback
    int last_errno;        ///< most recent refusal errno (0 = none)
} weft_xdp_stats_t;

/// An ingestion session: one fan-out broadcaster + (LIVE) one xsk whose
/// UMEM is the ring's own session pages, or (URING fallback) one datagram
/// fd driven through weft_uring_rx.
/// SINGLE CONSUMER BY CONTRACT (one next() caller — the single-writer ring
/// contract above it, exactly like uring_rx).
typedef struct weft_xdp_rx {
    weft_fanout_t* fan;         ///< borrowed broadcaster (writer side)
    weft_xdp_mode_t mode;       ///< negotiated at attach
    size_t payload_bytes;       ///< cached geometry
    unsigned slot_count;
    uint8_t* session_base;      ///< ring - 64 (the UMEM base; NULL when delegated)
    size_t session_span;        ///< 64 + ring_bytes (logical)
    size_t umem_bytes;          ///< page-rounded span actually registered
    uint64_t in_flight_umem;    ///< the posted slot's UMEM offset (UINT64_MAX = none)
    uint64_t in_flight_slot;    ///< which slot index it is
    // xsk plumbing (SETUP/LIVE modes; zero otherwise). Hand-rolled uapi —
    // no libbpf/libxdp dependency (the uring_rx discipline).
    int xsk_fd;
    void* fill_map;             ///< fill ring mmap (XDP_UMEM_PGOFF_FILL_RING)
    void* rx_map;               ///< rx ring mmap (XDP_PGOFF_RX_RING)
    size_t fill_map_bytes, rx_map_bytes;
    uint32_t fill_size, rx_size;
    volatile uint32_t* fill_prod;   ///< ours (we write, release)
    volatile uint32_t* fill_cons;   ///< kernel's (we read)
    volatile uint32_t* rx_prod;     ///< kernel's (we read)
    volatile uint32_t* rx_cons;     ///< ours (we write, release)
    uint32_t fill_mask, rx_mask;
    uint64_t* fill_desc;           ///< fill ring entry array (u64 addrs)
    struct weft_xdp_desc {          // if_xdp.h's xdp_desc, local dialect
        uint64_t addr;
        uint32_t len;
        uint32_t options;
    }* rx_desc;
    weft_uring_rx_t uring;      ///< the fallback (URING mode)
    weft_xdp_stats_t stats;
} weft_xdp_rx_t;

/// Attach an ingestion session.
///   f:  an initialized broadcaster whose ring lives in PAGE-ALIGNED shared
///       memory (shm_ring / weft_dmabuf / memfd session — attach over the
///       ring BYTES; the module derives the session base at ring-64 and
///       validates the WFSH header). A malloc'd ring CANNOT host UMEM and
///       is delegated to the uring ladder (counted, never silent).
///   fd: a DATAGRAM socket for the fallback ladder (may be -1 to refuse
///       delegation — XDP-only callers get an honest attach failure when
///       the kernel lacks AF_XDP).
/// Returns 0; -1 with rx->stats.last_errno set on refusal (bad geometry,
/// no backend at all).
int weft_xdp_attach(weft_xdp_rx_t* rx, weft_fanout_t* f, int fd);

/// Open the next bracket AND post its slot to the fill ring: begin()
/// (invalidate, SeqCst+fence) then the fill-ring entry. Returns the posted
/// UMEM offset, or 0 when the session is delegated/not attached. The
/// kernel may DMA into the slot from this call until publish_desc().
uint64_t weft_xdp_begin_fill(weft_xdp_rx_t* rx);

/// THE state-machine transition (the seam next() drives and tests inject):
/// an rx descriptor arrived for `umem_addr` carrying `len` bytes. Verifies
/// the address is the in-flight slot (misorder = refused + counted), pads
/// or truncates per the slot geometry, closes the bracket (publish —
/// Release), and returns the published frame seq (0 on refusal).
uint64_t weft_xdp_publish_desc(weft_xdp_rx_t* rx, uint64_t umem_addr, uint32_t len);

/// Ingest ONE frame:
///   LIVE:  consume an rx descriptor (shared ring; no syscall in the
///          steady state — need_wakeup gates the rare recvmsg), publish,
///          open the next bracket
///   URING: delegate to weft_uring_next (its own documented ladder)
/// Returns the published frame seq, or 0 when nothing was ready. Never
/// blocks unboundedly (Law 1).
uint64_t weft_xdp_next(weft_xdp_rx_t* rx);

/// Detach (munmap rings, close the xsk, or detach the uring fallback).
/// Idempotent. The broadcaster stays the caller's.
void weft_xdp_detach(weft_xdp_rx_t* rx);

/// Advisory stats snapshot (AXIOM T).
void weft_xdp_stats(const weft_xdp_rx_t* rx, weft_xdp_stats_t* out);

#ifdef __cplusplus
}
#endif

#endif // WEFT_XDP_RX_H
