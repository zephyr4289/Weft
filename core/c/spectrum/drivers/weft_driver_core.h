// weft_driver_core.h — shared engine machinery for the vendor drivers (P5).
//
// WHY EXISTS: all five vendor drivers (Qualcomm FastRPC/Hexagon, MediaTek
// Neuropilot/APU, Apple Metal 3/ANE, NVIDIA CUDA/TensorRT + PC vector,
// ARM/RISC-V CPU vector) move commands through the same generic engine
// discipline once their transport is attached:
//
//     validate op (fail-closed dims/dtype/bytes law)
//       -> map in/aux/out buffers on the transport (ZERO-COPY: the host
//          pointer must survive map() untouched; unified engines alias)
//       -> form the 64B weft_cmd_pkt_t projection (dma tags, dims, scalars)
//       -> enqueue on the transport (the LATENCY MODEL assigns deadlines;
//          bus saturation refuses with EBUSY — honest backpressure)
//       -> poll the completion register through a bounded wait ladder
//          (64 pauses -> sched_yield -> 50us nanosleep; no unbounded spin)
//       -> unmap and fill the dispatch result
//
// Factoring that path HERE (instead of five near-copies) is what lets each
// driver file contain ONLY what is genuinely vendor-specific: the real
// library discovery (dlopen symbol tables with the actual FastRPC /
// NeuronRuntime / Metal / CUDA entry points), engine limits, capability
// claims and the real transport constructor. The generic ring logic is
// identically exercised for every vendor by the mock harness.
//
// HOST_VECTOR mode: the "PC workstation" (x86) and "ARM/RISC-V" rows of
// the pipeline execute the same op surface directly on the SIMD core when
// no discrete engine is present — same validation, same result contract,
// device_ns honestly 0 (CPU time shows up in enqueue/complete stamps).
//
// LAWS: nothing in this file allocates after init; every path is
// fail-closed; the wait ladder is bounded (courtesy of the tensor
// module's Law-2 discipline).

#ifndef WEFT_DRIVER_CORE_H
#define WEFT_DRIVER_CORE_H

#include "weft_backend.h"
#include "weft_dma.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===========================================================================
// §1 Driver modes
// ===========================================================================

#define WEFT_DRIVER_MODE_DEAD         0u  ///< init failed / device vanished
#define WEFT_DRIVER_MODE_DEVICE       1u  ///< speak through a transport
#define WEFT_DRIVER_MODE_HOST_VECTOR  2u  ///< execute on the SIMD core

// ===========================================================================
// §2 Driver state header (every driver's state block starts with this)
// ===========================================================================

typedef struct weft_driver_map_slot {
    weft_buffer_desc_t desc;   ///< mapped descriptor (tag == slot + 1)
    uint32_t tag;              ///< 0 = free slot
    uint32_t reserved;
} weft_driver_map_slot_t;

typedef struct weft_driver_base {
    uint32_t magic;                     ///< WEFT_DRIVER_BASE_MAGIC
    uint32_t mode;                      ///< WEFT_DRIVER_MODE_*
    uint32_t backend_idx;               ///< index in the owning ctx table
    uint32_t cmd_slots;                 ///< command ring depth
    uint32_t map_slots;                 ///< DMA handle table depth

    const weft_dma_transport_t* transport;   ///< NULL in HOST_VECTOR mode
    void*   owner;                            ///< owning ctx (logging)
    void  (*log)(void* owner, uint32_t code, uint32_t backend_idx, uint32_t aux);

    uint32_t submitted_seq;             ///< last enqueued command sequence
    uint32_t completed_seq;             ///< last observed completion
    uint32_t map_count;                 ///< live mappings
    uint32_t reserved0;

    // Carved AFTER this header by weft_driver_base_init (arena, one block):
    //   weft_cmd_pkt_t  cmd_ring[cmd_slots];
    //   weft_driver_map_slot_t map_table[map_slots];
} weft_driver_base_t;

#define WEFT_DRIVER_BASE_MAGIC 0x57445243u  ///< 'WDRC'

// ===========================================================================
// §3 Shared engine paths (used by every driver's ops vtable)
// ===========================================================================

/// Initialize the base header + carve the command ring and map table from
/// ONE arena block of *state_bytes_total*. mode is decided by the driver's
/// init (DEVICE requires cfg->transport != NULL). backend_idx comes from
/// the registry so driver log records attribute correctly.
weft_backend_status_t weft_driver_base_init(weft_driver_base_t* base,
                                            const weft_backend_init_cfg_t* cfg,
                                            uint32_t backend_idx,
                                            uint32_t mode,
                                            uint64_t state_bytes_total);

/// Fail-closed op validation (dims/dtype/bytes/stride law) shared by every
/// execution path — device AND host-vector. Returns EINVAL/ERANGE with no
/// side effects.
weft_backend_status_t weft_driver_validate_op(const weft_op_desc_t* op);

/// The generic DEVICE execution path: validate -> map -> form packet ->
/// enqueue -> poll (bounded ladder) -> unmap -> fill result. Zero heap,
/// zero copy. Transient transport errors propagate (EBUSY/ETIMEOUT);
/// EDEVICE from the transport marks the driver DEAD and propagates for a
/// fallback hop (Law 3).
weft_backend_status_t weft_driver_device_execute(weft_driver_base_t* base,
                                                 const weft_op_desc_t* op,
                                                 weft_dispatch_result_t* out);

/// The HOST-VECTOR execution path: validate -> weft_simd_* -> fill result.
/// Used by the terminal CPU engine, the PC-workstation host mode and the
/// ARM/RISC-V vector driver.
weft_backend_status_t weft_driver_host_execute(const weft_op_desc_t* op,
                                               weft_dispatch_result_t* out,
                                               uint32_t* seq_io);

/// Device batch submit: validate + map + form + enqueue each op (no poll).
weft_backend_status_t weft_driver_device_submit(weft_driver_base_t* base,
                                                const weft_op_desc_t* ops,
                                                uint32_t count);

/// Bounded wait for completed_seq >= want (the wait ladder).
weft_backend_status_t weft_driver_device_sync(weft_driver_base_t* base,
                                              uint64_t want_seq,
                                              uint64_t timeout_ns);

/// Bounded poll helper: advances base->completed_seq from the transport's
/// doorbell register; returns the latest sequence.
weft_backend_status_t weft_driver_poll_once(weft_driver_base_t* base,
                                            uint32_t* done_seq_out);

/// Monotonic ns (delegates to weft_backend_now_ns; exposed for drivers).
uint64_t weft_driver_now_ns(void);

#ifdef __cplusplus
}
#endif

#endif // WEFT_DRIVER_CORE_H
