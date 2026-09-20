// weft_render_bridge.h — Cross-Platform Render Bridge, C driver layer
//
// WHY EXISTS: nano/doc-007.md §6. Connects a Weft fan-out ring to platform
// rasterizers (Vulkan, Metal, WebGPU, GLES) with zero copy and zero allocations
// on the frame hot path.
//
// LAW 3: Mechanism, not policy. The kernel (weft.h/weft.c) remains strictly
// byte-frozen; the render bridge operates purely at the driver/presentation seam.

#ifndef WEFT_RENDER_BRIDGE_H
#define WEFT_RENDER_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include "fanout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEFT_RENDER_BACKEND_NONE   = 0,
    WEFT_RENDER_BACKEND_VULKAN = 1,
    WEFT_RENDER_BACKEND_METAL  = 2,
    WEFT_RENDER_BACKEND_WEBGPU = 3,
    WEFT_RENDER_BACKEND_GLES   = 4,
    WEFT_RENDER_BACKEND_MOCK   = 5,
} weft_render_backend_t;

typedef struct weft_render_bridge {
    weft_fanout_t*        ring;
    weft_render_backend_t backend;
    void*                 gpu_handle;       ///< Platform GPU buffer / texture / uniform handle
    uint64_t              last_seq_bound;   ///< Sequence number from last successful bind
    uint64_t              bind_count;       ///< Total bind invocations
    uint64_t              present_count;    ///< Total present/draw submissions
    int                   is_direct_mapped; ///< 1 if GPU directly reads host-visible memory
} weft_render_bridge_t;

/// Heap-allocated render bridge constructor.
int weft_render_bridge_create(weft_render_bridge_t** out,
                              weft_fanout_t* ring,
                              weft_render_backend_t backend);

/// Zero-allocation static/stack initialization.
int weft_render_bridge_init(weft_render_bridge_t* bridge,
                            weft_fanout_t* ring,
                            weft_render_backend_t backend);

/// Attach a platform GPU buffer/texture handle (e.g. VkBuffer, MTLBuffer, GPUBuffer).
void weft_render_bridge_attach_gpu_handle(weft_render_bridge_t* b, void* gpu_handle, int direct_mapped);

/// Per-frame: synchronize and make the newest ring payload visible to the GPU.
/// Zero heap allocations on this hot path.
void weft_render_bridge_bind(weft_render_bridge_t* b);

/// Submit/present the bound frame to the rasterizer pipeline.
/// Zero heap allocations on this hot path.
void weft_render_bridge_present(weft_render_bridge_t* b);

/// Destroy and free any bridge resources.
void weft_render_bridge_destroy(weft_render_bridge_t* b);

/// Query active backend.
weft_render_backend_t weft_render_bridge_get_backend(const weft_render_bridge_t* b);

#ifdef __cplusplus
}
#endif

#endif // WEFT_RENDER_BRIDGE_H
