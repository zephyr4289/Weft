// weft_render_bridge.c — Cross-Platform Render Bridge, C driver layer
//
// LAW 3: Mechanism, not policy. Zero kernel modification.

#include "weft_render_bridge.h"
#include <stdlib.h>
#include <string.h>

int weft_render_bridge_init(weft_render_bridge_t* b,
                            weft_fanout_t* ring,
                            weft_render_backend_t backend) {
    if (!b || !ring) return -1;

    memset(b, 0, sizeof(*b));
    b->ring = ring;
    b->backend = backend;
    b->gpu_handle = NULL;
    b->last_seq_bound = 0;
    b->bind_count = 0;
    b->present_count = 0;

    // Metal / unified memory systems default to direct mapped
    if (backend == WEFT_RENDER_BACKEND_METAL || backend == WEFT_RENDER_BACKEND_MOCK) {
        b->is_direct_mapped = 1;
    } else {
        b->is_direct_mapped = 0;
    }

    return 0;
}

int weft_render_bridge_create(weft_render_bridge_t** out,
                              weft_fanout_t* ring,
                              weft_render_backend_t backend) {
    if (!out || !ring) return -1;

    weft_render_bridge_t* b = (weft_render_bridge_t*)calloc(1, sizeof(weft_render_bridge_t));
    if (!b) return -1;

    int rc = weft_render_bridge_init(b, ring, backend);
    if (rc != 0) {
        free(b);
        return rc;
    }

    *out = b;
    return 0;
}

void weft_render_bridge_attach_gpu_handle(weft_render_bridge_t* b, void* gpu_handle, int direct_mapped) {
    if (!b) return;
    b->gpu_handle = gpu_handle;
    b->is_direct_mapped = direct_mapped;
}

void weft_render_bridge_bind(weft_render_bridge_t* b) {
    if (!b || !b->ring) return;

    b->bind_count++;

    // Probe latest sequence from the ring header (ctrl[0] is latestSeq)
    if (b->ring->ctrl) {
        b->last_seq_bound = atomic_load_explicit(&b->ring->ctrl[0], memory_order_acquire);
    }
}

void weft_render_bridge_present(weft_render_bridge_t* b) {
    if (!b) return;
    b->present_count++;
}

void weft_render_bridge_destroy(weft_render_bridge_t* b) {
    if (!b) return;
    // Release bridge container
    free(b);
}

weft_render_backend_t weft_render_bridge_get_backend(const weft_render_bridge_t* b) {
    if (!b) return WEFT_RENDER_BACKEND_NONE;
    return b->backend;
}
