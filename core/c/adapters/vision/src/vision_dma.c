// vision_dma.c — the zero-copy frame ingestion engine (see the header for
// the contract). The hot path (dequeue -> descriptor activation) is:
//   * one backend dqbuf (mock: atomics + optional futex; V4L2: ioctl),
//   * a fixed 176-byte view template copy + DLPack pointer fill,
//   * two clock reads for the handoff measurement.
// No allocation, no syscalls beyond the backend's own dequeue.

#include "weft_vision_dma.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define WEFT_FUTEX_WAIT 0
#define WEFT_FUTEX_WAKE 1

/* weak: only test/bench binaries link the mock device module */
__attribute__((weak)) const weft_vision_backend_ops_t *
    weft_vision_mock_backend(void) {
    return NULL;
}

const char *weft_vision_status_name(int st) {
    switch (st) {
        case WEFT_VISION_OK: return "WEFT_VISION_OK";
        case WEFT_VISION_EINVAL: return "WEFT_VISION_EINVAL";
        case WEFT_VISION_ENODEV: return "WEFT_VISION_ENODEV";
        case WEFT_VISION_ENOMEM: return "WEFT_VISION_ENOMEM";
        case WEFT_VISION_ETIMEOUT: return "WEFT_VISION_ETIMEOUT";
        case WEFT_VISION_ESTATE: return "WEFT_VISION_ESTATE";
        case WEFT_VISION_EIO: return "WEFT_VISION_EIO";
        case WEFT_VISION_EMAGIC: return "WEFT_VISION_EMAGIC";
        case WEFT_VISION_EDMABUF: return "WEFT_VISION_EDMABUF";
        default: return "WEFT_VISION_E<unknown>";
    }
}

uint32_t weft_vision_pixfmt_bpp(weft_vision_pixfmt_t f) {
    switch (f) {
        case WEFT_VISION_PIXFMT_RGBX8888: return 4;
        case WEFT_VISION_PIXFMT_YUYV: return 2;
        case WEFT_VISION_PIXFMT_GRAY8: return 1;
        default: return 0;
    }
}

void weft_vision_dma_config_default(weft_vision_dma_config_t *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->backend = WEFT_VISION_BACKEND_AUTO;
    snprintf(cfg->path, sizeof(cfg->path), "/dev/video0");
    cfg->width = 1920;
    cfg->height = 1080;
    cfg->pixfmt = WEFT_VISION_PIXFMT_RGBX8888;
    cfg->fps = 30.0;
    cfg->buffer_count = 4;
    cfg->mock_dma_latency_ns = 0;
    cfg->mock_swath_div = 0;
}

static int64_t vis_now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

static weft_dtype_t vis_dtype(weft_vision_pixfmt_t f) {
    (void)f;
    return WEFT_DTYPE_U8; /* raw sensor bytes until ISP/convert stage */
}

// ---------------------------------------------------------------------------
// Open / start
// ---------------------------------------------------------------------------

int weft_vision_dma_open(const weft_vision_dma_config_t *cfg,
                         weft_vision_dma_t **out) {
    if (cfg == NULL || out == NULL) return WEFT_VISION_EINVAL;
    if (cfg->width == 0 || cfg->height == 0 ||
        cfg->width > 16384 || cfg->height > 16384) {
        return WEFT_VISION_EINVAL;
    }
    if (cfg->fps < 1.0 || cfg->fps > 480.0) return WEFT_VISION_EINVAL;
    if (cfg->buffer_count < 2 ||
        cfg->buffer_count > WEFT_VISION_MAX_BUFFERS) {
        return WEFT_VISION_EINVAL;
    }
    if (weft_vision_pixfmt_bpp(cfg->pixfmt) == 0) return WEFT_VISION_EINVAL;

    weft_vision_dma_t *eng = calloc(1, sizeof(*eng));
    if (eng == NULL) return WEFT_VISION_ENOMEM;
    eng->cfg = *cfg;
    eng->buffer_count = cfg->buffer_count;
    eng->frame_bytes = cfg->width * cfg->height *
                       weft_vision_pixfmt_bpp(cfg->pixfmt);
    if (eng->frame_bytes > (256u << 20)) {
        free(eng);
        return WEFT_VISION_EINVAL;
    }
    eng->period_ns = (uint64_t)(1000000000.0 / cfg->fps);

    const weft_vision_backend_ops_t *ops = NULL;
    if (cfg->backend == WEFT_VISION_BACKEND_MOCK) {
        ops = weft_vision_mock_backend();
        if (ops == NULL) {
            free(eng);
            return WEFT_VISION_ENODEV; /* fail closed, honest */
        }
    } else {
        ops = weft_vision_v4l2_backend();
    }
    eng->ops = ops;

    int rc = ops->open(eng);
    if (rc != WEFT_VISION_OK) {
        free(eng);
        return rc;
    }

    /* Precompute the per-buffer view templates (the zero-copy handoff is
     * a memcpy of one of these + two pointer fills). */
    uint64_t shape[WEFT_TENSOR_MAX_DIMS] = {0, 0, 0, 0, 0, 0, 0, 0};
    shape[0] = cfg->height;
    shape[1] = cfg->width;
    shape[2] = weft_vision_pixfmt_bpp(cfg->pixfmt);
    for (uint32_t i = 0; i < eng->buffer_count; i++) {
        int r = weft_tensor_view_init(&eng->tmpl[i], 0, vis_dtype(cfg->pixfmt),
                                      3, shape, (uintptr_t)eng->buf_map[i], 0);
        if (r != WEFT_TENSOR_OK) {
            (void)ops->close(eng);
            free(eng);
            return WEFT_VISION_EINVAL;
        }
        /* shape/strides arrays for DLPack: filled ONCE, per descriptor */
        weft_vision_frame_t *fr = &eng->frames[i];
        for (int k = 0; k < WEFT_TENSOR_MAX_DIMS; k++) {
            fr->shape_i64[k] = (int64_t)eng->tmpl[i].shape[k];
            fr->strides_i64[k] = eng->tmpl[i].strides[k];
        }
        atomic_init(&eng->frame_in_use[i], 0u);
    }

    *out = eng;
    return WEFT_VISION_OK;
}

