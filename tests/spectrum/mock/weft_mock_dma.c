// weft_mock_dma.c — the synthetic hardware transport (see weft_mock_dma.h).
//
// DEVICE MODEL, honestly stated: this is not a simulator of any specific
// silicon — it is the deterministic DEVICE SIDE of the weft_dma_t seam.
// The latency formula, saturation window and doorbell semantics model the
// ENGINEERING envelope of the pipeline's devices (FastRPC cDSP channel,
// Neuropilot APU window, Metal command queue, CUDA stream), not their
// cycle-accurate behavior. The mock runs single-threaded (same contract
// as the drivers: a context is single-writer) — TSan-clean by design.
//
// LAW 2 ENFORCEMENT: map() never copies; the would-copy counter exists
// only so the injected Law-2-violating transport can be PROVEN caught.
// Completions write results through the pointers captured at enqueue —
// the caller's buffers mutate directly; no staging buffer exists in this
// file at all.

#include "weft_mock_dma.h"

#include "../../../core/c/spectrum/simd/weft_simd.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MOCK_MAP_SLOTS 256u
#define MOCK_FLIGHT_SLOTS 4096u

typedef struct {
    uint8_t  used;          // 0 = free, 1 = mapped
    uint8_t  broken;        // injected Law-2 violation marker
    uint64_t bytes;
    void*    data_at_map;
    void*    data_now;      // diverges only for the injected breaker
} mock_map_slot_t;

typedef struct {
    weft_cmd_pkt_t pkt;         // command snapshot (64B, captured at enqueue)
    void*   in_data;            // buffer bindings captured at enqueue
    void*   aux_data;
    void*   out_data;
    uint64_t bytes;             // latency-model byte count
    uint64_t deadline_ns;
} mock_flight_t;

typedef struct {
    weft_dma_transport_t vtbl;      // FIRST member: &mock == &mock->vtbl
    weft_mock_dma_cfg_t  cfg;

    mock_map_slot_t maps[MOCK_MAP_SLOTS];

    mock_flight_t  flight[MOCK_FLIGHT_SLOTS];
    uint32_t       flight_head;     // next to complete (FIFO)
    uint32_t       flight_count;

    uint64_t enq_seq;               // doorbell register mirror
    uint64_t done_seq;              // completion register mirror
    uint64_t inflight_bytes;

    // honest ledger
    uint64_t would_copies;
    uint64_t poll_count;
    uint64_t enqueued_pkts;
    uint64_t completed_pkts;
    uint64_t zero_copy_completions;

    // fault injection
    int device_gone;
    int break_zero_copy;
} weft_mock_dma_t;

// ---------------------------------------------------------------------------
// Clock (the device's virtual clock: CLOCK_MONOTONIC + zero offset here;
// the deadline formula is deterministic given the call sequence)
// ---------------------------------------------------------------------------

