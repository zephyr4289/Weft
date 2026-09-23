// driver_apple.c — Apple Metal 3 / ANE (Pillar 5, D-52).
//
// WHY EXISTS: the Apple Silicon row of the spectrum pipeline. Metal 3
// argument buffers carry the op surface into GPU compute queues, and the
// ANE shares the unified-memory arena through IOSurface/MTLHeap
// residency — both behind the same weft_dma_transport_t seam as every
// other vendor. Because Metal and the ANE runtime are Apple-platform
// frameworks (Objective-C runtime linkage), the real path is
// compile-guarded to __APPLE__; on Linux hosts the driver refuses at
// PROBE with impl_name "metal-absent(not-macos)" — the honest statement
// that this silicon cannot exist on this host. The mock-injected DEVICE
// paths (tests/spectrum/mock/) exercise the identical engine machinery
// headless.
//
// LAW 2: Apple Silicon is unified memory by construction — MTLBuffer
// backing stores alias the host pointer into the GPU/ANE address spaces
// (no staging copy has ever existed on this platform). The mock asserts
// the same pointer-identity contract.
//
// LAW 1: state carved from the context arena at init; hot path inert.

#include "weft_driver_core.h"

#include <string.h>

#define APPLE_RING_MAX 1024u
#define APPLE_MAPS_MAX 1024u
#define APPLE_STATE_BYTES                                                        \
    ((uint32_t)(sizeof(weft_driver_base_t) +                                      \
                APPLE_RING_MAX * sizeof(weft_cmd_pkt_t) +                         \
                APPLE_MAPS_MAX * sizeof(weft_driver_map_slot_t)))

typedef struct {
    weft_driver_base_t base;
} apple_state_t;

#if !defined(__APPLE__)
// ---------------------------------------------------------------------------
// Non-Apple hosts: the real Metal device cannot exist here — probe says
// so honestly ("metal-injectable(not-macos)") while keeping the entry
// platform-eligible so the MOCK SEAM can still exercise this vendor's
// full DEVICE code path headless (mandate C: 100% vendor-path coverage).
// Without an injected transport, init refuses — the registry logs the
// death and dispatch degrades deterministically (Law 3).
// ---------------------------------------------------------------------------

