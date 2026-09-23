// weft_cluster_fabric.h — RFC-0019 §6: the transport selector with the
// HONEST REFUSAL CASCADE (tools layer).
//
// WHY EXISTS: a cluster node boots into an unknown world — an HCA may
// or may not sit behind the PCIe bus, CAP_NET_ADMIN may or may not be
// held, the kernel may or may not allow io_uring. The mandate's Law 4
// requires the fabric to ROUTE, not crash: probe every engine in
// preference order, keep the full refusal CHAIN (each engine's named
// reason), and open the best available road. The chain is the artifact
// ops teams read when a node lands on loopback unexpectedly — the
// fabric never hides WHY.
//
// PREFERENCE ORDER (the mandate's architecture):
//   rdma (one-sided, 0 receiver wakeups) -> xdp (line-rate ingress)
//   -> uring (batched syscall bypass) -> loopback (in-process truth)
//
// LAW 2: probing is side-effect-free (no QPs created, no XSK bound;
//   device handles opened and closed inside the probe calls).
// LAW 4: the chain string contains every engine's verdict + reason +
//   status code — the selector's output IS the honesty record.

#ifndef WEFT_CLUSTER_FABRIC_H
#define WEFT_CLUSTER_FABRIC_H

#include <stddef.h>
#include <stdint.h>

#include "weft_cluster_core.h"
#include "weft_wcr1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEFT_FABRIC_RDMA     = 0,
    WEFT_FABRIC_XDP      = 1,
    WEFT_FABRIC_URING    = 2,
    WEFT_FABRIC_LOOPBACK = 3,
    WEFT_FABRIC_KIND_COUNT = 4,
} weft_fabric_kind_t;

const char* weft_fabric_kind_str(weft_fabric_kind_t k);

/// One engine's probe verdict.
typedef struct {
    weft_fabric_kind_t kind;
    int                available;      ///< 1 = probe succeeded
    weft_cluster_status_t status;      ///< refusal code when !available
    char               reason[160];    ///< the named honesty line
} weft_fabric_probe_t;

/// Probe all engines in preference order (no side effects held).
void weft_fabric_probe_all(weft_fabric_probe_t out[WEFT_FABRIC_KIND_COUNT]);

/// Render the refusal chain (one line per engine, preference order).
size_t weft_fabric_chain(char* buf, size_t buflen);

/// The best available kind (or LOOPBACK — the guaranteed floor).
weft_fabric_kind_t weft_fabric_best(void);

#ifdef __cplusplus
}
#endif

#endif // WEFT_CLUSTER_FABRIC_H
