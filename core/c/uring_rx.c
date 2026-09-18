// uring_rx.c — RFC 0012 kernel-bypass ingestion (see uring_rx.h for the
// contract). No external io_uring library: the five good reasons to hand-
// roll the ring plumbing are (1) zero new link-time deps for a repo that
// builds with plain gcc, (2) the layout is a stable UAPI (linux/io_uring.h),
// (3) depth-1 needs ~40 lines of SQ/CQ code, (4) every syscall return is
// checked and REPORTED (Law 4) rather than wrapped by a library's errno
// laundering, and (5) the capability ladder must degrade on kernels where
// pieces are missing — a library would happily paper over the refusal.

#define _GNU_SOURCE
#include "uring_rx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/utsname.h>

#if WEFT_URING_RX_LINUX
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/io_uring.h>
#endif

// MSG_TRUNC on recv(2): returns the FULL datagram length even when the
// buffer was too small — the exact-truncation accounting both modes use.

// Kernel-floor gate: registered fixed recv (IORING_RECV_FIXED_BUFFER) and
// multishot landed in 5.19; FIXED mode is only negotiated at/above that.
#define WEFT_URING_FIXED_MAJOR 5
#define WEFT_URING_FIXED_MINOR 19

// ---------------------------------------------------------------------------
// Capability ladder
// ---------------------------------------------------------------------------

#if WEFT_URING_RX_LINUX

static int g_probe_done = 0;
static weft_uring_mode_t g_probe_mode = WEFT_URING_MODE_NONE;
static int g_register_errno = 0;

static int kernel_at_least(int major, int minor) {
    struct utsname u;
    if (uname(&u) != 0) return 0;
    int a = 0, b = 0;
    if (sscanf(u.release, "%d.%d", &a, &b) != 2) return 0;
    if (a != major) return a > major;
    return b >= minor;
}

