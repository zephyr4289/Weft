// weft_hw_v4l2.h — RFC-0016 §6: the Linux camera seam (V4L2 -> DMA-BUF ->
// Weft ring, zero CPU touches on the frame bytes).
//
// WHY EXISTS: the mandate's camera pipeline — "camera/sensor inputs ->
// Weft ring -> surface compositor with zero CPU memory touches." On Linux
// the zero-copy currency between a capture device and everything else is
// the dma-buf fd: V4L2 MMAP buffers export via VIDIOC_EXPBUF, and the
// exported fd binds into a Weft ring session (weft_dmabuf) or a GPU
// import (weft_gpu_wrap_dmabuf) without a single CPU copy of the frame.
//
// THE SEAM, SHAPED HONESTLY: this module captures ONE frame per call
// under the standard V4L2 stream protocol (querycap/reqbufs(1, MMAP)/
// mmap/EXPBUF/STREAMON/DQBUF) and hands back the exported dma-buf fd.
// It is deliberately NOT a camera daemon — pacing, format negotiation
// beyond the device's default, and multi-buffer pipelines are the
// application's policy (the same mechanism-not-policy line the render
// bridge draws). What it removes is the PLUMBING between "the frame the
// sensor produced" and "the fd every device in the mesh can consume."
//
// CAPABILITY LADDER (Law 4):
//   no /dev/video* — every entry refuses with ENODEV (the CI/sandbox
//   state, asserted by the V-series gates); the module compiles and
//   self-describes everywhere
//   device present — probe reports driver/card/bus; capture runs where
//   the device streams (DECLARED for the sandbox: no capture hardware)
//
// LAW 3: driver layer; weft.c/weft.h untouched. LAW 2: the capture path
// performs ioctls (setup) but ZERO frame copies — the frame pages are
// the dma-buf's; the CPU mmaps them only to hand the ring a session
// view (the compositor consumes the fd, not CPU-touched bytes).

#ifndef WEFT_HW_V4L2_H
#define WEFT_HW_V4L2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEFT_V4L2_UNSUPPORTED = 0,  ///< non-Linux, or no /dev/video* device
    WEFT_V4L2_DEVICE = 1,       ///< a capture device opened + queried
} weft_v4l2_caps_t;

typedef struct {
    int probed;
    weft_v4l2_caps_t caps;
    char device[32];            ///< the first working /dev/videoN ("" none)
    char driver[32];            ///< V4L2 driver name (diagnostics, AXIOM T)
    char card[64];              ///< human device name
    uint32_t width, height, pixfmt;  ///< negotiated capture format
} weft_v4l2_probe_t;

/// Run (or join) the one-shot probe: scans /dev/video0..15 for a
/// VIDEO_CAPTURE-capable device, opens + queries it. Advisory (AXIOM T).
const weft_v4l2_probe_t* weft_v4l2_probe(void);

/// One capability line for evidence logs.
size_t weft_v4l2_report(char* buf, size_t buflen);

/// A captured frame: the exported dma-buf fd (caller owns — close(2))
/// plus the buffer's geometry. The frame NEVER passes through CPU bytes.
typedef struct {
    int fd;                 ///< exported dma-buf (VIDIOC_EXPBUF) — owned
    uint32_t width, height;
    uint32_t bytesused;     ///< the driver's byte count
    uint32_t pixfmt;        ///< V4L2 fourcc
} weft_v4l2_frame_t;

/// Capture ONE frame and export it as a dma-buf fd. `device` selects the
/// capture node ("" or NULL = the probed default). Returns 0; -1 with
/// errno on every refusal (no device, no MMAP support, EXPBUF refused —
/// some virtual nodes cannot export). The caller composes the fd with
/// weft_dmabuf_import_fd (raw camera bytes) or binds the ring session
/// storage and hands the fd to a GPU import.
int weft_v4l2_capture_frame(const char* device, weft_v4l2_frame_t* out);

#ifdef __cplusplus
}
#endif

#endif // WEFT_HW_V4L2_H
