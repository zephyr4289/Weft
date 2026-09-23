// weft_cluster_fabric.c — the refusal-cascade selector (see the header).

#include "weft_cluster_fabric.h"

#include <stdio.h>
#include <string.h>

#include "backends/rdma/weft_rdma_driver.h"
#include "backends/uring/weft_uring_driver.h"
#include "backends/xdp/weft_xdp_driver.h"
#include "backends/loopback/weft_loopback.h"

const char* weft_fabric_kind_str(weft_fabric_kind_t k) {
    switch (k) {
        case WEFT_FABRIC_RDMA:     return "rdma";
        case WEFT_FABRIC_XDP:      return "xdp";
        case WEFT_FABRIC_URING:    return "uring";
        case WEFT_FABRIC_LOOPBACK: return "loopback";
        default:                   return "?";
    }
}

void weft_fabric_probe_all(weft_fabric_probe_t out[WEFT_FABRIC_KIND_COUNT]) {
    memset(out, 0, sizeof(*out) * WEFT_FABRIC_KIND_COUNT);

    out[WEFT_FABRIC_RDMA].kind = WEFT_FABRIC_RDMA;
    out[WEFT_FABRIC_RDMA].status = weft_rdma_probe(
        out[WEFT_FABRIC_RDMA].reason, sizeof(out[WEFT_FABRIC_RDMA].reason));
    out[WEFT_FABRIC_RDMA].available =
        (out[WEFT_FABRIC_RDMA].status == WEFT_CLUSTER_OK);

    out[WEFT_FABRIC_XDP].kind = WEFT_FABRIC_XDP;
    {
        char d[192];
        const weft_xdp_cap_t cap = weft_xdp_probe(d, sizeof(d));
        out[WEFT_FABRIC_XDP].available = (cap >= WEFT_XDP_CAP_SETUP);
        out[WEFT_FABRIC_XDP].status =
            out[WEFT_FABRIC_XDP].available ? WEFT_CLUSTER_OK
                                           : WEFT_CLUSTER_E_PERMS;
        snprintf(out[WEFT_FABRIC_XDP].reason,
                 sizeof(out[WEFT_FABRIC_XDP].reason), "%.158s", d);
    }

    out[WEFT_FABRIC_URING].kind = WEFT_FABRIC_URING;
    {
        char d[192];
        const weft_uring_cap_t cap = weft_uring_probe(d, sizeof(d));
        out[WEFT_FABRIC_URING].available = (cap >= WEFT_URING_CAP_RING);
        out[WEFT_FABRIC_URING].status =
            out[WEFT_FABRIC_URING].available ? WEFT_CLUSTER_OK
                                             : WEFT_CLUSTER_E_SYS;
        snprintf(out[WEFT_FABRIC_URING].reason,
                 sizeof(out[WEFT_FABRIC_URING].reason), "%.158s", d);
    }

    out[WEFT_FABRIC_LOOPBACK].kind = WEFT_FABRIC_LOOPBACK;
    out[WEFT_FABRIC_LOOPBACK].available = 1;   // the guaranteed floor
    out[WEFT_FABRIC_LOOPBACK].status = WEFT_CLUSTER_OK;
    snprintf(out[WEFT_FABRIC_LOOPBACK].reason,
             sizeof(out[WEFT_FABRIC_LOOPBACK].reason),
             "loopback: always available (in-process, [FALLBACK-COPY])");
}

size_t weft_fabric_chain(char* buf, size_t buflen) {
    weft_fabric_probe_t p[WEFT_FABRIC_KIND_COUNT];
    weft_fabric_probe_all(p);
    size_t off = 0;
    for (int i = 0; i < WEFT_FABRIC_KIND_COUNT; i++) {
        if (off + 1 >= buflen) break;
        const int n = snprintf(buf + off, buflen - off,
                               "%s%s: %s — %s (%s)\n", off ? "  " : "",
                               weft_fabric_kind_str((weft_fabric_kind_t)i),
                               p[i].available ? "OK" : "REFUSED",
                               p[i].reason,
                               weft_cluster_status_str(p[i].status));
        if (n < 0) break;
        off += (size_t)n;
    }
    return off;
}

weft_fabric_kind_t weft_fabric_best(void) {
    weft_fabric_probe_t p[WEFT_FABRIC_KIND_COUNT];
    weft_fabric_probe_all(p);
    for (int i = 0; i < WEFT_FABRIC_KIND_COUNT; i++) {
        if (p[i].available) return (weft_fabric_kind_t)i;
    }
    return WEFT_FABRIC_LOOPBACK;
}
