// uring_rx.h — RFC 0012: kernel-bypass zero-copy datagram ingestion into the
// fan-out ring (C driver layer)
//
// WHY EXISTS: Weft's data sources are streams — telemetry packets, L2 order
// updates, frame chunks off a socket. The stock ingestion path is
// recv(fd, scratch, n) + memcpy(scratch -> ring slot): two copies, one
// syscall, and a scratch buffer the allocator owns. This module makes the
// KERNEL itself the filler:
//
//     weft_fanout_begin()      // FI1 bracket OPENS: slotSeq[k] <- 0 (SeqCst)
//        |
//        |  kernel copies the datagram DIRECTLY into the slot's payload
//        |  (the cursor begin() returned — no scratch, no second copy)
//        v
//     weft_fanout_publish()    // bracket CLOSES: stamp + latestSeq (Release)
//
// Readers that arrive while the kernel is mid-copy observe slotSeq == 0 and
// take the frozen skip path (Law 1) — exactly the mid-overwrite semantics
// the ring was designed for. The writer is "the kernel" for the duration of
// the fill; I1-I8 hold unchanged (the bracket, not the filler's identity,
// carries the invariants — Volume I §3).
//
// CAPABILITY LADDER (probed at runtime, every rung degrades, Law 4):
//   NONE      no ingestion backend at all (non-Linux without recv?!) — refuse
//   SYSCALL   plain recv() into the slot — ONE copy (kernel->slot), zero
//             scratch, universal fallback; the bracket is identical, so the
//             invariant proof covers this mode too
//   RING      io_uring setup + enter functional: the recv is an async SQE,
//             submission/completion rings mmap'd, one io_uring_enter per
//             frame (LIVE in the dev sandbox: kernel 5.10 — setup, enter,
//             and buffer registration all functional; the FIXED rung is
//             version-gated there, not permission-gated)
//   FIXED     + IORING_REGISTER_BUFFERS pinning the slot payloads (registered
//             fixed recv needs kernel >= 5.19: IORING_RECV_FIXED_BUFFER) —
//             page-pinning happens ONCE at attach, not per recv
//   (multishot + SQPOLL steady state — zero syscalls per frame — is the
//    documented >= 5.19 deployment rung; see RFC 0012, labeled PREDICTED)
//
// PROTOCOL DEPTH: one in-flight datagram per next() call (depth-1). The
// frozen publish() stamps slot w_slot with w_seq — only the most recent
// begin()'s pair — so out-of-order completion of multiple in-flight recvs
// cannot be expressed without breaking the frozen API. Depth-1 is the sound
// design; the throughput-oriented multishot design is the RFC's next rung.
//
// LAW 2: next() allocates nothing (SQE/CQEs pre-mapped at attach).
// LAW 1: next() is bounded — one enter + one ioctl gate, no spins.
// LAW 4: every capability refusal is probed, recorded, and reported.

#ifndef WEFT_URING_RX_H
#define WEFT_URING_RX_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "fanout.h"

#if defined(__linux__)
#define WEFT_URING_RX_LINUX 1
#else
#define WEFT_URING_RX_LINUX 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capability ladder
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_URING_MODE_NONE    = 0,  ///< no backend — attach refuses
    WEFT_URING_MODE_SYSCALL = 1,  ///< recv() bracket-filler (universal)
    WEFT_URING_MODE_RING    = 2,  ///< io_uring setup+enter functional
    WEFT_URING_MODE_FIXED   = 3,  ///< + registered buffers usable (k >= 5.19)
} weft_uring_mode_t;

/// Probe the ladder once (cached). Probing performs: io_uring_setup, a NOP
/// submit+enter (verifying the full mmap/SQE/CQE plumbing), and a buffer
/// registration attempt. The result is advisory (AXIOM T): attach re-derives
/// its own mode and every syscall return is checked at use time.
weft_uring_mode_t weft_uring_probe(void);

/// Human-readable mode name ("none"/"syscall"/"ring"/"fixed").
const char* weft_uring_mode_str(weft_uring_mode_t m);

// ---------------------------------------------------------------------------
// Ingestion session
// ---------------------------------------------------------------------------