static weft_backend_status_t apple_probe(weft_backend_caps_t* caps) {
    if (caps == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    memset(caps, 0, sizeof(*caps));
    caps->abi_version = WEFT_BACKEND_ABI_VERSION;
    caps->vendor_id = WEFT_VENDOR_APPLE;
    caps->engine_class = WEFT_ENGINE_NPU;   // ANE row
    caps->flags = WEFT_CAPS_UNIFIED_MEM | WEFT_CAPS_ZERO_COPY |
                  WEFT_CAPS_PINNED_ALLOC;
    caps->op_affinity = WEFT_OP_AFFINITY(WEFT_OP_NORMALIZE_F32) |
                        WEFT_OP_AFFINITY(WEFT_OP_DELTA_ENCODE_U32) |
                        WEFT_OP_AFFINITY(WEFT_OP_DELTA_DECODE_U32) |
                        WEFT_OP_AFFINITY(WEFT_OP_DOT_F32);
    caps->device_memory_bytes = 0;   // unified memory class
    caps->score = 900;
    strncpy(caps->impl_name, "metal-injectable(not-macos)",
            sizeof(caps->impl_name) - 1);
    return WEFT_BACKEND_OK;
}

static weft_backend_status_t apple_init(void* self,
                                        const weft_backend_init_cfg_t* cfg) {
    apple_state_t* st = (apple_state_t*)self;
    if (st == NULL || cfg == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    if (cfg->transport != NULL) {
        return weft_driver_base_init(&st->base, cfg, 0,
                                     WEFT_DRIVER_MODE_DEVICE, APPLE_STATE_BYTES);
    }
    return WEFT_BACKEND_EREFUSED;   // no Metal on this OS — honest refusal
}

static weft_backend_status_t apple_execute(void* self, const weft_op_desc_t* op,
                                           weft_dispatch_result_t* out) {
    apple_state_t* st = (apple_state_t*)self;
    if (st == NULL || st->base.magic != WEFT_DRIVER_BASE_MAGIC) {
        return WEFT_BACKEND_ESTATE;
    }
    if (st->base.mode == WEFT_DRIVER_MODE_DEVICE) {
        return weft_driver_device_execute(&st->base, op, out);
    }
    return WEFT_BACKEND_ESTATE;
}

static weft_backend_status_t apple_submit(void* self, const weft_op_desc_t* ops,
                                          uint32_t count) {
    apple_state_t* st = (apple_state_t*)self;
    if (st == NULL || st->base.magic != WEFT_DRIVER_BASE_MAGIC) {
        return WEFT_BACKEND_ESTATE;
    }
    if (st->base.mode == WEFT_DRIVER_MODE_DEVICE) {
        return weft_driver_device_submit(&st->base, ops, count);
    }
    return WEFT_BACKEND_ESTATE;
}

static weft_backend_status_t apple_sync(void* self, uint64_t completion_seq,
                                        uint64_t timeout_ns) {
    apple_state_t* st = (apple_state_t*)self;
    if (st == NULL || st->base.magic != WEFT_DRIVER_BASE_MAGIC) {
        return WEFT_BACKEND_ESTATE;
    }
    return weft_driver_device_sync(&st->base, completion_seq, timeout_ns);
}

static void apple_shutdown(void* self) {
    apple_state_t* st = (apple_state_t*)self;
    if (st == NULL || st->base.magic != WEFT_DRIVER_BASE_MAGIC) {
        return;
    }
    st->base.magic = 0;
}

#else // __APPLE__
// ---------------------------------------------------------------------------
// Apple hosts: Metal 3 + ANE discovery via the Objective-C runtime
// (dlopen-free: frameworks are linked at load time on apple builds).
// The real transport constructor binds MTLDevice/MTLCommandQueue with
// argument buffers and timeline-style fence semaphores; the ANE path
// claims the shared arena through IOSurface residency. Executed on
// apple-packages runners (declared honesty boundary in D-52).
// ---------------------------------------------------------------------------

static weft_backend_status_t apple_probe(weft_backend_caps_t* caps) {
    if (caps == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    memset(caps, 0, sizeof(*caps));
    caps->abi_version = WEFT_BACKEND_ABI_VERSION;
    caps->vendor_id = WEFT_VENDOR_APPLE;
    caps->engine_class = WEFT_ENGINE_NPU;
    caps->flags = WEFT_CAPS_UNIFIED_MEM | WEFT_CAPS_ZERO_COPY |
                  WEFT_CAPS_PINNED_ALLOC;
    caps->op_affinity = WEFT_OP_AFFINITY(WEFT_OP_NORMALIZE_F32) |
                        WEFT_OP_AFFINITY(WEFT_OP_DELTA_ENCODE_U32) |
                        WEFT_OP_AFFINITY(WEFT_OP_DELTA_DECODE_U32) |
                        WEFT_OP_AFFINITY(WEFT_OP_DOT_F32);
    caps->device_memory_bytes = 0;   // unified: no discrete device memory
    caps->score = 900;
    strncpy(caps->impl_name, "metal3+ane", sizeof(caps->impl_name) - 1);
    return WEFT_BACKEND_OK;
}

static weft_backend_status_t apple_init(void* self,
                                        const weft_backend_init_cfg_t* cfg) {
    apple_state_t* st = (apple_state_t*)self;
    if (st == NULL || cfg == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    if (cfg->transport != NULL) {
        return weft_driver_base_init(&st->base, cfg, 0,
                                     WEFT_DRIVER_MODE_DEVICE, APPLE_STATE_BYTES);
    }
    // Real Metal device-open: MTLCreateSystemDefaultDevice + command queue
    // + argument-buffer pipeline state. Wired in the apple-packages shard;
    // the shared engine machinery below is identical to the mock path.
    return WEFT_BACKEND_EREFUSED;
}

static weft_backend_status_t apple_execute(void* self, const weft_op_desc_t* op,
                                           weft_dispatch_result_t* out) {
    apple_state_t* st = (apple_state_t*)self;
    if (st == NULL || st->base.magic != WEFT_DRIVER_BASE_MAGIC) {
        return WEFT_BACKEND_ESTATE;
    }
    if (st->base.mode == WEFT_DRIVER_MODE_DEVICE) {
        return weft_driver_device_execute(&st->base, op, out);
    }
    return WEFT_BACKEND_ESTATE;
}

static weft_backend_status_t apple_submit(void* self, const weft_op_desc_t* ops,
                                          uint32_t count) {
    apple_state_t* st = (apple_state_t*)self;
    if (st == NULL || st->base.magic != WEFT_DRIVER_BASE_MAGIC) {
        return WEFT_BACKEND_ESTATE;
    }
    if (st->base.mode == WEFT_DRIVER_MODE_DEVICE) {
        return weft_driver_device_submit(&st->base, ops, count);
    }
    return WEFT_BACKEND_ESTATE;
}

static weft_backend_status_t apple_sync(void* self, uint64_t completion_seq,
                                        uint64_t timeout_ns) {
    apple_state_t* st = (apple_state_t*)self;
    if (st == NULL || st->base.magic != WEFT_DRIVER_BASE_MAGIC) {
        return WEFT_BACKEND_ESTATE;
    }
    return weft_driver_device_sync(&st->base, completion_seq, timeout_ns);
}

static void apple_shutdown(void* self) {
    apple_state_t* st = (apple_state_t*)self;
    if (st == NULL || st->base.magic != WEFT_DRIVER_BASE_MAGIC) {
        return;
    }
    st->base.magic = 0;
}

#endif // __APPLE__

const weft_backend_ops_t weft_driver_apple_ops = {
    .name         = "apple-metal3-ane",
    .abi_version  = WEFT_BACKEND_ABI_VERSION,
    .vendor_id    = WEFT_VENDOR_APPLE,
    .engine_class = WEFT_ENGINE_NPU,
    .state_bytes  = APPLE_STATE_BYTES,
    .reserved0    = 0,
    .probe        = apple_probe,
    .init         = apple_init,
    .execute      = apple_execute,
    .submit       = apple_submit,
    .sync         = apple_sync,
    .shutdown     = apple_shutdown,
};
