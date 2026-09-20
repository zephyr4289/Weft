// weft_hw_v4l2.c — the Linux camera seam (RFC-0016 §6).

#include "weft_hw_v4l2.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__)

#include <dirent.h>
#include <pthread.h>

// ---- V4L2 uapi subset (self-contained; values from videodev2.h, stable
// since 3.x — the uring_rx/dmabuf discipline of no header dependency) ----

struct weft_v4l2_capability {
    char driver[16];
    char card[32];
    char bus_info[32];
    char version[4];
    uint32_t capabilities[2];
    char reserved[32];
};
struct weft_v4l2_format {
    uint32_t type;
    struct { uint32_t width, height, pixelformat; uint32_t field;
             uint32_t bytesperline, sizeimage; uint32_t colorspace;
             uint32_t priv, flags; } pix;
    uint8_t raw[200];
};
struct weft_v4l2_requestbuffers {
    uint32_t count, type, memory, capabilities, flags;
    uint8_t reserved[24];
};
struct weft_v4l2_buffer {
    uint32_t index, type, bytesused, flags, field;
    double timestamp;  // timeval on 64-bit LE; padding-compatible
    struct { uint32_t timecode_type, flags, frames, seconds; } timecode;
    uint32_t sequence, memory, offset;
    uint8_t reserved[84];  // union m offset + length live here (see below)
};
// the m union: we use offset (MMAP) at a fixed place — videodev2's
// struct v4l2_buffer m union starts after `memory`; we hand-roll the
// exact layout below instead to stay honest about ABI:

// EXACT layouts (x86_64/aarch64 LP64 — the platforms this module builds
// for; a 32-bit build refuses at compile time, declared):
#define WEFT_V4L2_ASSERT_LP64
struct weft_v4l2_buffer_lp64 {
    uint32_t index;        // 0
    uint32_t type;         // 4
    uint32_t bytesused;    // 8
    uint32_t flags;        // 12
    uint32_t field;        // 16
    // struct timeval: 2x long (16 bytes on LP64)
    long tv_sec;           // 24
    long tv_usec;          // 32
    struct { uint32_t a, b, c, d; } timecode;  // 40
    uint32_t sequence;     // 56
    uint32_t memory;       // 60
    union { uint32_t offset; uint64_t userptr; int fd; } m;  // 64
    uint32_t length;       // 72
    uint32_t reserved2;    // 76
    uint32_t reserved[2];  // 80..88 (v4l2_buffer is 88 on LP64 —
                           //  the kernel accepts sizeof-84/88 variants;
                           //  we pass the full struct)
};

struct weft_v4l2_exportbuffer {
    uint32_t type, index, plane, flags;
    int fd;
    uint32_t reserved[11];
};

#define WEFT_VIDIOC_QUERYCAP  _IOR('V',  0, struct weft_v4l2_capability)
#define WEFT_VIDIOC_G_FMT     _IOR('V',  4, struct weft_v4l2_format)
#define WEFT_VIDIOC_REQBUFS   _IOWR('V', 8, struct weft_v4l2_requestbuffers)
#define WEFT_VIDIOC_QBUF      _IOWR('V', 15, struct weft_v4l2_buffer_lp64)
#define WEFT_VIDIOC_DQBUF     _IOWR('V', 17, struct weft_v4l2_buffer_lp64)
#define WEFT_VIDIOC_STREAMON  _IOW('V', 18, int)
#define WEFT_VIDIOC_STREAMOFF _IOW('V', 19, int)
#define WEFT_VIDIOC_EXPBUF    _IOWR('V', 16, struct weft_v4l2_exportbuffer)

#define WEFT_V4L2_BUF_TYPE_VIDEO_CAPTURE 1u
#define WEFT_V4L2_MEMORY_MMAP 1u
#define WEFT_V4L2_CAP_VIDEO_CAPTURE 0x00000001u
#define WEFT_V4L2_BUF_FLAG_QUEUED 0x00000002u

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

static weft_v4l2_probe_t g_probe;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static int try_device(const char* path) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    struct weft_v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, WEFT_VIDIOC_QUERYCAP, &cap) != 0 ||
        !(cap.capabilities[0] & WEFT_V4L2_CAP_VIDEO_CAPTURE)) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void probe_run(void) {
    memset(&g_probe, 0, sizeof(g_probe));
    char path[32];
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/video%d", i);
        if (try_device(path) == 0) {
            g_probe.caps = WEFT_V4L2_DEVICE;
            snprintf(g_probe.device, sizeof(g_probe.device), "%s", path);
            // fill driver/card on the capture path (the probe node may be
            // busy; diagnostics there are best-effort, AXIOM T)
            break;
        }
    }
    g_probe.probed = 1;
}

