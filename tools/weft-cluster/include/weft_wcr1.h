// weft_wcr1.h — RFC-0019 §2: the WCR1 cluster-region contract seam
// (tools layer, C driver module).
//
// WHY EXISTS: Pillar 3 splits the cluster fabric in two. Engineer 1 owns
// the WCR1 memory layout itself — the lock-free cross-server ring memory
// a node PUBLISHES into (the shm_ring/WFSH discipline carried across the
// wire). This pillar owns the TRANSPORT ENGINES that move ring bytes
// between nodes: RoCEv2/InfiniBand one-sided RDMA, eBPF/XDP line-rate
// ingestion, io_uring batched UDP, and the in-process loopback fallback.
// Every engine registers THE SAME MEMORY with ITS kernel/hardware
// interface (ibv_reg_mr / XDP_UMEM_REG / IORING_REGISTER_BUFFERS), so
// they need ONE frozen description of "the region I may touch and where
// chunk k lives in it" — written once, verified by static asserts, never
// re-derived per backend (the weft_tensor_view discipline: the seam is
// the contract, the adapters are its consumers).
//
// THE CONTRACT (40 bytes, little-endian, ABI v1):
//   base            page-aligned (4096) virtual base of the region. For a
//                   WFSH session this is the MAPPING start (the session
//                   header is inside the region — the transport registers
//                   the whole span, exactly the xdp_rx UMEM precedent).
//   span            total registered bytes (>= chunk0_offset + count*size)
//   chunk0_offset   byte offset of chunk 0 from base. A plain anon region
//                   uses 0; a WFSH-backed region uses
//                   64 + 16 + 8*slot_count (header + ctrl + stamps).
//   chunk_size      one transport frame slot: WCF1 header (64) + payload,
//                   multiple of 64 (Law 2 alignment; matches the
//                   gpu_ready 64-byte discipline of the tensor view).
//   chunk_count     number of addressable chunks (>= 2).
//   node_id         cluster node identity carried into WCF1 frames.
//
// ADDRESS LAW: chunk k lives at base + chunk0_offset + k*chunk_size, for
// k in [0, chunk_count). Nothing else about the region's interior is
// assumed: the transport NEVER interprets WCR1 control words (stamps are
// Engineer 1's authority — AXIOM T); it only moves WCF1-framed bytes in
// and out of chunk boundaries.
//
// REGISTRATION LIFETIME (the RFC-0019 memory contract, one paragraph):
//   registration is idempotent-per-engine and EXCLUSIVE of unmap — a
//   region handed to an engine must outlive the engine (destroy the
//   engine first); a region may be registered by several engines at once
//   (RDMA + XDP + io_uring over the same span is legal — the kernel pins
//   pages once per interface, not once per engine). Anon regions created
//   here are MAP_PRIVATE; engines that pin (all three do) require the
//   pages to stay faulted-in — create_anon prefaults and best-effort
//   mlocks (RLIMIT_MEMLOCK honesty: pin failure is REPORTED in
//   pinned_hint, never silent, never fatal — the engines each re-derive
//   their own registration outcome).
//
// LAW 1: create/destroy MAY allocate (setup); the transport data path
//        touches only pre-registered memory — zero allocations.
// LAW 2: chunk geometry is frozen by static asserts; a region that fails
//        weft_wcr1_validate is refused with a NAMED rung, never guessed.
// LAW 3: tools layer — weft.c/weft.h and all of core/c stay untouched.
// LAW 4: the refusal ladder below is the whole honesty surface; every
//        rung is exercised by the CL-series gates.
//
// Honesty boundary: WCR1-the-layout is Engineer 1's parallel deliverable;
// this header freezes the REGION VIEW both sides already agreed on
// (chunked, page-aligned, 64-byte geometry). The WFSH bridge
// (weft_wcr1_from_shm) is EXECUTABLE-VERIFIED here against the in-tree
// shm_ring; a future WCR1 native header that satisfies this view drops
// in without touching the engines.

#ifndef WEFT_WCR1_H
#define WEFT_WCR1_H

#include <stddef.h>
#include <stdint.h>

struct weft_shm_map;  // core/c shm_ring.h — the WFSH bridge (borrowed)