/// Advisory per-session statistics (AXIOM T). The honesty record for the
/// datagram->frame mapping.
typedef struct {
    uint64_t frames;       ///< datagrams accepted and published
    uint64_t bytes;        ///< payload bytes ingested (post-truncation)
    uint64_t padded;       ///< datagrams shorter than payload_bytes (tail zeroed)
    uint64_t truncated;    ///< datagrams longer than payload_bytes (head kept)
    uint64_t aborted;      ///< recv failures after begin() (seq burned; counted)
    uint64_t enters;       ///< io_uring_enter calls (RING mode)
    uint64_t syscalls;     ///< recv() calls (SYSCALL mode)
    uint64_t gates;        ///< FIONREAD availability gates performed
    int last_errno;        ///< most recent refusal errno (0 = none)
} weft_uring_stats_t;

/// An ingestion session: one fan-out broadcaster + one datagram fd.
/// SINGLE CONSUMER BY CONTRACT (the socket is drained by one next() caller
/// — the single-writer ring contract above it anyway).
typedef struct weft_uring_rx {
    weft_fanout_t* fan;        ///< borrowed broadcaster (writer side)
    int fd;                    ///< borrowed datagram fd (caller owns)
    int fd_flags_saved;        ///< original O_NONBLOCK state, restored at detach
    weft_uring_mode_t mode;    ///< negotiated at attach
    // io_uring plumbing (RING/FIXED modes; NULL otherwise). Direct shared-
    // memory ring pointers — the classic no-libio_uring layout.
    int ring_fd;
    void* sq_map;              ///< SQ ring mmap (head/tail/mask/array)
    void* cq_map;              ///< CQ ring mmap (head/tail/mask/cqes)
    void* sqes_map;            ///< SQE array mmap
    size_t sq_map_len, cq_map_len, sqes_map_len;
    uint32_t ring_entries;
    uint32_t* sq_head;         ///< kernel-owned consumer index (we read)
    uint32_t* sq_tail;         ///< our producer index (we write, release)
    uint32_t* sq_array;        ///< SQE index array
    uint32_t* sq_mask;
    uint32_t* cq_head;         ///< our consumer index (we write, release)
    uint32_t* cq_tail;         ///< kernel-owned producer index (we read)
    uint32_t* cq_mask;
    void* cqes;                ///< struct io_uring_cqe array base
    weft_uring_stats_t stats;
} weft_uring_rx_t;

/// Attach an ingestion session.
///   fd:  a DATAGRAM socket (SOCK_DGRAM / SOCK_SEQPACKET — enforced via
///        getsockopt(SO_TYPE); stream fds are refused: FIONREAD on a stream
///        counts bytes, not frames, and would corrupt the frame mapping)
///   f:   an initialized broadcaster (begin/publish driven by this module)
/// The mode is negotiated DOWN from the probe result by what actually works
/// at attach time. `rx` must be zeroed storage. Returns 0, or -1 with
/// rx->stats.last_errno set (bad fd type, geometry, or ring setup).
int weft_uring_attach(weft_uring_rx_t* rx, weft_fanout_t* f, int fd);

/// Ingest ONE datagram as ONE frame, under the FI1 bracket:
///   1. FIONREAD gate — no bytes queued -> return 0 (no frame, no seq burn)
///   2. cursor = weft_fanout_begin(f)      (slot invalidated, SeqCst fence)
///   3. the kernel copies the datagram into the slot payload:
///        RING mode: async RECV SQE (MSG_TRUNC accounting) + one enter
///        SYSCALL mode: recv() (MSG_TRUNC accounting)
///   4. tail shorter than the slot -> zero-filled (padded++); longer ->
///      head kept (truncated++); recv failure -> aborted++ (the begun seq
///      is burned — counted, never silent; telescoping stays exact because
///      the burned seq is accounted as a drop on the next claim)
///   5. weft_fanout_publish(f)             (stamp + latestSeq, Release)
/// Returns the published frame seq, or 0 when no datagram was ready.
/// Never blocks (the FIONREAD gate guarantees data is queued; the fd itself
/// is put in non-blocking mode at attach and restored at detach).
uint64_t weft_uring_next(weft_uring_rx_t* rx);

/// Detach: munmap the uring rings, close the ring fd, restore the fd's
/// original flags. The broadcaster and fd remain the caller's. Idempotent.
void weft_uring_detach(weft_uring_rx_t* rx);

/// Advisory stats snapshot (AXIOM T).
void weft_uring_stats(const weft_uring_rx_t* rx, weft_uring_stats_t* out);

/// One human-readable mode/capability line for evidence logs.
size_t weft_uring_report(char* buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif // WEFT_URING_RX_H