weft_uring_mode_t weft_uring_probe(void) {
#if defined(WEFT_URING_FORCE_SYSCALL)
    // Bench-only A/B leg (house precedent: WEFT_FANOUT_SEQ_CST): force the
    // universal recv() rung so the RING rung's delta is measurable where the
    // ladder negotiates it. Never set in production builds.
    return WEFT_URING_MODE_SYSCALL;
#else
    if (g_probe_done) return g_probe_mode;

    weft_uring_mode_t mode = WEFT_URING_MODE_SYSCALL;  // recv() is POSIX

    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    int fd = (int)syscall(__NR_io_uring_setup, 16, &p);
    if (fd < 0) {
        // Blocked (seccomp EPERM/EBADF) or absent (ENOSYS): the ladder stops
        // at SYSCALL. The errno is recorded for the report line.
        g_register_errno = errno;
        g_probe_mode = mode;
        g_probe_done = 1;
        return mode;
    }

    // Map the rings and prove the full plumbing with a NOP roundtrip —
    // "setup returned a fd" is NOT proof the rings work under this seccomp.
    const uint32_t entries = 16;
    const size_t sq_len = (size_t)p.sq_off.array + entries * sizeof(unsigned);
    const size_t cq_len = (size_t)p.cq_off.cqes + entries * sizeof(struct io_uring_cqe);
    void* sq = mmap(NULL, sq_len, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    void* cq = mmap(NULL, cq_len, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    void* sqes = mmap(NULL, entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);

    if (sq != MAP_FAILED && cq != MAP_FAILED && sqes != MAP_FAILED) {
        uint32_t* tail = (uint32_t*)((char*)sq + p.sq_off.tail);
        uint32_t* mask = (uint32_t*)((char*)sq + p.sq_off.ring_mask);
        unsigned* array = (unsigned*)((char*)sq + p.sq_off.array);
        struct io_uring_sqe* sqe = (struct io_uring_sqe*)sqes;
        unsigned idx = *tail & *mask;
        memset(&sqe[idx], 0, sizeof(*sqe));
        sqe[idx].opcode = IORING_OP_NOP;
        sqe[idx].user_data = 0xFEED;
        array[idx] = idx;
        __atomic_store_n(tail, *tail + 1, __ATOMIC_RELEASE);
        long rc = syscall(__NR_io_uring_enter, fd, 1, 1, IORING_ENTER_GETEVENTS, NULL);
        if (rc == 1) {
            mode = WEFT_URING_MODE_RING;
            // Registration rung: if register works AND the kernel is >= 5.19
            // (fixed recv usable), FIXED is negotiable. Both facts needed.
            static char probe_buf[4096] __attribute__((aligned(4096)));
            struct iovec iov = { .iov_base = probe_buf, .iov_len = sizeof(probe_buf) };
            long rrc = syscall(__NR_io_uring_register, fd,
                               IORING_REGISTER_BUFFERS, &iov, 1);
            g_register_errno = (rrc == 0) ? 0 : errno;
            if (rrc == 0 && kernel_at_least(WEFT_URING_FIXED_MAJOR, WEFT_URING_FIXED_MINOR)) {
                mode = WEFT_URING_MODE_FIXED;
            }
        }
    }

    if (sq != MAP_FAILED) munmap(sq, sq_len);
    if (cq != MAP_FAILED) munmap(cq, cq_len);
    if (sqes != MAP_FAILED) munmap(sqes, entries * sizeof(struct io_uring_sqe));
    close(fd);

    g_probe_mode = mode;
    g_probe_done = 1;
    return mode;
#endif // WEFT_URING_FORCE_SYSCALL
}

#else  // !WEFT_URING_RX_LINUX

weft_uring_mode_t weft_uring_probe(void) { return WEFT_URING_MODE_SYSCALL; }

#endif

const char* weft_uring_mode_str(weft_uring_mode_t m) {
    switch (m) {
        case WEFT_URING_MODE_NONE:    return "none";
        case WEFT_URING_MODE_SYSCALL: return "syscall";
        case WEFT_URING_MODE_RING:    return "ring";
        case WEFT_URING_MODE_FIXED:   return "fixed";
    }
    return "?";
}

size_t weft_uring_report(char* buf, size_t buflen) {
    const weft_uring_mode_t m = weft_uring_probe();
#if WEFT_URING_RX_LINUX
    struct utsname u;
    uname(&u);
    int n = snprintf(buf, buflen,
                     "uring-capability: mode=%s register_errno=%d kernel=%s "
                     "(fixed-recv rung needs >= 5.19; multishot+sqpoll: PREDICTED, hardware-gated)\n",
                     weft_uring_mode_str(m), g_register_errno, u.release);
#else
    int n = snprintf(buf, buflen, "uring-capability: mode=%s (non-linux)\n",
                     weft_uring_mode_str(m));
#endif
    return n > 0 ? (size_t)n : 0;
}

// ---------------------------------------------------------------------------
// Session internals (Linux / RING+ modes)
// ---------------------------------------------------------------------------

#if WEFT_URING_RX_LINUX

static int uring_setup(weft_uring_rx_t* rx) {
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    int fd = (int)syscall(__NR_io_uring_setup, 16, &p);
    if (fd < 0) { rx->stats.last_errno = errno; return -1; }

    const uint32_t entries = 16;
    rx->ring_entries = entries;
    rx->sq_map_len = (size_t)p.sq_off.array + entries * sizeof(unsigned);
    rx->cq_map_len = (size_t)p.cq_off.cqes + entries * sizeof(struct io_uring_cqe);
    rx->sqes_map_len = entries * sizeof(struct io_uring_sqe);

    rx->sq_map = mmap(NULL, rx->sq_map_len, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    rx->cq_map = mmap(NULL, rx->cq_map_len, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    rx->sqes_map = mmap(NULL, rx->sqes_map_len, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (rx->sq_map == MAP_FAILED || rx->cq_map == MAP_FAILED || rx->sqes_map == MAP_FAILED) {
        rx->stats.last_errno = errno;
        return -1;
    }

    rx->sq_head = (uint32_t*)((char*)rx->sq_map + p.sq_off.head);
    rx->sq_tail = (uint32_t*)((char*)rx->sq_map + p.sq_off.tail);
    rx->sq_array = (uint32_t*)((char*)rx->sq_map + p.sq_off.array);
    rx->sq_mask = (uint32_t*)((char*)rx->sq_map + p.sq_off.ring_mask);
    rx->cq_head = (uint32_t*)((char*)rx->cq_map + p.cq_off.head);
    rx->cq_tail = (uint32_t*)((char*)rx->cq_map + p.cq_off.tail);
    rx->cq_mask = (uint32_t*)((char*)rx->cq_map + p.cq_off.ring_mask);
    rx->cqes = (char*)rx->cq_map + p.cq_off.cqes;
    rx->ring_fd = fd;
    return 0;
}

/// Submit one RECV SQE targeting `cursor` and wait for its CQE. Returns the
/// recv result (bytes / full dgram length with MSG_TRUNC), or -errno.
static int uring_recv_once(weft_uring_rx_t* rx, void* cursor, size_t len) {
    struct io_uring_sqe* sqes = (struct io_uring_sqe*)rx->sqes_map;
    const unsigned idx = (*rx->sq_tail) & (*rx->sq_mask);

    memset(&sqes[idx], 0, sizeof(*sqes));
    sqes[idx].opcode = IORING_OP_RECV;
    sqes[idx].fd = rx->fd;
    sqes[idx].addr = (uint64_t)(uintptr_t)cursor;
    sqes[idx].len = (uint32_t)len;
    sqes[idx].msg_flags = MSG_TRUNC;  // res = FULL dgram length, Linux-wide
    sqes[idx].user_data = 0xFEED;
    rx->sq_array[idx] = idx;
    __atomic_store_n(rx->sq_tail, *rx->sq_tail + 1, __ATOMIC_RELEASE);

    long rc = syscall(__NR_io_uring_enter, rx->ring_fd, 1, 1,
                      IORING_ENTER_GETEVENTS, NULL);
    rx->stats.enters++;
    if (rc < 0) return -(errno ? errno : EIO);
    if (rc == 0) return -EAGAIN;

    // Depth-1: harvest exactly one CQE (tail must be past head).
    const uint32_t head = *rx->cq_head;
    if (*rx->cq_tail == head) return -EAGAIN;  // nothing to harvest
    const struct io_uring_cqe* cq =
        (const struct io_uring_cqe*)((char*)rx->cqes +
            (head & (*rx->cq_mask)) * sizeof(struct io_uring_cqe));
    const int32_t res = cq->res;
    // Advance the CQ head (release: kernel must not overwrite before our read).
    __atomic_store_n(rx->cq_head, head + 1, __ATOMIC_RELEASE);
    return (int)res;
}

static void uring_teardown(weft_uring_rx_t* rx) {
    if (rx->sq_map) { munmap(rx->sq_map, rx->sq_map_len); rx->sq_map = NULL; }
    if (rx->cq_map) { munmap(rx->cq_map, rx->cq_map_len); rx->cq_map = NULL; }
    if (rx->sqes_map) { munmap(rx->sqes_map, rx->sqes_map_len); rx->sqes_map = NULL; }
    if (rx->ring_fd >= 0) { close(rx->ring_fd); rx->ring_fd = -1; }
}

#endif // WEFT_URING_RX_LINUX

// ---------------------------------------------------------------------------
// Session API
// ---------------------------------------------------------------------------

int weft_uring_attach(weft_uring_rx_t* rx, weft_fanout_t* f, int fd) {
    if (!rx || !f || fd < 0) return -1;
    memset(rx, 0, sizeof(*rx));
    rx->fan = f;
    rx->fd = fd;
    rx->ring_fd = -1;

    // Datagram-only contract: FIONREAD counts DATAGRAMS on dgram sockets;
    // on a stream it counts bytes and would corrupt the frame mapping.
    int so_type = 0;
    socklen_t tl = sizeof(so_type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &so_type, &tl) != 0) {
        rx->stats.last_errno = errno;
        return -1;
    }
    if (so_type != SOCK_DGRAM && so_type != SOCK_SEQPACKET) {
        rx->stats.last_errno = EINVAL;
        return -1;
    }

    // Non-blocking for the duration (restored at detach): the FIONREAD gate
    // guarantees queued data, so recv never blocks; a spurious EAGAIN is a
    // counted abort, never a hang.
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0 && (fl & O_NONBLOCK) == 0) {
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        rx->fd_flags_saved = 1;
    }

    const weft_uring_mode_t probed = weft_uring_probe();
    rx->mode = WEFT_URING_MODE_SYSCALL;

#if WEFT_URING_RX_LINUX
    if (probed >= WEFT_URING_MODE_RING) {
        if (uring_setup(rx) == 0) {
            rx->mode = WEFT_URING_MODE_RING;
            // FIXED: register the M slot payload regions once — page pinning
            // moves off the per-recv path. (RECV_FIXED itself needs >= 5.19;
            // on older kernels registration succeeds but fixed recv would
            // not, so FIXED is only negotiated when the version gate passes.)
            if (probed == WEFT_URING_MODE_FIXED) {
                const size_t pb = f->payload_bytes;
                struct iovec iovs[WEFT_FANOUT_MAX_SLOTS];
                const size_t base = 16 + 8 * (size_t)f->slot_count;
                for (unsigned k = 0; k < f->slot_count; k++) {
                    iovs[k].iov_base = f->ring + base + (size_t)k * pb;
                    iovs[k].iov_len = pb;
                }
                long rc = syscall(__NR_io_uring_register, rx->ring_fd,
                                  IORING_REGISTER_BUFFERS, iovs, (unsigned)f->slot_count);
                if (rc == 0) {
                    rx->mode = WEFT_URING_MODE_FIXED;
                } else {
                    rx->stats.last_errno = errno;  // recorded; RING still sound
                }
            }
        } else {
            uring_teardown(rx);  // probe said RING, live attach refused: degrade
        }
    }
#else
    (void)probed;
#endif

    return 0;
}

uint64_t weft_uring_next(weft_uring_rx_t* rx) {
    if (!rx->fan) return 0;

    // 1. Availability gate — no queued bytes: no frame, no seq burn.
    int queued = 0;
    rx->stats.gates++;
    if (ioctl(rx->fd, FIONREAD, &queued) != 0) {
        rx->stats.last_errno = errno;
        return 0;
    }
    if (queued <= 0) return 0;

    // 2. Bracket OPENS (frozen begin: invalidate + SeqCst fence).
    uint8_t* cursor = weft_fanout_begin(rx->fan);
    if (!cursor) return 0;
    const size_t pb = rx->fan->payload_bytes;

    // 3. The kernel fills the slot (this is the zero-copy line).
    int res;
#if WEFT_URING_RX_LINUX
    if (rx->mode >= WEFT_URING_MODE_RING) {
        res = uring_recv_once(rx, cursor, pb);
    } else
#endif
    {
        res = (int)recv(rx->fd, cursor, pb, MSG_TRUNC);
        rx->stats.syscalls++;
    }

    if (res < 0) {
        // recv failure with the bracket open: publish nothing; the begun seq
        // is burned (counted — on the next successful claim the reader's
        // telescoping accounts it as a drop; the identity stays exact).
        rx->stats.aborted++;
        rx->stats.last_errno = (res < -1) ? (-res) : errno;
        return 0;
    }

    // 4. Size honesty: short datagrams zero the tail (no cross-frame byte
    //    leakage through a fixed-geometry slot); oversized datagrams keep
    //    the head (MSG_TRUNC made res the FULL length — truncated counted).
    if ((size_t)res > pb) {
        rx->stats.truncated++;
        res = (int)pb;
    } else if ((size_t)res < pb) {
        memset(cursor + res, 0, pb - (size_t)res);
        rx->stats.padded++;
    }
    rx->stats.bytes += (uint64_t)res;

    // 5. Bracket CLOSES (frozen publish: stamp + latestSeq, Release).
    const uint64_t seq = weft_fanout_publish(rx->fan);
    if (seq != 0) rx->stats.frames++;
    return seq;
}

void weft_uring_detach(weft_uring_rx_t* rx) {
    if (!rx) return;
#if WEFT_URING_RX_LINUX
    uring_teardown(rx);
#endif
    if (rx->fd >= 0 && rx->fd_flags_saved) {
        int fl = fcntl(rx->fd, F_GETFL, 0);
        if (fl >= 0) fcntl(rx->fd, F_SETFL, fl & ~O_NONBLOCK);
        rx->fd_flags_saved = 0;
    }
    memset(rx, 0, sizeof(*rx));
}

void weft_uring_stats(const weft_uring_rx_t* rx, weft_uring_stats_t* out) {
    if (!rx || !out) return;
    *out = rx->stats;
}