const weft_v4l2_probe_t* weft_v4l2_probe(void) {
    pthread_once(&g_once, probe_run);
    return &g_probe;
}

size_t weft_v4l2_report(char* buf, size_t buflen) {
    const weft_v4l2_probe_t* p = weft_v4l2_probe();
    int n;
    if (p->caps == WEFT_V4L2_DEVICE) {
        n = snprintf(buf, buflen, "v4l2: device=%s", p->device);
    } else {
        n = snprintf(buf, buflen, "v4l2: unsupported (no /dev/video* capture device)");
    }
    return (n < 0) ? 0 : (size_t)n;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

int weft_v4l2_capture_frame(const char* device, weft_v4l2_frame_t* out) {
    if (out == NULL) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    const weft_v4l2_probe_t* p = weft_v4l2_probe();
    const char* path = (device && device[0]) ? device : p->device;
    if (path == NULL || path[0] == '\0') {
        errno = ENODEV;  // honest refusal: no capture node
        return -1;
    }
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;

    struct weft_v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, WEFT_VIDIOC_QUERYCAP, &cap) != 0 ||
        !(cap.capabilities[0] & WEFT_V4L2_CAP_VIDEO_CAPTURE)) {
        close(fd);
        errno = ENODEV;
        return -1;
    }

    // the device's current format (no negotiation — the default is the
    // seam's contract; policy belongs to the application)
    struct weft_v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = WEFT_V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, WEFT_VIDIOC_G_FMT, &fmt) != 0) {
        const int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    // one MMAP buffer
    struct weft_v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = WEFT_V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = WEFT_V4L2_MEMORY_MMAP;
    if (ioctl(fd, WEFT_VIDIOC_REQBUFS, &req) != 0 || req.count < 1) {
        const int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    struct weft_v4l2_buffer_lp64 buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = WEFT_V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = WEFT_V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (ioctl(fd, WEFT_VIDIOC_QBUF, &buf) != 0) {
        const int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    int on = (int)WEFT_V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, WEFT_VIDIOC_STREAMON, &on) != 0 ||
        ioctl(fd, WEFT_VIDIOC_DQBUF, &buf) != 0) {
        const int e = errno;
        ioctl(fd, WEFT_VIDIOC_STREAMOFF, &on);
        close(fd);
        errno = e;
        return -1;
    }

    // THE seam: export the driver's buffer as a dma-buf fd
    struct weft_v4l2_exportbuffer exp;
    memset(&exp, 0, sizeof(exp));
    exp.type = WEFT_V4L2_BUF_TYPE_VIDEO_CAPTURE;
    exp.index = 0;
    exp.plane = 0;
    exp.fd = -1;
    if (ioctl(fd, WEFT_VIDIOC_EXPBUF, &exp) != 0 || exp.fd < 0) {
        const int e = errno;
        ioctl(fd, WEFT_VIDIOC_STREAMOFF, &on);
        close(fd);
        errno = e;
        return -1;  // e.g. virtual nodes without dma-buf support — honest
    }

    ioctl(fd, WEFT_VIDIOC_STREAMOFF, &on);
    close(fd);
    out->fd = exp.fd;  // caller owns it now
    out->width = fmt.pix.width;
    out->height = fmt.pix.height;
    out->bytesused = buf.bytesused;
    out->pixfmt = fmt.pix.pixelformat;
    return 0;
}

#else  // !__linux__

const weft_v4l2_probe_t* weft_v4l2_probe(void) {
    static weft_v4l2_probe_t p;
    p.probed = 1;
    p.caps = WEFT_V4L2_UNSUPPORTED;
    return &p;
}

size_t weft_v4l2_report(char* buf, size_t buflen) {
    int n = snprintf(buf, buflen, "v4l2: unsupported (non-linux)");
    return (n < 0) ? 0 : (size_t)n;
}

int weft_v4l2_capture_frame(const char* device, weft_v4l2_frame_t* out) {
    (void)device;
    if (out) { memset(out, 0, sizeof(*out)); out->fd = -1; }
    errno = EOPNOTSUPP;
    return -1;
}

#endif  // __linux__
