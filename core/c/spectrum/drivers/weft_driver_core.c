// weft_driver_core.c — shared engine machinery (see weft_driver_core.h).
//
// Law 1: no allocation anywhere in this file's execution paths.
// Law 2: map() must leave the host pointer untouched — enforced below.
// Law 3: EDEVICE marks the driver DEAD and propagates for a fallback hop.
// Law 4: every argument law is checked fail-closed before any side effect.

#include "weft_driver_core.h"
#include "../simd/weft_simd.h"

#include <sched.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Bounded wait ladder (the tensor module's Law-2 discipline: fixed rungs,
// caller deadline checked between rungs — no unbounded spinning).
// ---------------------------------------------------------------------------

#define WB_LADDER_PAUSE_ROUNDS 64u   // (legacy constant; ladder is time-budgeted)
#define WB_LADDER_SLEEP_NS     50000ull   ///< 50 us rung
#define WB_LADDER_SPIN_NS      64000ull   ///< pause-spin phase budget

static void wb_pause(void) {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("yield" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

uint64_t weft_driver_now_ns(void) { return weft_backend_now_ns(); }

// ---------------------------------------------------------------------------
// Base init: one arena block, header + cmd ring + map table
// ---------------------------------------------------------------------------

weft_backend_status_t weft_driver_base_init(weft_driver_base_t* base,
                                            const weft_backend_init_cfg_t* cfg,
                                            uint32_t backend_idx,
                                            uint32_t mode,
                                            uint64_t state_bytes_total) {
    if (base == NULL || cfg == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    if (state_bytes_total < sizeof(weft_driver_base_t)) {
        return WEFT_BACKEND_ERANGE;
    }
    if (mode == WEFT_DRIVER_MODE_DEVICE && cfg->transport == NULL) {
        return WEFT_BACKEND_EINVAL;   // device mode REQUIRES a transport
    }

    base->magic         = WEFT_DRIVER_BASE_MAGIC;
    base->mode          = mode;
    base->backend_idx   = backend_idx;
    base->cmd_slots     = cfg->cmd_ring_slots;
    base->map_slots     = cfg->dma_map_slots;
    base->transport     = (mode == WEFT_DRIVER_MODE_DEVICE) ? cfg->transport : NULL;
    base->owner         = cfg->owner;
    base->log           = cfg->log;
    base->submitted_seq = 0;
    base->completed_seq = 0;
    base->map_count     = 0;
    base->reserved0     = 0;

    // Carve the command ring + map table behind the header (the registry
    // allocated state_bytes_total for us; arrays of 64B structs stay
    // aligned behind the header in the 64B-aligned state block).
    const uint64_t need = sizeof(weft_driver_base_t) +
                          (uint64_t)cfg->cmd_ring_slots * sizeof(weft_cmd_pkt_t) +
                          (uint64_t)cfg->dma_map_slots * sizeof(weft_driver_map_slot_t);
    if (state_bytes_total < need) {
        return WEFT_BACKEND_ERANGE;
    }
    weft_cmd_pkt_t* ring = (weft_cmd_pkt_t*)((unsigned char*)base + sizeof(*base));
    weft_driver_map_slot_t* maps =
        (weft_driver_map_slot_t*)((unsigned char*)ring +
                                  (uint64_t)cfg->cmd_ring_slots * sizeof(weft_cmd_pkt_t));
    for (uint32_t i = 0; i < cfg->cmd_ring_slots; i++) {
        ring[i].op_kind = 0;         // poison: kind 0 is not a valid op
        ring[i].reserved0 = 0;
    }
    for (uint32_t i = 0; i < cfg->dma_map_slots; i++) {
        maps[i].tag = 0;
        maps[i].reserved = 0;
    }
    return WEFT_BACKEND_OK;
}

static weft_cmd_pkt_t* wb_ring(weft_driver_base_t* base) {
    return (weft_cmd_pkt_t*)((unsigned char*)base + sizeof(weft_driver_base_t));
}
static weft_driver_map_slot_t* wb_maps(weft_driver_base_t* base) {
    return (weft_driver_map_slot_t*)
        ((unsigned char*)base + sizeof(weft_driver_base_t) +
         (uint64_t)base->cmd_slots * sizeof(weft_cmd_pkt_t));
}

// ---------------------------------------------------------------------------
// Fail-closed op validation (the argument law every engine enforces)
// ---------------------------------------------------------------------------

weft_backend_status_t weft_driver_validate_op(const weft_op_desc_t* op) {
    if (op == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    const weft_buffer_desc_t* dst =
        (op->flags & WEFT_OP_FLAG_INPLACE) ? &op->bufs[0] : &op->bufs[2];

    switch (op->kind) {
    case WEFT_OP_NORMALIZE_F32:
    case WEFT_OP_DELTA_ENCODE_U32:
    case WEFT_OP_DELTA_DECODE_U32: {
        if (op->m == 0) {
            return WEFT_BACKEND_EINVAL;
        }
        const uint32_t want_dtype =
            (op->kind == WEFT_OP_NORMALIZE_F32) ? WEFT_BACKEND_DTYPE_F32
                                                : WEFT_BACKEND_DTYPE_U32;
        if (op->bufs[0].data == NULL || dst->data == NULL ||
            op->bufs[0].dtype != want_dtype || dst->dtype != want_dtype) {
            return WEFT_BACKEND_EINVAL;
        }
        if ((uint64_t)op->m * 4u > op->bufs[0].bytes ||
            (uint64_t)op->m * 4u > dst->bytes) {
            return WEFT_BACKEND_ERANGE;
        }
        return WEFT_BACKEND_OK;
    }
    case WEFT_OP_DOT_F32: {
        if (op->m == 0 || op->k == 0 || op->n == 0) {
            return WEFT_BACKEND_EINVAL;
        }
        if (op->lda < op->k || op->ldb < op->n || op->ldc < op->n) {
            return WEFT_BACKEND_ERANGE;
        }
        const uint64_t a_elems = (uint64_t)(op->m - 1u) * op->lda + op->k;
        const uint64_t b_elems = (uint64_t)(op->k - 1u) * op->ldb + op->n;
        const uint64_t c_elems = (uint64_t)(op->m - 1u) * op->ldc + op->n;
        if (a_elems >= (1ull << 30) || b_elems >= (1ull << 30) ||
            c_elems >= (1ull << 30)) {
            return WEFT_BACKEND_ERANGE;
        }
        if (op->bufs[0].data == NULL || op->bufs[1].data == NULL ||
            op->bufs[2].data == NULL ||
            op->bufs[0].dtype != WEFT_BACKEND_DTYPE_F32 ||
            op->bufs[1].dtype != WEFT_BACKEND_DTYPE_F32 ||
            op->bufs[2].dtype != WEFT_BACKEND_DTYPE_F32) {
            return WEFT_BACKEND_EINVAL;
        }
        if (a_elems * 4u > op->bufs[0].bytes || b_elems * 4u > op->bufs[1].bytes ||
            c_elems * 4u > op->bufs[2].bytes) {
            return WEFT_BACKEND_ERANGE;
        }
        return WEFT_BACKEND_OK;
    }
    case WEFT_OP_SEQLOCK_CHECKSUM: {
        if (op->bufs[0].data == NULL || op->bufs[0].bytes < 2 ||
            (op->bufs[0].bytes & 1u) != 0) {
            return WEFT_BACKEND_EINVAL;
        }
        return WEFT_BACKEND_OK;
    }
    default:
        return WEFT_BACKEND_EINVAL;   // unknown kind: fail-closed
    }
}

// ---------------------------------------------------------------------------
// Transport plumbing: map / form / enqueue / poll / unmap
// ---------------------------------------------------------------------------

/// Map one operand. The TRANSPORT assigns desc->dma_tag (the device
/// handle); the driver records the descriptor in its bounded table and
/// returns its own slot id for unmap bookkeeping. Law 2 is structurally
/// enforced: the host pointer must survive map() untouched, and the device
/// handle must fit the 32-bit command-packet tag ABI.
static weft_backend_status_t wb_map_one(weft_driver_base_t* base,
                                        const weft_buffer_desc_t* buf,
                                        uint32_t* slot_out,
                                        uint32_t* dev_tag_out) {
    *slot_out = 0;
    *dev_tag_out = 0;
    if (buf->data == NULL) {
        return WEFT_BACKEND_OK;   // absent operand (e.g. checksum has no dst)
    }
    weft_driver_map_slot_t* maps = wb_maps(base);
    for (uint32_t i = 0; i < base->map_slots; i++) {
        if (maps[i].tag == 0) {
            maps[i].desc = *buf;                       // 64B copy, no heap
            weft_backend_status_t st = base->transport->map(
                base->transport->transport_ctx, &maps[i].desc);
            if (st != WEFT_BACKEND_OK) {
                maps[i].tag = 0;
                return st;   // engine refused / map table upstream exhausted
            }
            if (maps[i].desc.data != buf->data) {
                // Law 2 structural violation: the transport COPIED.
                maps[i].tag = 0;
                (void)base->transport->unmap(base->transport->transport_ctx,
                                             &maps[i].desc);
                return WEFT_BACKEND_ESTATE;
            }
            if (maps[i].desc.dma_tag > 0xFFFFFFFFull) {
                // The frozen 64B command packet carries 32-bit handles.
                maps[i].tag = 0;
                (void)base->transport->unmap(base->transport->transport_ctx,
                                             &maps[i].desc);
                return WEFT_BACKEND_ESTATE;
            }
            maps[i].tag = i + 1;                       // driver-side slot id
            base->map_count++;
            *slot_out = maps[i].tag;
            *dev_tag_out = (uint32_t)maps[i].desc.dma_tag;
            return WEFT_BACKEND_OK;
        }
    }
    return WEFT_BACKEND_EBUSY;   // fail-closed: bounded tables, never eviction
}

static void wb_unmap_slot(weft_driver_base_t* base, uint32_t slot_id) {
    if (slot_id == 0) {
        return;
    }
    weft_driver_map_slot_t* maps = wb_maps(base);
    weft_driver_map_slot_t* slot = &maps[slot_id - 1];
    if (slot->tag == slot_id) {
        (void)base->transport->unmap(base->transport->transport_ctx, &slot->desc);
        slot->tag = 0;
        base->map_count--;
    }
}

weft_backend_status_t weft_driver_poll_once(weft_driver_base_t* base,
                                            uint32_t* done_seq_out) {
    if (base == NULL || base->transport == NULL || done_seq_out == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    return base->transport->poll(base->transport->transport_ctx, done_seq_out);
}

static void wb_form_pkt(const weft_op_desc_t* op, uint32_t in_tag,
                        uint32_t aux_tag, uint32_t out_tag,
                        weft_cmd_pkt_t* pkt) {
    pkt->op_kind  = op->kind;
    pkt->flags    = op->flags;
    pkt->in_tag   = in_tag;
    pkt->aux_tag  = aux_tag;
    pkt->out_tag  = out_tag;
    pkt->m = op->m; pkt->k = op->k; pkt->n = op->n;
    pkt->lda = op->lda; pkt->ldb = op->ldb; pkt->ldc = op->ldc;
    pkt->f0 = op->f0;  pkt->f1 = op->f1;
    pkt->u0 = op->u0;  pkt->u1 = op->u1;
}

static weft_backend_status_t wb_wait_completion(weft_driver_base_t* base,
                                                uint32_t want_seq,
                                                uint64_t timeout_ns,
                                                uint32_t* done_out) {
    const uint64_t start = weft_backend_now_ns();
    uint32_t done = base->completed_seq;
    // Bounded wait ladder, TIME-budgeted (register-poll discipline): the
    // pause-spin phase covers the first ~64 us of waiting — the entire
    // latency envelope of the pipeline's devices (single-digit-us DMA
    // deadlines) — and only then do the yield/50us-sleep rungs engage.
    // Rung counts alone made deadlines that straddled 64 pauses bimodal
    // (a 5-us deadline must never cost a 50-us sleep).
    const uint64_t spin_budget_ns = 64000ull;
    for (;;) {
        uint32_t t = 0;
        weft_backend_status_t st =
            base->transport->poll(base->transport->transport_ctx, &t);
        if (st == WEFT_BACKEND_EDEVICE) {
            base->mode = WEFT_DRIVER_MODE_DEAD;      // hot-unplug: Law 3
            return WEFT_BACKEND_EDEVICE;
        }
        if (st != WEFT_BACKEND_OK) {
            return st;
        }
        if (t > done) {
            done = t;
            base->completed_seq = t;
        }
        if (done >= want_seq) {
            *done_out = done;
            return WEFT_BACKEND_OK;
        }
        const uint64_t now = weft_backend_now_ns();
        if (now - start >= timeout_ns) {
            *done_out = done;
            return WEFT_BACKEND_ETIMEOUT;
        }
        if (now - start < spin_budget_ns) {
            wb_pause();
        } else {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = WB_LADDER_SLEEP_NS };
            (void)nanosleep(&ts, NULL);
            (void)sched_yield();
        }
    }
}

// ---------------------------------------------------------------------------
// Generic DEVICE execution path
// ---------------------------------------------------------------------------

/// The device engines accelerate the data-plane transforms. The seqlock
/// checksum returns its digest through the HOST result register — a
/// device engine answers ENOTSUP (honest affinity boundary; the registry
/// routes checksums to CPU engines that own the host contract).
static int wb_device_supports(uint32_t kind) {
    return kind == WEFT_OP_NORMALIZE_F32 || kind == WEFT_OP_DELTA_ENCODE_U32 ||
           kind == WEFT_OP_DELTA_DECODE_U32 || kind == WEFT_OP_DOT_F32;
}

weft_backend_status_t weft_driver_device_execute(weft_driver_base_t* base,
                                                 const weft_op_desc_t* op,
                                                 weft_dispatch_result_t* out) {
    if (base == NULL || base->magic != WEFT_DRIVER_BASE_MAGIC ||
        base->mode != WEFT_DRIVER_MODE_DEVICE) {
        return WEFT_BACKEND_ESTATE;
    }
    if (!wb_device_supports(op->kind)) {
        return WEFT_BACKEND_ENOTSUP;
    }
    weft_backend_status_t st = weft_driver_validate_op(op);
    if (st != WEFT_BACKEND_OK) {
        return st;
    }

    const uint64_t t0 = weft_backend_now_ns();
    uint32_t in_slot = 0, aux_slot = 0, out_slot = 0;
    uint32_t in_tag = 0, aux_tag = 0, out_tag = 0;
    st = wb_map_one(base, &op->bufs[0], &in_slot, &in_tag);
    if (st == WEFT_BACKEND_OK) {
        st = wb_map_one(base, &op->bufs[1], &aux_slot, &aux_tag);
    }
    if (st == WEFT_BACKEND_OK) {
        const weft_buffer_desc_t* dst =
            (op->flags & WEFT_OP_FLAG_INPLACE) ? &op->bufs[0] : &op->bufs[2];
        st = wb_map_one(base, dst, &out_slot, &out_tag);
    }
    if (st != WEFT_BACKEND_OK) {
        wb_unmap_slot(base, in_slot);
        wb_unmap_slot(base, aux_slot);
        wb_unmap_slot(base, out_slot);
        if (st == WEFT_BACKEND_EDEVICE) {
            base->mode = WEFT_DRIVER_MODE_DEAD;
        }
        return st;
    }

    weft_cmd_pkt_t* ring = wb_ring(base);
    const uint32_t slot = base->submitted_seq % base->cmd_slots;
    weft_cmd_pkt_t* pkt = &ring[slot];
    wb_form_pkt(op, in_tag, aux_tag, out_tag, pkt);
    const uint64_t enqueue_ns =
        base->transport->now_ns(base->transport->transport_ctx);

    st = base->transport->enqueue(base->transport->transport_ctx, pkt, 1);
    if (st != WEFT_BACKEND_OK) {
        wb_unmap_slot(base, in_slot);
        wb_unmap_slot(base, aux_slot);
        wb_unmap_slot(base, out_slot);
        if (st == WEFT_BACKEND_EDEVICE) {
            base->mode = WEFT_DRIVER_MODE_DEAD;
        }
        return st;   // EBUSY (bus saturated) / EDEVICE propagate honestly
    }
    const uint32_t want_seq = ++base->submitted_seq;

    uint32_t done = 0;
    st = wb_wait_completion(base, want_seq, 50ull * 1000ull * 1000ull, &done);
    const uint64_t t1 = weft_backend_now_ns();

    wb_unmap_slot(base, in_slot);
    wb_unmap_slot(base, aux_slot);
    wb_unmap_slot(base, out_slot);

    if (out != NULL) {
        out->result_u64    = 0;
        out->completed_seq = done;
        out->enqueue_ns    = enqueue_ns;
        out->complete_ns   = t1;
        out->device_ns     = (t1 > t0) ? (t1 - t0) : 0;
    }
    return st;
}

weft_backend_status_t weft_driver_device_submit(weft_driver_base_t* base,
                                                const weft_op_desc_t* ops,
                                                uint32_t count) {
    if (base == NULL || base->magic != WEFT_DRIVER_BASE_MAGIC ||
        base->mode != WEFT_DRIVER_MODE_DEVICE) {
        return WEFT_BACKEND_ESTATE;
    }
    if (ops == NULL || count == 0 || count > base->cmd_slots) {
        return WEFT_BACKEND_EINVAL;
    }
    const uint32_t kind = ops[0].kind;
    for (uint32_t i = 0; i < count; i++) {
        if (ops[i].kind != kind) {
            return WEFT_BACKEND_EINVAL;   // homogeneous batches only
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!wb_device_supports(ops[i].kind)) {
            return WEFT_BACKEND_ENOTSUP;
        }
        weft_backend_status_t st = weft_driver_validate_op(&ops[i]);
        if (st != WEFT_BACKEND_OK) {
            return st;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        const weft_op_desc_t* op = &ops[i];
        uint32_t in_slot = 0, aux_slot = 0, out_slot = 0;
        uint32_t in_tag = 0, aux_tag = 0, out_tag = 0;
        weft_backend_status_t st = wb_map_one(base, &op->bufs[0], &in_slot, &in_tag);
        if (st == WEFT_BACKEND_OK) {
            st = wb_map_one(base, &op->bufs[1], &aux_slot, &aux_tag);
        }
        if (st == WEFT_BACKEND_OK) {
            const weft_buffer_desc_t* dst =
                (op->flags & WEFT_OP_FLAG_INPLACE) ? &op->bufs[0] : &op->bufs[2];
            st = wb_map_one(base, dst, &out_slot, &out_tag);
        }
        if (st != WEFT_BACKEND_OK) {
            wb_unmap_slot(base, in_slot);
            wb_unmap_slot(base, aux_slot);
            wb_unmap_slot(base, out_slot);
            if (st == WEFT_BACKEND_EDEVICE) {
                base->mode = WEFT_DRIVER_MODE_DEAD;
            }
            return st;
        }
        weft_cmd_pkt_t* ring = wb_ring(base);
        const uint32_t slot = base->submitted_seq % base->cmd_slots;
        weft_cmd_pkt_t* pkt = &ring[slot];
        wb_form_pkt(op, in_tag, aux_tag, out_tag, pkt);
        st = base->transport->enqueue(base->transport->transport_ctx, pkt, 1);
        base->submitted_seq++;
        wb_unmap_slot(base, in_slot);
        wb_unmap_slot(base, aux_slot);
        wb_unmap_slot(base, out_slot);
        if (st != WEFT_BACKEND_OK) {
            if (st == WEFT_BACKEND_EDEVICE) {
                base->mode = WEFT_DRIVER_MODE_DEAD;
            }
            return st;
        }
    }
    return WEFT_BACKEND_OK;
}

weft_backend_status_t weft_driver_device_sync(weft_driver_base_t* base,
                                              uint64_t want_seq,
                                              uint64_t timeout_ns) {
    if (base == NULL || base->magic != WEFT_DRIVER_BASE_MAGIC) {
        return WEFT_BACKEND_ESTATE;
    }
    if (base->mode != WEFT_DRIVER_MODE_DEVICE) {
        return WEFT_BACKEND_OK;   // host engine: submit already completed
    }
    if (want_seq > 0xFFFFFFFFull) {
        return WEFT_BACKEND_ERANGE;
    }
    uint32_t done = 0;
    return wb_wait_completion(base, (uint32_t)want_seq, timeout_ns, &done);
}

// ---------------------------------------------------------------------------
// HOST-VECTOR execution path (terminal engine / PC / ARM-RISC-V host modes)
// ---------------------------------------------------------------------------

weft_backend_status_t weft_driver_host_execute(const weft_op_desc_t* op,
                                               weft_dispatch_result_t* out,
                                               uint32_t* seq_io) {
    weft_backend_status_t st = weft_driver_validate_op(op);
    if (st != WEFT_BACKEND_OK) {
        return st;
    }
    const uint64_t t0 = weft_backend_now_ns();
    const weft_buffer_desc_t* dst =
        (op->flags & WEFT_OP_FLAG_INPLACE) ? &op->bufs[0] : &op->bufs[2];

    switch (op->kind) {
    case WEFT_OP_NORMALIZE_F32:
        weft_simd_normalize((float*)dst->data, (const float*)op->bufs[0].data,
                            op->m, op->f0, op->f1);
        break;
    case WEFT_OP_DELTA_ENCODE_U32:
        weft_simd_delta_encode((uint32_t*)dst->data,
                               (const uint32_t*)op->bufs[0].data, op->m, op->u0);
        break;
    case WEFT_OP_DELTA_DECODE_U32:
        weft_simd_delta_decode((uint32_t*)dst->data,
                               (const uint32_t*)op->bufs[0].data, op->m, op->u0);
        break;
    case WEFT_OP_DOT_F32:
        weft_simd_dot_f32((float*)op->bufs[2].data, (const float*)op->bufs[0].data,
                          (const float*)op->bufs[1].data, op->m, op->k, op->n,
                          op->lda, op->ldb, op->ldc);
        break;
    case WEFT_OP_SEQLOCK_CHECKSUM: {
        uint32_t digest = weft_simd_seqlock_checksum(op->bufs[0].data,
                                                     op->bufs[0].bytes, op->u0);
        if (out != NULL) {
            out->result_u64 = digest;
        }
        break;
    }
    default:
        return WEFT_BACKEND_EINVAL;
    }

    const uint64_t t1 = weft_backend_now_ns();
    if (seq_io != NULL) {
        *seq_io += 1;
    }
    if (out != NULL) {
        out->completed_seq = (seq_io != NULL) ? *seq_io : 0;
        out->enqueue_ns   = t0;
        out->complete_ns  = t1;
        out->device_ns    = 0;   // honest: CPU execution is host time
    }
    return WEFT_BACKEND_OK;
}
