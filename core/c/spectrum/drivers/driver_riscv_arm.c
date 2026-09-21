// driver_riscv_arm.c — ARM SVE2/NEON + RISC-V RVV 1.0 CPU vector engines
// (Pillar 5, D-52).
//
// WHY EXISTS: the CPU-vector rows of the spectrum pipeline for the two
// non-x86 ISAs. On aarch64 this driver fronts the SIMD core's NEON/SVE2
// engines; on riscv64 (with the V extension) it fronts RVV 1.0. On any
// other host it REFUSES at probe — the honest statement that this row's
// silicon is absent (the x86 PC row lives in driver_nvidia_pc.c, and the
// guaranteed terminal engine is weft-cpu-simd in the registry).
//
// The driver runs in HOST_VECTOR mode exclusively: ops execute on the
// SIMD core with device_ns honestly 0 — identical validation and result
// contract to the terminal engine, scored above it so the pipeline's
// arch-appropriate vector row dispatches first.
//
// DEVICE IDENTITY HONESTY: "arm-neon", "arm-sve2", or "riscv-rvv10" on
// the arches where they exist; "absent(wrong-arch)" everywhere else.
//
// LAW 1: state carved from the context arena at init; hot path inert.
// LAW 2: host memory IS the engine memory — zero copies by construction.

#include "weft_driver_core.h"
#include "../simd/weft_simd.h"

#include <string.h>

#define RA_RING_MAX 1024u
#define RA_MAPS_MAX 1024u
#define RA_STATE_BYTES                                                            \
    ((uint32_t)(sizeof(weft_driver_base_t) +                                      \
                RA_RING_MAX * sizeof(weft_cmd_pkt_t) +                            \
                RA_MAPS_MAX * sizeof(weft_driver_map_slot_t)))

typedef struct {
    weft_driver_base_t base;
    uint32_t host_seq;
    uint32_t pad[15];
} ra_state_t;

static weft_backend_status_t ra_probe(weft_backend_caps_t* caps) {
    if (caps == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    memset(caps, 0, sizeof(*caps));
    caps->abi_version = WEFT_BACKEND_ABI_VERSION;
    caps->vendor_id = WEFT_VENDOR_RISCV_ARM;
    caps->engine_class = WEFT_ENGINE_CPU_VECTOR;
    caps->flags = WEFT_CAPS_ZERO_COPY | WEFT_CAPS_UNIFIED_MEM;
    caps->op_affinity = WEFT_OP_AFFINITY(WEFT_OP_NORMALIZE_F32) |
                        WEFT_OP_AFFINITY(WEFT_OP_DELTA_ENCODE_U32) |
                        WEFT_OP_AFFINITY(WEFT_OP_DELTA_DECODE_U32) |
                        WEFT_OP_AFFINITY(WEFT_OP_DOT_F32) |
                        WEFT_OP_AFFINITY(WEFT_OP_SEQLOCK_CHECKSUM);
    caps->device_memory_bytes = 0;
    caps->score = 500;

#if defined(__aarch64__)
#if defined(__ARM_FEATURE_SVE2)
    strncpy(caps->impl_name, "arm-sve2", sizeof(caps->impl_name) - 1);
#else
    strncpy(caps->impl_name, "arm-neon", sizeof(caps->impl_name) - 1);
#endif
    return WEFT_BACKEND_OK;
#elif defined(__riscv) && defined(__riscv_v_intrinsic)
    strncpy(caps->impl_name, "riscv-rvv10", sizeof(caps->impl_name) - 1);
    return WEFT_BACKEND_OK;
#else
    strncpy(caps->impl_name, "absent(wrong-arch)", sizeof(caps->impl_name) - 1);
    return WEFT_BACKEND_EREFUSED;   // honest: this row's silicon is absent
#endif
}

static weft_backend_status_t ra_init(void* self,
                                     const weft_backend_init_cfg_t* cfg) {
    ra_state_t* st = (ra_state_t*)self;
    if (st == NULL || cfg == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    st->host_seq = 0;

#if defined(__aarch64__) || (defined(__riscv) && defined(__riscv_v_intrinsic))
    // HOST_VECTOR: the SIMD core is the engine (transport unused; an
    // injected transport would target the vector engine's queue — refused
    // because this row owns no device transport; the mock seam belongs to
    // the device rows).
    return weft_driver_base_init(&st->base, cfg, 0,
                                 WEFT_DRIVER_MODE_HOST_VECTOR, RA_STATE_BYTES);
#else
    (void)st;
    return WEFT_BACKEND_EREFUSED;
#endif
}

static weft_backend_status_t ra_execute(void* self, const weft_op_desc_t* op,
                                        weft_dispatch_result_t* out) {
    ra_state_t* st = (ra_state_t*)self;
    if (st == NULL || st->base.magic != WEFT_DRIVER_BASE_MAGIC) {
        return WEFT_BACKEND_ESTATE;
    }
    if (st->base.mode == WEFT_DRIVER_MODE_HOST_VECTOR) {
        return weft_driver_host_execute(op, out, &st->host_seq);
    }
    return WEFT_BACKEND_ESTATE;
}

static weft_backend_status_t ra_submit(void* self, const weft_op_desc_t* ops,
                                       uint32_t count) {
    ra_state_t* st = (ra_state_t*)self;
    if (st == NULL || st->base.magic != WEFT_DRIVER_BASE_MAGIC) {
        return WEFT_BACKEND_ESTATE;
    }
    if (st->base.mode != WEFT_DRIVER_MODE_HOST_VECTOR) {
        return WEFT_BACKEND_ESTATE;
    }
    if (ops == NULL || count == 0) {
        return WEFT_BACKEND_EINVAL;
    }
    const uint32_t kind = ops[0].kind;
    for (uint32_t i = 0; i < count; i++) {
        if (ops[i].kind != kind) {
            return WEFT_BACKEND_EINVAL;
        }
        const weft_backend_status_t s =
            weft_driver_host_execute(&ops[i], NULL, &st->host_seq);
        if (s != WEFT_BACKEND_OK) {
            return s;
        }
    }
    return WEFT_BACKEND_OK;
}

static weft_backend_status_t ra_sync(void* self, uint64_t completion_seq,
                                     uint64_t timeout_ns) {
    ra_state_t* st = (ra_state_t*)self;
    if (st == NULL || st->base.magic != WEFT_DRIVER_BASE_MAGIC) {
        return WEFT_BACKEND_ESTATE;
    }
    (void)completion_seq;
    (void)timeout_ns;
    return WEFT_BACKEND_OK;   // host vector: submit completed inline
}

static void ra_shutdown(void* self) {
    ra_state_t* st = (ra_state_t*)self;
    if (st == NULL || st->base.magic != WEFT_DRIVER_BASE_MAGIC) {
        return;
    }
    st->base.magic = 0;
}

const weft_backend_ops_t weft_driver_riscv_arm_ops = {
    .name         = "arm-sve2-riscv-rvv",
    .abi_version  = WEFT_BACKEND_ABI_VERSION,
    .vendor_id    = WEFT_VENDOR_RISCV_ARM,
    .engine_class = WEFT_ENGINE_CPU_VECTOR,
    .state_bytes  = RA_STATE_BYTES,
    .reserved0    = 0,
    .probe        = ra_probe,
    .init         = ra_init,
    .execute      = ra_execute,
    .submit       = ra_submit,
    .sync         = ra_sync,
    .shutdown     = ra_shutdown,
};