int weft_vision_dma_stream_start(weft_vision_dma_t *eng) {
    if (eng == NULL) return WEFT_VISION_EINVAL;
    if (eng->streaming) return WEFT_VISION_ESTATE;
    int rc = eng->ops->stream_on(eng);
    if (rc != WEFT_VISION_OK) return rc;
    eng->streaming = 1;
    return WEFT_VISION_OK;
}

// ---------------------------------------------------------------------------
// Hot path: dequeue / release
// ---------------------------------------------------------------------------

static void frame_dl_deleter(DLManagedTensor *self);

int weft_vision_dma_dequeue(weft_vision_dma_t *eng,
                            weft_vision_frame_t **frame_out,
                            int64_t timeout_ns) {
    if (eng == NULL || frame_out == NULL) return WEFT_VISION_EINVAL;
    if (!eng->streaming) return WEFT_VISION_ESTATE;

    int idx = -1;
    uint64_t ts = 0, frame_no = 0;
    int64_t deadline = timeout_ns < 0
                           ? (vis_now_ns() + 100000000)
                           : (timeout_ns == 0 ? vis_now_ns()
                                              : timeout_ns);
    int rc = eng->ops->dqbuf(eng, &idx, &ts, &frame_no, deadline);
    if (rc != WEFT_VISION_OK) return rc;

    eng->t_dq_ns = (uint64_t)vis_now_ns();

    /* find a free descriptor slot (round-robin from idx; fixed pool) */
    weft_vision_frame_t *fr = NULL;
    for (uint32_t k = 0; k < eng->buffer_count; k++) {
        uint32_t s = (uint32_t)((idx + k) % (int)eng->buffer_count);
        uint32_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(
                &eng->frame_in_use[s], &expected, 1u, memory_order_acq_rel,
                memory_order_relaxed)) {
            fr = &eng->frames[s];
            break;
        }
    }
    if (fr == NULL) {
        /* all descriptors held: requeue rather than corrupt — honest */
        (void)eng->ops->qbuf(eng, idx);
        return WEFT_VISION_ESTATE;
    }

    /* --- descriptor activation: the measured < 200 ns handoff --- */
    fr->view = eng->tmpl[idx];
    fr->view.tensor_id = frame_no;
    fr->frame_no = frame_no;
    fr->ts_unix_ns = ts;
    fr->buffer_idx = idx;
    fr->owner = eng;
    fr->in_use = 1u;

    fr->dl.dl_tensor.data = eng->buf_map[idx];
    fr->dl.dl_tensor.ctx.device_type = kDLCPU;
    fr->dl.dl_tensor.ctx.device_id = 0;
    fr->dl.dl_tensor.ndim = (int)fr->view.ndim;
    fr->dl.dl_tensor.dtype.code = (uint8_t)kDLUInt;
    fr->dl.dl_tensor.dtype.bits = 8;
    fr->dl.dl_tensor.dtype.lanes = 1;
    fr->dl.dl_tensor.shape = fr->shape_i64;
    fr->dl.dl_tensor.strides = fr->strides_i64;
    fr->dl.dl_tensor.byte_offset = fr->view.byte_offset;
    fr->dl.manager_ctx = fr;
    fr->dl.deleter = frame_dl_deleter;

    eng->t_act_ns = (uint64_t)vis_now_ns();
    eng->last_handoff_ns = eng->t_act_ns - eng->t_dq_ns;
    eng->handoff_total_ns += eng->last_handoff_ns;
    eng->delivered++;
    eng->last_frame_no = frame_no;

    *frame_out = fr;
    return WEFT_VISION_OK;
}

