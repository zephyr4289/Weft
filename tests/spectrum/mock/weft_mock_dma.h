// weft_mock_dma.h — the synthetic hardware transport (Pillar 5, D-52,
// mandate C: the headless-CI device model).
//
// WHY EXISTS: every vendor driver's DEVICE-mode code path — packet
// formation, channel sequencing, bounded wait ladder, DEVICE_GONE
// fallback — must be 100% exercisable on hardware-less CI. This mock IS
// the device: a weft_dma_transport_t whose map/enqueue/poll/unmap motions
// emulate real silicon with a DETERMINISTIC virtual-clock model:
//
//   * DMA latency:   deadline = now + fixed_ns + bytes * ns_per_byte
//   * Bus saturation: an in-flight byte window (token bucket): enqueue
//                     refuses EBUSY until completed transfers refund it —
//                     honest backpressure, never silent drops
//   * Register polling: poll() advances the completion doorbell only
//                     once the device clock clears the programmed
//                     deadline; every poll is COUNTED (the wait-ladder
//                     evidence the audit report cites)
//   * Device compute: completions EXECUTE the op through the mapped
//                     (aliased!) pointers using the normative scalar
//                     oracle kernels — so end-to-end DEVICE-path results
//                     are bit-exact by construction, and zero-copy is
//                     PROVEN: outputs appear in the CALLER's buffers with
//                     no intermediate copy anywhere
//   * Fault injection: hot-unplug (EDEVICE), Law-2 violation (a transport
//                     that copies — caught by the driver's structural
//                     check), both for the error-ledger batteries
//
// MOCK = test infrastructure: malloc at create/destroy only; ZERO
// allocation on any map/enqueue/poll/uncall path (the torture battery
// would catch it instantly — the module heap lock does not govern test
// code, but /proc/self/statm and mallinfo2 witnesses do).

#ifndef WEFT_MOCK_DMA_H
#define WEFT_MOCK_DMA_H

#include "../../../core/c/spectrum/drivers/weft_dma.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct weft_mock_dma_cfg {
    const char* name;          ///< honest synthetic device identity
    uint64_t    fixed_ns;      ///< DMA base latency
    uint64_t    ps_per_byte;   ///< bus transfer time per byte (picoseconds:
                               ///<  a 20 GB/s bus = 50; keeps math integer)
    uint64_t    capacity_bytes;///< in-flight window (saturation model)
} weft_mock_dma_cfg_t;

/// Create a synthetic device transport (malloc once; caller destroys).
weft_dma_transport_t* weft_mock_dma_new(const weft_mock_dma_cfg_t* cfg);
void                  weft_mock_dma_destroy(weft_dma_transport_t* t);

// --- witnesses (the honest ledger the audit report reads) -----------------

/// Transports that would have copied (map() moved the data pointer).
/// Stays 0 unless break_zero_copy() was injected — the Law-2 proof.
uint64_t weft_mock_dma_would_copies(const weft_dma_transport_t* t);
/// Total register polls (the wait-ladder evidence).
uint64_t weft_mock_dma_poll_count(const weft_dma_transport_t* t);
/// Bytes currently in flight (the saturation witness).
uint64_t weft_mock_dma_inflight_bytes(const weft_dma_transport_t* t);
/// Total packets enqueued / completed by the synthetic device.
uint64_t weft_mock_dma_enqueued_pkts(const weft_dma_transport_t* t);
uint64_t weft_mock_dma_completed_pkts(const weft_dma_transport_t* t);
/// Total completions executed with results written through aliased
/// pointers (the zero-copy end-to-end witness).
uint64_t weft_mock_dma_zero_copy_completions(const weft_dma_transport_t* t);

// --- fault injection (the error-ledger batteries) ---------------------------

/// Next enqueue/poll returns EDEVICE (hot-unplug; Law 3 degradation path).
void weft_mock_dma_inject_device_gone(weft_dma_transport_t* t);
/// Next map() deliberately COPIES (moves the data pointer) — the driver's
/// Law-2 structural check must refuse it with ESTATE.
void weft_mock_dma_break_zero_copy(weft_dma_transport_t* t);

#ifdef __cplusplus
}
#endif

#endif // WEFT_MOCK_DMA_H