#ifdef __cplusplus
extern "C" {
#endif

/// ABI version of this contract (bump on struct layout change — engines
/// refuse mismatches rather than guess, the WFSH attach discipline).
#define WEFT_WCR1_ABI_VERSION 1u

/// Minimum chunk granularity (WCF1 header + aligned payload; Law 2).
#define WEFT_WCR1_CHUNK_ALIGN 64u

/// Page alignment required of `base` (UMEM/MR/fixed-buffer floors).
#define WEFT_WCR1_PAGE_ALIGN 4096u

/// Region flags (advisory — engines read them for evidence labels only).
#define WEFT_WCR1_F_NONE      0u
#define WEFT_WCR1_F_SHM_BACKED 1u   ///< from a WFSH/memfd mapping
#define WEFT_WCR1_F_HUGETLB    2u   ///< madvise/hugetlbfs backed
#define WEFT_WCR1_F_PINNED     4u   ///< mlock succeeded at create/attach

/// The frozen region view (40 bytes — static asserts in weft_wcr1.c).
typedef struct weft_wcr1_region {
    uint8_t* base;          ///< page-aligned region base (mapping start)
    uint64_t span;          ///< total bytes registered by engines
    uint64_t chunk0_offset; ///< offset of chunk 0 from base
    uint32_t chunk_size;    ///< bytes per chunk (multiple of 64)
    uint32_t chunk_count;   ///< addressable chunks (>= 2)
    uint16_t node_id;       ///< cluster node identity (WCF1 src_node)
    uint16_t reserved0;     ///< zero on create; engines must not read
    uint32_t flags;         ///< WEFT_WCR1_F_*
} weft_wcr1_region_t;

/// Named refusal rungs for weft_wcr1_validate (the ladder, Law 4).
typedef enum {
    WEFT_WCR1_OK = 0,
    WEFT_WCR1_REFUSE_NULL,         ///< NULL region/out pointer
    WEFT_WCR1_REFUSE_BASE_ALIGN,   ///< base not 4096-aligned
    WEFT_WCR1_REFUSE_SPAN_SMALL,   ///< span < chunk0_offset + 2 chunks
    WEFT_WCR1_REFUSE_CHUNK_ALIGN,  ///< chunk_size not multiple of 64
    WEFT_WCR1_REFUSE_CHUNK_COUNT,  ///< chunk_count < 2
    WEFT_WCR1_REFUSE_SPAN_GEOMETRY,///< span != chunk0 + count*size (tight)
    WEFT_WCR1_REFUSE_CHUNK0_ALIGN, ///< chunk0_offset not multiple of 64
} weft_wcr1_refusal_t;

/// Validate a region against the contract. `why` (may be NULL) receives a
/// one-line named refusal for evidence logs. Returns WEFT_WCR1_OK or the
/// first failed rung — never guesses, never clamps.
weft_wcr1_refusal_t weft_wcr1_validate(const weft_wcr1_region_t* r,
                                       char* why, size_t whylen);

/// Chunk k address: base + chunk0_offset + k*chunk_size. Caller validates
/// first; k is trusted after a passing validate (the engines do).
static inline uint8_t* weft_wcr1_chunk(const weft_wcr1_region_t* r,
                                       uint32_t k) {
    return r->base + r->chunk0_offset + (uint64_t)k * r->chunk_size;
}

/// The offset of chunk k from base (for wire fields / rkey exchange).
static inline uint64_t weft_wcr1_chunk_offset(const weft_wcr1_region_t* r,
                                              uint32_t k) {
    return r->chunk0_offset + (uint64_t)k * r->chunk_size;
}

// ---------------------------------------------------------------------------
// Region creation (setup-path only — Law 1 exempts this from zero-alloc)
// ---------------------------------------------------------------------------

/// Create an anonymous page-aligned region of chunk_count*chunk_size
/// bytes (chunk0_offset = 0). Pages are prefaulted; mlock is attempted
/// and the outcome recorded in flags (WEFT_WCR1_F_PINNED) — a pin
/// refusal (RLIMIT_MEMLOCK) is recorded, not laundered. Returns 0/-1.
int weft_wcr1_create_anon(uint32_t chunk_count, uint32_t chunk_size,
                          uint16_t node_id, weft_wcr1_region_t* out);

/// Destroy a region created by weft_wcr1_create_anon (munmap + zero).
/// Regions bridged from shm sessions are NOT owned here — see
/// weft_wcr1_from_shm. Idempotent.
void weft_wcr1_destroy(weft_wcr1_region_t* r);

/// Bridge a WFSH shm session (shm_ring.h) into a region view: chunk k IS
/// fanout slot k's payload area (chunk_size = payload_bytes, chunk_count
/// = slot_count, chunk0_offset = 64 + 16 + 8*slot_count — the RFC-0004
/// layout frozen in fanout.h). Refuses (returns the rung) when the
/// session's payload_bytes is not a multiple of 64 — the cluster seam
/// keeps the 64-byte law even where the fanout core does not require it.
/// The map is BORROWED: caller keeps it alive (destroy the engines
/// before weft_shm_destroy).
weft_wcr1_refusal_t weft_wcr1_from_shm(const struct weft_shm_map* m,
                                       uint16_t node_id,
                                       weft_wcr1_region_t* out,
                                       char* why, size_t whylen);

/// One-line capability/geometry line for evidence logs.
size_t weft_wcr1_report(const weft_wcr1_region_t* r, char* buf,
                        size_t buflen);

#ifdef __cplusplus
}
#endif

#endif // WEFT_WCR1_H
