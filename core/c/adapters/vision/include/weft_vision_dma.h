// weft_vision_dma.h — zero-copy camera frame ingestion engine
// (Linux V4L2 MMAP + DMA-BUF -> weft_tensor_view_t + DLPack).
//
// WHY EXISTS: the classic edge pipeline copies a captured frame 4-5 times
// (kernel DMA buffer -> uvcvideo user copy -> framework staging -> tensor
// -> accelerator upload). This engine hands back, per dequeued buffer, a
// FROZEN weft_tensor_view_t (core/c/tensor/weft_tensor.h, 176-byte ABI,
// static-assert-pinned) and a DLPack DLManagedTensor whose data pointer IS
// the mmap'd V4L2/DMA-BUF mapping — pointer identity preserved from kernel
// dequeue to tensor descriptor activation (Rule 4), with a measured
// handoff of tens of nanoseconds (directive SLA: < 200 ns).
//
// BACKEND SEAM: one ops table mirrors the V4L2 UAPI surface
// (open/reqbufs-mmap/stream-on/dqbuf/qbuf/expbuf/stream-off/close) so the
// SAME engine drives the real ioctl backend and the synthetic mock device
// (tests/adapters/mock/v4l2_mock_device.c — memfd DMA arena, emulated
// sensor writer thread). AUTO = V4L2 only; a machine with no camera
// fails closed with an honest error — the mock is always EXPLICIT.
//
// LAWS:
//   Law 1  zero heap allocation while streaming — every frame descriptor,
//          shape/stride array, and buffer mapping is preallocated at
//          stream_start; dequeue/release are index swaps + one template
//          copy. The native battery interposes the allocator to prove it.
//   Law 2  zero-copy — frame->view.physical_or_shm_addr equals the buffer
//          mapping (asserted in tests); the DLPack deleter REQUEUES the
//          V4L2 buffer instead of freeing anything.
//   Law 3  deterministic degradation — dequeue takes an explicit
//          timeout_ns; timeouts return WEFT_VISION_ETIMEOUT, never block.

#ifndef WEFT_VISION_DMA_H_
#define WEFT_VISION_DMA_H_

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "weft_dlpack.h"
#include "weft_tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Status registry (house style: negative codes, no errno laundering)
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_VISION_OK = 0,
    WEFT_VISION_EINVAL = -1,
    WEFT_VISION_ENODEV = -2,   /* device open/negotiation failed */
    WEFT_VISION_ENOMEM = -3,
    WEFT_VISION_ETIMEOUT = -4,
    WEFT_VISION_ESTATE = -5,   /* wrong streaming state */
    WEFT_VISION_EIO = -6,      /* backend dqbuf/qbuf/expbuf failure */
    WEFT_VISION_EMAGIC = -7,   /* mock frame stamp mismatch (integrity) */
    WEFT_VISION_EDMABUF = -8,  /* dma-buf import/export failed */
} weft_vision_status_t;

const char *weft_vision_status_name(int st);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

typedef enum {
    WEFT_VISION_BACKEND_AUTO = 0,   /* V4L2 (real device required) */
    WEFT_VISION_BACKEND_V4L2 = 1,
    WEFT_VISION_BACKEND_MOCK = 2,   /* synthetic camera (tests) */
} weft_vision_backend_t;

typedef enum {
    WEFT_VISION_PIXFMT_RGBX8888 = 0, /* 4 bytes/px, sensor-style RGBA-ish */
    WEFT_VISION_PIXFMT_YUYV = 1,     /* 2 bytes/px packed YUV 4:2:2 */
    WEFT_VISION_PIXFMT_GRAY8 = 2,    /* 1 byte/px */
} weft_vision_pixfmt_t;

uint32_t weft_vision_pixfmt_bpp(weft_vision_pixfmt_t f);

#define WEFT_VISION_MAX_BUFFERS 8

