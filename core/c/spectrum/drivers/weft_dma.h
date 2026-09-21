// weft_dma.h — the injectable device command-path seam (Pillar 5, D-52).
//
// WHY EXISTS: every vendor engine in this tree — Qualcomm FastRPC/Hexagon,
// MediaTek Neuropilot/APU, Apple Metal 3, NVIDIA CUDA, the Vulkan 1.3
// timeline queues — moves work through the SAME four motions:
//
//     map(buffer)      attach a device handle to host memory (NEVER copy:
//                      on unified-memory silicon the device address must
//                      alias the host pointer — Law 2)
//     enqueue(pkts)    push pre-formed command packets into the device ring
//     poll(&done_seq)  register-poll the completion doorbell
//     unmap(buffer)    detach the handle
//
// Formalizing those four motions as weft_dma_transport_t is what makes the
// vendor code paths 100% testable on headless CI (mandate C): the REAL
// driver state machines (packet formation, channel sequencing, completion
// handling, DEVICE_GONE fallback) run unchanged against a synthetic
// transport that models DMA latency, bus bandwidth saturation and register
// polling — while production runs construct the real transport (FastRPC
// ioctls, Neuropilot descriptors, Metal command buffers, CUDA streams)
// inside the driver's device-open path.
//
// The transport is injected through weft_backend_cfg_t::transport_overrides
// (tests) or constructed by the driver (production). Either way the driver
// above it is byte-identical — that is the point of the seam.
//
// LAW 2 NOTE: map() is the ONLY place a device handle may appear, and it
// must set WEFT_BUF_UNIFIED + alias data when the engine is unified-memory
// class. A transport that copies inside map() violates the zero-copy law
// and will be caught: the mock harness counts would-be copies and the
// STRICT_ZERO_COPY ctx flag asserts host<->device pointer identity through
// the entire pipeline.
//
// The command packet is the DEVICE-VISIBLE projection of a weft_op_desc_t:
// dma tags (not pointers), dims, scalars and the deadline the device clock
// must clear before the completion register advances.

#ifndef WEFT_DMA_H
#define WEFT_DMA_H

#include "weft_backend.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===========================================================================
// §1 Command packet — 64B, one cache line, device-visible (frozen)
// ===========================================================================

/// One device command. Tags reference the transport's map() handles; the
/// device never sees host pointers except through aliased unified handles.
/// Exactly 64 bytes — one cache line (Pillar-1 mandate). The DMA latency
/// model's submit/deadline timestamps live in TRANSPORT-side state (the
/// device clock is the transport's; the packet stays pure command).
typedef struct {
    uint32_t op_kind;       ///< weft_op_kind_t
    uint32_t flags;         ///< WEFT_OP_FLAG_*
    uint32_t in_tag;        ///< bufs[0] dma tag (0 = none)
    uint32_t aux_tag;       ///< bufs[1] dma tag (DOT_F32 B operand)
    uint32_t out_tag;       ///< bufs[2] dma tag (0 = inplace / pure-result)
    uint32_t m, k, n;       ///< dims (see weft_backend.h §5)
    uint32_t lda, ldb, ldc;
    float    f0, f1;        ///< normalize: min, rs
    uint32_t u0, u1;        ///< delta: seed / checksum: seqlock stamp
    uint32_t reserved0;     ///< pads to exactly one 64B cache line
} weft_cmd_pkt_t;

_Static_assert(sizeof(weft_cmd_pkt_t) == 64,
               "weft_cmd_pkt_t must be exactly one 64B cache line");

// ===========================================================================
// §2 The transport vtable (frozen; pure C ABI like the backend vtable)
// ===========================================================================

struct weft_dma_transport_s {
    const char* name;               ///< honest identity ("mock-dma", "fastrpc-adsp")
    void*       transport_ctx;      ///< transport-private state

    /// Attach a device handle to buf. MUST NOT copy. On unified-memory
    /// engines: buf->dma_tag set, WEFT_BUF_DEVICE|UNIFIED flagged, data
    /// pointer unchanged (aliasing). Bounded map tables: exhaustion is
    /// EBUSY (fail-closed, never eviction).
    weft_backend_status_t (*map)(void* tctx, weft_buffer_desc_t* buf);

    /// Push n pre-formed packets into the device ring. Applies the latency
    /// model: deadline_ns = now + fixed + bytes*ns_per_byte. Bus saturation
    /// (token bucket exhausted) refuses with EBUSY — honest backpressure.
    weft_backend_status_t (*enqueue)(void* tctx, weft_cmd_pkt_t* pkts,
                                     uint32_t count);

    /// Register-poll the completion doorbell. Emulates device completion
    /// registers: the done sequence advances once the device clock passes
    /// the programmed deadline; poll counts are instrumented. Bounded
    /// spin ladder — never an unbounded burn loop.
    weft_backend_status_t (*poll)(void* tctx, uint32_t* done_seq_out);

    /// Detach a handle (idempotent; unmapping an unmapped tag is EINVAL).
    weft_backend_status_t (*unmap)(void* tctx, const weft_buffer_desc_t* buf);

    /// The device's virtual clock (monotonic ns). Mocks derive it from
    /// CLOCK_MONOTONIC + the programmed offset; real transports read their
    /// device timestamps.
    uint64_t (*now_ns)(void* tctx);
};

typedef struct weft_dma_transport_s weft_dma_transport_t;

#ifdef __cplusplus
}
#endif

#endif // WEFT_DMA_H