int weft_vision_dma_release(weft_vision_dma_t *eng,
                            weft_vision_frame_t *frame) {
    if (eng == NULL || frame == NULL) return WEFT_VISION_EINVAL;
    if (!frame->in_use) return WEFT_VISION_OK; /* idempotent (DLPack path) */
    int idx = frame->buffer_idx;
    atomic_store_explicit(&eng->frame_in_use[frame - eng->frames], 0u,
                          memory_order_release);
    frame->in_use = 0u;
    frame->owner = NULL;
    return eng->ops->qbuf(eng, idx);
}

static void frame_dl_deleter(DLManagedTensor *self) {
    if (self == NULL) return;
    weft_vision_frame_t *fr = (weft_vision_frame_t *)self->manager_ctx;
    if (fr == NULL || fr->owner == NULL) return;
    (void)weft_vision_dma_release(fr->owner, fr);
    self->manager_ctx = NULL;
    self->deleter = NULL;
}

DLManagedTensor *weft_vision_frame_dlpack(weft_vision_frame_t *frame) {
    if (frame == NULL || !frame->in_use) return NULL;
    return &frame->dl;
}

// ---------------------------------------------------------------------------
// DMA-BUF export / import
// ---------------------------------------------------------------------------

int weft_vision_dma_export_dmabuf(weft_vision_dma_t *eng,
                                  weft_vision_frame_t *frame, int *fd_out) {
    if (eng == NULL || frame == NULL || fd_out == NULL)
        return WEFT_VISION_EINVAL;
    if (!frame->in_use) return WEFT_VISION_ESTATE;
    return eng->ops->expbuf(eng, frame->buffer_idx, fd_out);
}

int weft_vision_dma_wrap_dmabuf(int fd, uint64_t offset, uint64_t len,
                                uint32_t width, uint32_t height,
                                weft_vision_pixfmt_t fmt,
                                weft_tensor_view_t *view_out,
                                void **mapping_out, uint64_t *map_len_out) {
    if (fd < 0 || width == 0 || height == 0 || view_out == NULL ||
        mapping_out == NULL || map_len_out == NULL) {
        return WEFT_VISION_EINVAL;
    }
    size_t need = (size_t)width * height * weft_vision_pixfmt_bpp(fmt);
    if (need == 0 || len < need) return WEFT_VISION_EINVAL;

    void *p = mmap(NULL, (size_t)len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   (off_t)offset);
    if (p == MAP_FAILED) return WEFT_VISION_EDMABUF;

    uint64_t shape[WEFT_TENSOR_MAX_DIMS] = {0, 0, 0, 0, 0, 0, 0, 0};
    shape[0] = height;
    shape[1] = width;
    shape[2] = weft_vision_pixfmt_bpp(fmt);
    int r = weft_tensor_view_init(view_out, 0, WEFT_DTYPE_U8, 3, shape,
                                  (uintptr_t)p, 0);
    if (r != WEFT_TENSOR_OK) {
        (void)munmap(p, (size_t)len);
        return WEFT_VISION_EINVAL;
    }
    *mapping_out = p;
    *map_len_out = len;
    return WEFT_VISION_OK;
}

// ---------------------------------------------------------------------------
// Counters / teardown
// ---------------------------------------------------------------------------

uint64_t weft_vision_dma_last_handoff_ns(const weft_vision_dma_t *eng) {
    return eng == NULL ? 0 : eng->last_handoff_ns;
}

uint64_t weft_vision_dma_frames_delivered(const weft_vision_dma_t *eng) {
    return eng == NULL ? 0 : eng->delivered;
}

uint64_t weft_vision_dma_frames_dropped(const weft_vision_dma_t *eng) {
    if (eng == NULL) return 0;
    uint64_t w = atomic_load_explicit(&eng->mock.writer_dropped,
                                      memory_order_acquire);
    return eng->dropped + w;
}

uint64_t weft_vision_dma_handoff_total_ns(const weft_vision_dma_t *eng) {
    return eng == NULL ? 0 : eng->handoff_total_ns;
}

int weft_vision_dma_stream_stop(weft_vision_dma_t *eng) {
    if (eng == NULL) return WEFT_VISION_EINVAL;
    if (!eng->streaming) return WEFT_VISION_ESTATE;
    int rc = eng->ops->stream_off(eng);
    eng->streaming = 0;
    return rc;
}

int weft_vision_dma_close(weft_vision_dma_t *eng) {
    if (eng == NULL) return WEFT_VISION_EINVAL;
    if (eng->streaming) {
        (void)eng->ops->stream_off(eng);
        eng->streaming = 0;
    }
    (void)eng->ops->close(eng);
    free(eng);
    return WEFT_VISION_OK;
}