typedef struct weft_vision_dma_config {
    weft_vision_backend_t backend;
    char path[64];              /* V4L2 device node (backend V4L2) */
    uint32_t width;
    uint32_t height;
    weft_vision_pixfmt_t pixfmt;
    double fps;
    uint32_t buffer_count;      /* 2..WEFT_VISION_MAX_BUFFERS */
    /* mock-only knobs (honest synthetic model, documented in D-62 §B.4) */
    uint32_t mock_dma_latency_ns;  /* emulated sensor->DONE delay      */
    uint32_t mock_swath_div;       /* 0=auto, 1=full write, N=1/N swath */
} weft_vision_dma_config_t;

/// Sensible defaults (1920x1080 RGBX @ 30 fps, 4 buffers).
void weft_vision_dma_config_default(weft_vision_dma_config_t *cfg);

// ---------------------------------------------------------------------------
// The frame descriptor (preallocated pool — never heap'd on the hot path)
// ---------------------------------------------------------------------------

typedef struct weft_vision_dma weft_vision_dma_t;

typedef struct weft_vision_frame {
    weft_tensor_view_t view;      /* the FROZEN core ABI (176 B)          */
    DLManagedTensor dl;           /* DLPack v1.0 handle over the SAME mm */
    int64_t shape_i64[WEFT_TENSOR_MAX_DIMS];
    int64_t strides_i64[WEFT_TENSOR_MAX_DIMS];
    uint64_t frame_no;            /* sensor frame counter (monotonic)     */
    uint64_t ts_unix_ns;          /* DONE timestamp                       */
    int buffer_idx;               /* owning DMA buffer slot               */
    weft_vision_dma_t *owner;
    uint32_t in_use;              /* descriptor slot liveness             */
} weft_vision_frame_t;

_Static_assert(sizeof(weft_vision_frame_t) <= 512,
               "frame descriptor must stay compact (cache-friendly pool)");

// ---------------------------------------------------------------------------
// Engine API
// ---------------------------------------------------------------------------

/// Open + negotiate + map buffers (cold; allocates the engine once).
int weft_vision_dma_open(const weft_vision_dma_config_t *cfg,
                         weft_vision_dma_t **out);

/// Begin streaming (buffers queued to the backend; descriptor templates
/// precomputed; mock sensor writer thread starts here).
int weft_vision_dma_stream_start(weft_vision_dma_t *eng);

/// Block until a frame is DONE (bounded by timeout_ns), then ACTIVATE its
/// descriptors: *frame_out carries the zero-copy view + DLPack handle.
/// The engine measures the handoff (kernel-dequeue -> descriptor-ready)
/// and exposes it via weft_vision_dma_last_handoff_ns.
int weft_vision_dma_dequeue(weft_vision_dma_t *eng,
                            weft_vision_frame_t **frame_out,
                            int64_t timeout_ns);

/// Release a frame: requeues the DMA buffer, frees the descriptor slot.
/// Idempotent per frame.
int weft_vision_dma_release(weft_vision_dma_t *eng,
                            weft_vision_frame_t *frame);

/// The DLPack entry point for frameworks: returns the frame's
/// DLManagedTensor. The deleter requeues the buffer — PyTorch calling
/// `from_dlpack` and later freeing the tensor IS the release path.
DLManagedTensor *weft_vision_frame_dlpack(weft_vision_frame_t *frame);

/// Export the frame's buffer as a dma-buf fd (VIDIOC_EXPBUF on real V4L2;
/// the mock returns the buffer's memfd — an fd-passable stand-in).
int weft_vision_dma_export_dmabuf(weft_vision_dma_t *eng,
                                  weft_vision_frame_t *frame, int *fd_out);

/// Importer seam: wrap an EXTERNAL dma-buf fd (offset/len) as a frozen
/// tensor view mapped read-write into this process — the zero-copy
/// consumer side of DMA-BUF sharing (test-proven against the mock's
/// exported memfd).
int weft_vision_dma_wrap_dmabuf(int fd, uint64_t offset, uint64_t len,
                                uint32_t width, uint32_t height,
                                weft_vision_pixfmt_t fmt,
                                weft_tensor_view_t *view_out,
                                void **mapping_out, uint64_t *map_len_out);

/// Measured handoff of the last dequeue (kernel buffer dequeue ->
/// tensor descriptor activation). Pure read; SLA gate material.
uint64_t weft_vision_dma_last_handoff_ns(const weft_vision_dma_t *eng);

