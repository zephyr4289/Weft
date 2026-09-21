// v4l2_backend.c — the REAL Linux V4L2 backend (V4L2_MEMORY_MMAP capture
// + VIDIOC_EXPBUF dma-buf export), mirroring the UAPI exactly.
//
// WHY EXISTS: production machines feed weft-vision-dma from /dev/video*.
// This backend speaks the kernel's own ioctl surface with the official
// struct layouts (struct v4l2_format / v4l2_requestbuffers / v4l2_buffer /
// v4l2_exportbuffer) — Rule 1 fidelity comes from including the kernel's
// UAPI header, never from re-declaring it. When the kernel headers are
// absent (WEFT_HAVE_V4L2=0) or no device node opens, every call fails
// closed with WEFT_VISION_ENODEV — the mock is never silently substituted.
//
// Honesty boundary: this path is COMPILE-verified (kernel UAPI headers,
// -Werror -pedantic) and runtime-verified to fail honestly on machines
// with no camera (the sandbox); full capture silicon legs run on the
// integration fleet's video nodes. The mock device carries CI.

#include "weft_vision_dma.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if WEFT_HAVE_V4L2

/* Kernel UAPI headers occasionally trip -pedantic; they are external
 * standard ABIs (Rule 1) — silence the pedantic pass for the include,
 * never edit or re-declare them. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <linux/videodev2.h>
#pragma GCC diagnostic pop

static uint32_t v4l2_pix_code(weft_vision_pixfmt_t f) {
    switch (f) {
        case WEFT_VISION_PIXFMT_RGBX8888: return V4L2_PIX_FMT_XBGR32;
        case WEFT_VISION_PIXFMT_YUYV: return V4L2_PIX_FMT_YUYV;
        case WEFT_VISION_PIXFMT_GRAY8: return V4L2_PIX_FMT_GREY;
        default: return 0;
    }
}

static int v4l2_xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

static int v4l2_open(weft_vision_dma_t *eng) {
    int fd = open(eng->cfg.path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return WEFT_VISION_ENODEV;

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (v4l2_xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
        !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        close(fd);
        return WEFT_VISION_ENODEV;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = eng->cfg.width;
    fmt.fmt.pix.height = eng->cfg.height;
    fmt.fmt.pix.pixelformat = v4l2_pix_code(eng->cfg.pixfmt);
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (fmt.fmt.pix.pixelformat == 0 ||
        v4l2_xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        close(fd);
        return WEFT_VISION_ENODEV;
    }
    eng->cfg.width = fmt.fmt.pix.width;
    eng->cfg.height = fmt.fmt.pix.height;
    eng->frame_bytes = fmt.fmt.pix.sizeimage;

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = eng->buffer_count;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (v4l2_xioctl(fd, VIDIOC_REQBUFS, &rb) < 0 ||
        rb.count < 2) {
        close(fd);
        return WEFT_VISION_ENODEV;
    }
    eng->buffer_count = rb.count < WEFT_VISION_MAX_BUFFERS
                            ? rb.count
                            : WEFT_VISION_MAX_BUFFERS;

    for (uint32_t i = 0; i < eng->buffer_count; i++) {
        struct v4l2_buffer qb;
        memset(&qb, 0, sizeof(qb));
        qb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        qb.memory = V4L2_MEMORY_MMAP;
        qb.index = i;
        if (v4l2_xioctl(fd, VIDIOC_QUERYBUF, &qb) < 0) {
            for (uint32_t k = 0; k < i; k++) {
                (void)munmap(eng->buf_map[k], eng->buf_len[k]);
                eng->buf_map[k] = NULL;
            }
            close(fd);
            return WEFT_VISION_ENODEV;
        }
        eng->buf_len[i] = qb.length;
        eng->buf_map[i] =
            mmap(NULL, qb.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 qb.m.offset);
        if (eng->buf_map[i] == MAP_FAILED) {
            eng->buf_map[i] = NULL;
            for (uint32_t k = 0; k < i; k++) {
                (void)munmap(eng->buf_map[k], eng->buf_len[k]);
                eng->buf_map[k] = NULL;
            }
            close(fd);
            return WEFT_VISION_ENODEV;
        }
        eng->buf_fd[i] = -1;
    }
    eng->v4l2_fd = fd;
    return WEFT_VISION_OK;
}

static int v4l2_stream_on(weft_vision_dma_t *eng) {
    for (uint32_t i = 0; i < eng->buffer_count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (v4l2_xioctl(eng->v4l2_fd, VIDIOC_QBUF, &b) < 0)
            return WEFT_VISION_EIO;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (v4l2_xioctl(eng->v4l2_fd, VIDIOC_STREAMON, &type) < 0)
        return WEFT_VISION_EIO;
    return WEFT_VISION_OK;
}

static int64_t v4l2_now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

static int v4l2_dqbuf(weft_vision_dma_t *eng, int *idx, uint64_t *ts_ns,
                      uint64_t *frame_no, int64_t deadline_ns) {
    for (;;) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (v4l2_xioctl(eng->v4l2_fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) {
                if (v4l2_now_ns() >= deadline_ns) return WEFT_VISION_ETIMEOUT;
                struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
                (void)nanosleep(&ts, NULL);
                continue;
            }
            return WEFT_VISION_EIO;
        }
        *idx = (int)b.index;
        *ts_ns = (uint64_t)b.timestamp.tv_sec * 1000000000ull +
                 (uint64_t)b.timestamp.tv_usec * 1000ull;
        *frame_no = b.sequence;
        return WEFT_VISION_OK;
    }
}

static int v4l2_qbuf(weft_vision_dma_t *eng, int idx) {
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = (uint32_t)idx;
    if (v4l2_xioctl(eng->v4l2_fd, VIDIOC_QBUF, &b) < 0)
        return WEFT_VISION_EIO;
    return WEFT_VISION_OK;
}

static int v4l2_expbuf(weft_vision_dma_t *eng, int idx, int *fd_out) {
    struct v4l2_exportbuffer exp;
    memset(&exp, 0, sizeof(exp));
    exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    exp.index = (uint32_t)idx;
    exp.plane = 0;
    exp.flags = O_CLOEXEC;
    if (v4l2_xioctl(eng->v4l2_fd, VIDIOC_EXPBUF, &exp) < 0)
        return WEFT_VISION_EDMABUF;
    *fd_out = exp.fd;
    return WEFT_VISION_OK;
}

static int v4l2_stream_off(weft_vision_dma_t *eng) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (v4l2_xioctl(eng->v4l2_fd, VIDIOC_STREAMOFF, &type) < 0)
        return WEFT_VISION_EIO;
    return WEFT_VISION_OK;
}

static int v4l2_close(weft_vision_dma_t *eng) {
    if (eng->v4l2_fd >= 0) {
        for (uint32_t i = 0; i < eng->buffer_count; i++) {
            if (eng->buf_map[i] != NULL) {
                (void)munmap(eng->buf_map[i], eng->buf_len[i]);
                eng->buf_map[i] = NULL;
            }
            if (eng->buf_fd[i] >= 0) {
                (void)close(eng->buf_fd[i]);
                eng->buf_fd[i] = -1;
            }
        }
        (void)close(eng->v4l2_fd);
        eng->v4l2_fd = -1;
    }
    return WEFT_VISION_OK;
}

const weft_vision_backend_ops_t *weft_vision_v4l2_backend(void) {
    static const weft_vision_backend_ops_t ops = {
        .name = "v4l2",
        .open = v4l2_open,
        .stream_on = v4l2_stream_on,
        .dqbuf = v4l2_dqbuf,
        .qbuf = v4l2_qbuf,
        .expbuf = v4l2_expbuf,
        .stream_off = v4l2_stream_off,
        .close = v4l2_close,
    };
    return &ops;
}

#else /* !WEFT_HAVE_V4L2: honest ENODEV stubs, compile-verified */