static uint64_t mock_now_ns(void* tctx) {
    (void)tctx;
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static weft_mock_dma_t* mock_of(void* tctx) {
    return (weft_mock_dma_t*)tctx;
}

// ---------------------------------------------------------------------------
// Device compute: the op surface executed through the captured pointers
// using the DISPATCHED vector engine (the synthetic device computes at
// the host's SIMD rate — a CONSERVATIVE model of real NPU/GPU silicon,
// which is faster still; bit-exact vs the scalar oracle by the proven
// construction, so end-to-end DEVICE-path results stay byte-identical)
// ---------------------------------------------------------------------------

static void mock_compute(const mock_flight_t* f) {
    const weft_cmd_pkt_t* p = &f->pkt;
    switch (p->op_kind) {
    case WEFT_OP_NORMALIZE_F32:
        weft_simd_normalize((float*)f->out_data, (const float*)f->in_data,
                            p->m, p->f0, p->f1);
        break;
    case WEFT_OP_DELTA_ENCODE_U32:
        weft_simd_delta_encode((uint32_t*)f->out_data,
                               (const uint32_t*)f->in_data, p->m, p->u0);
        break;
    case WEFT_OP_DELTA_DECODE_U32:
        weft_simd_delta_decode((uint32_t*)f->out_data,
                               (const uint32_t*)f->in_data, p->m, p->u0);
        break;
    case WEFT_OP_DOT_F32:
        weft_simd_dot_f32((float*)f->out_data, (const float*)f->in_data,
                          (const float*)f->aux_data, p->m, p->k, p->n,
                          p->lda, p->ldb, p->ldc);
        break;
    default:
        break;   // SEQLOCK_CHECKSUM never reaches a device (affinity law)
    }
}

// ---------------------------------------------------------------------------
// The transport vtable
// ---------------------------------------------------------------------------

static weft_backend_status_t mock_map(void* tctx, weft_buffer_desc_t* buf) {
    weft_mock_dma_t* m = mock_of(tctx);
    if (m == NULL || buf == NULL || buf->data == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    for (uint32_t i = 0; i < MOCK_MAP_SLOTS; i++) {
        mock_map_slot_t* s = &m->maps[i];
        if (!s->used) {
            s->used = 1;
            s->broken = 0;
            s->bytes = buf->bytes;
            s->data_at_map = buf->data;
            s->data_now = buf->data;
            if (m->break_zero_copy) {
                // INJECTED Law-2 violation: pretend the transport staged a
                // copy (the device address no longer aliases the host one).
                s->broken = 1;
                s->data_now = (unsigned char*)buf->data + 4096;   // "staged"
                m->would_copies++;
                m->break_zero_copy = 0;   // one-shot
            }
            buf->dma_tag = (uint64_t)(i + 1);
            buf->flags |= WEFT_BUF_DEVICE | WEFT_BUF_UNIFIED;
            if (s->broken) {
                buf->data = s->data_now;   // the lie the driver must catch
            }
            return WEFT_BACKEND_OK;
        }
    }
    return WEFT_BACKEND_EBUSY;   // bounded table: fail-closed, no eviction
}

static weft_backend_status_t mock_unmap(void* tctx, const weft_buffer_desc_t* buf) {
    weft_mock_dma_t* m = mock_of(tctx);
    if (m == NULL || buf == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    const uint64_t tag = buf->dma_tag;
    if (tag == 0 || tag > MOCK_MAP_SLOTS || !m->maps[tag - 1].used) {
        return WEFT_BACKEND_EINVAL;   // unmapping an unmapped tag
    }
    m->maps[tag - 1].used = 0;
    return WEFT_BACKEND_OK;
}

static void* mock_data_of(weft_mock_dma_t* m, uint32_t tag) {
    if (tag == 0 || tag > MOCK_MAP_SLOTS || !m->maps[tag - 1].used) {
        return NULL;
    }
    return m->maps[tag - 1].data_at_map;   // binding captured at map time
}

static uint64_t mock_bytes_of(weft_mock_dma_t* m, uint32_t tag) {
    if (tag == 0 || tag > MOCK_MAP_SLOTS || !m->maps[tag - 1].used) {
        return 0;
    }
    return m->maps[tag - 1].bytes;
}

static weft_backend_status_t mock_enqueue(void* tctx, weft_cmd_pkt_t* pkts,
                                          uint32_t count) {
    weft_mock_dma_t* m = mock_of(tctx);
    if (m == NULL || pkts == NULL || count == 0) {
        return WEFT_BACKEND_EINVAL;
    }
    if (m->device_gone) {
        return WEFT_BACKEND_EDEVICE;   // hot-unplug: honest, fatal for driver
    }
    if (m->flight_count + count > MOCK_FLIGHT_SLOTS) {
        return WEFT_BACKEND_EBUSY;     // ring exhausted: fail-closed
    }

    // Latency model byte count: in + aux + out (device reads inputs,
    // writes outputs — both cross the bus). All-or-nothing saturation:
    // check capacity for the WHOLE burst before committing any packet.
    uint64_t burst_bytes = 0;
    for (uint32_t i = 0; i < count; i++) {
        burst_bytes += mock_bytes_of(m, pkts[i].in_tag);
        burst_bytes += mock_bytes_of(m, pkts[i].aux_tag);
        burst_bytes += mock_bytes_of(m, pkts[i].out_tag);
    }
    if (m->inflight_bytes + burst_bytes > m->cfg.capacity_bytes) {
        return WEFT_BACKEND_EBUSY;     // bus saturated: honest backpressure
    }

    const uint64_t now = mock_now_ns(tctx);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t slot =
            (m->flight_head + m->flight_count) % MOCK_FLIGHT_SLOTS;
        mock_flight_t* f = &m->flight[slot];
        f->pkt = pkts[i];              // 64B command capture (control plane)
        f->in_data = mock_data_of(m, pkts[i].in_tag);    // zero-copy binding
        f->aux_data = mock_data_of(m, pkts[i].aux_tag);
        f->out_data = mock_data_of(m, pkts[i].out_tag);
        if ((pkts[i].flags & WEFT_OP_FLAG_INPLACE) != 0u) {
            f->out_data = f->in_data;  // dst aliases src
        }
        f->bytes = mock_bytes_of(m, pkts[i].in_tag) +
                   mock_bytes_of(m, pkts[i].aux_tag) +
                   mock_bytes_of(m, pkts[i].out_tag);
        // The latency model: fixed DMA overhead + bus transfer time
        // (ps_per_byte keeps the arithmetic integer: a 20 GB/s bus = 50).
        f->deadline_ns = now + m->cfg.fixed_ns +
                         (f->bytes * m->cfg.ps_per_byte) / 1000ull;
        m->flight_count++;
        m->enq_seq++;
        m->enqueued_pkts++;
        m->inflight_bytes += f->bytes;
    }
    return WEFT_BACKEND_OK;
}

static weft_backend_status_t mock_poll(void* tctx, uint32_t* done_seq_out) {
    weft_mock_dma_t* m = mock_of(tctx);
    if (m == NULL || done_seq_out == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    m->poll_count++;
    if (m->device_gone) {
        return WEFT_BACKEND_EDEVICE;
    }
    const uint64_t now = mock_now_ns(tctx);
    // Advance the completion doorbell for every flight whose deadline the
    // device clock has cleared — then EXECUTE the compute through the
    // captured (aliased) pointers. This is the register-poll emulation.
    while (m->flight_count > 0) {
        mock_flight_t* f = &m->flight[m->flight_head];
        if (f->deadline_ns > now) {
            break;
        }
        if (f->in_data != NULL && f->out_data != NULL) {
            mock_compute(f);
            m->zero_copy_completions++;
        }
        m->completed_pkts++;
        m->inflight_bytes -= f->bytes;
        m->done_seq++;
        m->flight_head = (m->flight_head + 1) % MOCK_FLIGHT_SLOTS;
        m->flight_count--;
    }
    *done_seq_out = (uint32_t)m->done_seq;
    return WEFT_BACKEND_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle + witnesses + injection
// ---------------------------------------------------------------------------

weft_dma_transport_t* weft_mock_dma_new(const weft_mock_dma_cfg_t* cfg) {
    if (cfg == NULL || cfg->name == NULL) {
        return NULL;
    }
    weft_mock_dma_t* m = (weft_mock_dma_t*)calloc(1, sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    m->cfg = *cfg;
    m->vtbl.name = cfg->name;
    m->vtbl.transport_ctx = m;
    m->vtbl.map = mock_map;
    m->vtbl.enqueue = mock_enqueue;
    m->vtbl.poll = mock_poll;
    m->vtbl.unmap = mock_unmap;
    m->vtbl.now_ns = mock_now_ns;
    return &m->vtbl;
}

void weft_mock_dma_destroy(weft_dma_transport_t* t) {
    if (t == NULL) {
        return;
    }
    weft_mock_dma_t* m = (weft_mock_dma_t*)t->transport_ctx;
    free(m);
}

uint64_t weft_mock_dma_would_copies(const weft_dma_transport_t* t) {
    if (t == NULL) return 0;
    return ((const weft_mock_dma_t*)t->transport_ctx)->would_copies;
}
uint64_t weft_mock_dma_poll_count(const weft_dma_transport_t* t) {
    if (t == NULL) return 0;
    return ((const weft_mock_dma_t*)t->transport_ctx)->poll_count;
}
uint64_t weft_mock_dma_inflight_bytes(const weft_dma_transport_t* t) {
    if (t == NULL) return 0;
    return ((const weft_mock_dma_t*)t->transport_ctx)->inflight_bytes;
}
uint64_t weft_mock_dma_enqueued_pkts(const weft_dma_transport_t* t) {
    if (t == NULL) return 0;
    return ((const weft_mock_dma_t*)t->transport_ctx)->enqueued_pkts;
}
uint64_t weft_mock_dma_completed_pkts(const weft_dma_transport_t* t) {
    if (t == NULL) return 0;
    return ((const weft_mock_dma_t*)t->transport_ctx)->completed_pkts;
}
uint64_t weft_mock_dma_zero_copy_completions(const weft_dma_transport_t* t) {
    if (t == NULL) return 0;
    return ((const weft_mock_dma_t*)t->transport_ctx)->zero_copy_completions;
}

void weft_mock_dma_inject_device_gone(weft_dma_transport_t* t) {
    if (t == NULL) return;
    ((weft_mock_dma_t*)t->transport_ctx)->device_gone = 1;
}

void weft_mock_dma_break_zero_copy(weft_dma_transport_t* t) {
    if (t == NULL) return;
    ((weft_mock_dma_t*)t->transport_ctx)->break_zero_copy = 1;
}