/// Honest counters.
uint64_t weft_vision_dma_frames_delivered(const weft_vision_dma_t *eng);
uint64_t weft_vision_dma_frames_dropped(const weft_vision_dma_t *eng);
uint64_t weft_vision_dma_handoff_total_ns(const weft_vision_dma_t *eng);

int weft_vision_dma_stream_stop(weft_vision_dma_t *eng);
int weft_vision_dma_close(weft_vision_dma_t *eng);

// ---------------------------------------------------------------------------
// Backend seam (mirrors the V4L2 UAPI surface; mock implements the same)
// ---------------------------------------------------------------------------

typedef struct weft_vision_backend_ops {
    const char *name;
    int (*open)(weft_vision_dma_t *eng);
    int (*stream_on)(weft_vision_dma_t *eng);
    int (*dqbuf)(weft_vision_dma_t *eng, int *idx, uint64_t *ts_ns,
                 uint64_t *frame_no, int64_t deadline_ns);
    int (*qbuf)(weft_vision_dma_t *eng, int idx);
    int (*expbuf)(weft_vision_dma_t *eng, int idx, int *fd_out);
    int (*stream_off)(weft_vision_dma_t *eng);
    int (*close)(weft_vision_dma_t *eng);
} weft_vision_backend_ops_t;

/// The real V4L2 backend (compile-gated by WEFT_HAVE_V4L2; without kernel
/// headers it returns ENODEV honestly).
const weft_vision_backend_ops_t *weft_vision_v4l2_backend(void);

/// The synthetic mock camera (defined in tests/adapters/mock — linked
/// only into test/bench binaries; a WEAK symbol in the production lib,
/// so MOCK requests without the mock linked fail closed with ENODEV).
const weft_vision_backend_ops_t *weft_vision_mock_backend(void);

/// Engine state block — backends reach their private state through here.
struct weft_vision_dma {
    weft_vision_dma_config_t cfg;
    const weft_vision_backend_ops_t *ops;
    uint32_t frame_bytes;
    uint64_t period_ns;
    int streaming;

    /* buffer table (filled by backend open: mapping + length + fd) */
    uint8_t *buf_map[WEFT_VISION_MAX_BUFFERS];
    size_t buf_len[WEFT_VISION_MAX_BUFFERS];
    int buf_fd[WEFT_VISION_MAX_BUFFERS];
    uint32_t buffer_count;
    int v4l2_fd;                /* real backend device node; -1 otherwise */

    /* precomputed per-buffer view templates (zero-copy activation) */
    weft_tensor_view_t tmpl[WEFT_VISION_MAX_BUFFERS];

    /* descriptor pool */
    weft_vision_frame_t frames[WEFT_VISION_MAX_BUFFERS];
    _Atomic uint32_t frame_in_use[WEFT_VISION_MAX_BUFFERS];

    /* instrumentation + counters */
    uint64_t t_dq_ns;       /* clock right after backend dqbuf returned */
    uint64_t t_act_ns;      /* clock when descriptors were activated    */
    uint64_t last_handoff_ns;
    uint64_t handoff_total_ns;
    uint64_t delivered;
    uint64_t dropped;
    uint64_t last_frame_no;

    /* mock backend state (embedded: zero-heap streaming by construction) */
    struct {
        int arena_fds[WEFT_VISION_MAX_BUFFERS]; /* per-buffer memfd      */
        _Atomic uint32_t bstate[WEFT_VISION_MAX_BUFFERS];
        _Atomic uint64_t frame_counter;
        _Atomic uint64_t writer_dropped;
        _Atomic uint32_t doorbell; /* wake word for parked consumers   */
        _Atomic uint32_t waiters;
        _Atomic int writer_stop;
        int writer_started;
        uint32_t next_write;
        uint32_t next_read;
        pthread_t writer;
        uint64_t period_ns;
        uint32_t dma_latency_ns;
        uint32_t swath_div;
        uint32_t width, height;
        uint32_t bpp;
        uint8_t *pattern_tmpl; /* cold-built bulk fill template        */
    } mock;
};

#ifdef __cplusplus
}
#endif

#endif  // WEFT_VISION_DMA_H_