static int v4l2_stub_nodev(weft_vision_dma_t *eng) {
    (void)eng;
    return WEFT_VISION_ENODEV;
}
static int v4l2_stub_nodev_idx(weft_vision_dma_t *eng, int idx) {
    (void)eng;
    (void)idx;
    return WEFT_VISION_ENODEV;
}
static int v4l2_stub_nodev_dq(weft_vision_dma_t *eng, int *idx,
                              uint64_t *ts_ns, uint64_t *frame_no,
                              int64_t deadline_ns) {
    (void)eng;
    (void)idx;
    (void)ts_ns;
    (void)frame_no;
    (void)deadline_ns;
    return WEFT_VISION_ENODEV;
}
static int v4l2_stub_nodev_exp(weft_vision_dma_t *eng, int idx, int *fd_out) {
    (void)eng;
    (void)idx;
    (void)fd_out;
    return WEFT_VISION_ENODEV;
}

const weft_vision_backend_ops_t *weft_vision_v4l2_backend(void) {
    static const weft_vision_backend_ops_t ops = {
        .name = "v4l2(stub:no-kernel-headers)",
        .open = v4l2_stub_nodev,
        .stream_on = v4l2_stub_nodev,
        .dqbuf = v4l2_stub_nodev_dq,
        .qbuf = v4l2_stub_nodev_idx,
        .expbuf = v4l2_stub_nodev_exp,
        .stream_off = v4l2_stub_nodev,
        .close = v4l2_stub_nodev,
    };
    return &ops;
}

#endif  /* WEFT_HAVE_V4L2 */
