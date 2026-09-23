// weft_uring_driver.c — RFC-0019 §5 (see weft_uring_driver.h for the
// contract). Raw syscalls against the stable io_uring uapi — the
// uring_rx house style, extended from depth-1 ingestion to a batched
// BIDIRECTIONAL transport with registered buffers and the ZC ladder.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "weft_uring_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/io_uring.h>

// uapi fallbacks (kernel-header vintages; 5.10 headers lack these)
#ifndef IORING_OP_SEND_ZC
#define IORING_OP_SEND_ZC 38
#endif
#ifndef IORING_CQE_F_NOTIFY
#define IORING_CQE_F_NOTIFY (1u << 3)
#endif

#define WEFT_URING_MAX_REG 1024u  // IORING_MAX_REGISTERED_BUFFERS

// ---------------------------------------------------------------------------
// probe
// ---------------------------------------------------------------------------

static weft_uring_cap_t g_cap = WEFT_URING_CAP_NONE;
static int g_cap_probed = 0;
static char g_cap_detail[192];

weft_uring_cap_t weft_uring_probe(char* detail, size_t detail_len) {
    if (!g_cap_probed) {
        g_cap_probed = 1;
        g_cap = WEFT_URING_CAP_NONE;
        g_cap_detail[0] = '\0';
        struct io_uring_params p;
        memset(&p, 0, sizeof(p));
        int fd = (int)syscall(__NR_io_uring_setup, 16, &p);
        if (fd < 0) {
            snprintf(g_cap_detail, sizeof(g_cap_detail),
                     "uring: io_uring_setup refused: %s (ENOSYS = kernel "
                     "< 5.1; EPERM = seccomp/sysctl — Law 4 refusal)",
                     strerror(errno));
        } else {
            // prove the full plumbing with a NOP (the uring_rx probe)
            const uint32_t entries = 16;
            const size_t sq_len = (size_t)p.sq_off.array +
                                  entries * sizeof(unsigned);
            const size_t cq_len = (size_t)p.cq_off.cqes +
                                  entries * sizeof(struct io_uring_cqe);
            void* sq = mmap(NULL, sq_len, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
            void* cq = mmap(NULL, cq_len, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
            void* sqes = mmap(NULL, entries * sizeof(struct io_uring_sqe),
                              PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                              fd, IORING_OFF_SQES);
            if (sq != MAP_FAILED && cq != MAP_FAILED && sqes != MAP_FAILED) {
                uint32_t* tail = (uint32_t*)((char*)sq + p.sq_off.tail);
                uint32_t* mask = (uint32_t*)((char*)sq + p.sq_off.ring_mask);
                unsigned* array = (unsigned*)((char*)sq + p.sq_off.array);
                struct io_uring_sqe* sqe = (struct io_uring_sqe*)sqes;
                const unsigned idx = *tail & *mask;
                memset(&sqe[idx], 0, sizeof(*sqe));
                sqe[idx].opcode = IORING_OP_NOP;
                sqe[idx].user_data = 0x57524654;  // "WRFT"
                array[idx] = idx;
                __atomic_store_n(tail, *tail + 1, __ATOMIC_RELEASE);
                long rc = syscall(__NR_io_uring_enter, fd, 1, 1,
                                  IORING_ENTER_GETEVENTS, NULL);
                if (rc == 1) {
                    g_cap = WEFT_URING_CAP_RING;
                    // registration rung (5.10: register works; the FIXED
                    // recv itself needs >= 5.19 in uring_rx — but
                    // READ_FIXED/WRITE_FIXED opcodes are 5.1 vintage)
                    static char probe_buf[4096] __attribute__((aligned(4096)));
                    struct iovec iov = { probe_buf, sizeof(probe_buf) };
                    long rrc = syscall(__NR_io_uring_register, fd,
                                       IORING_REGISTER_BUFFERS, &iov, 1);
                    if (rrc == 0) g_cap = WEFT_URING_CAP_FIXED;
                    // ZC (>= 5.19) is probed live on the first TX CQE —
                    // a uname gate would be a guess; the CQE is the truth
                    snprintf(g_cap_detail, sizeof(g_cap_detail),
                             "uring: rings LIVE (register=%s)",
                             rrc == 0 ? "ok" : strerror(errno));
                }
            } else {
                snprintf(g_cap_detail, sizeof(g_cap_detail),
                         "uring: ring mmap refused: %s", strerror(errno));
            }
            if (sq != MAP_FAILED) munmap(sq, sq_len);
            if (cq != MAP_FAILED) munmap(cq, cq_len);
            if (sqes != MAP_FAILED) munmap(sqes, entries *
                                           sizeof(struct io_uring_sqe));
            close(fd);
        }
    }
    if (detail && detail_len) {
        snprintf(detail, detail_len, "%.160s (rung=%d)", g_cap_detail,
                 (int)g_cap);
    }
    return g_cap;
}

size_t weft_uring_report(char* buf, size_t buflen) {
    char d[192];
    weft_uring_probe(d, sizeof(d));
    int n = snprintf(buf, buflen, "%s\n", d);
    return n > 0 ? (size_t)n : 0;
}

weft_uring_config_t weft_uring_config_default(void) {
    weft_uring_config_t c = {
        .sq_entries = 128,
        .poll_timeout_ns = 50ull * 1000 * 1000,
        .want_zc = 1,
        .want_fixed = 1,
    };
    return c;
}

// ---------------------------------------------------------------------------
// ring internals (the uring_rx layout, verbatim discipline)
// ---------------------------------------------------------------------------

static int uring_map(weft_uring_ctx_t* ctx) {
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    const int fd = (int)syscall(__NR_io_uring_setup, ctx->cfg.sq_entries,
                                &p);
    if (fd < 0) {
        ctx->stats.last_errno = errno;
        snprintf(ctx->err, sizeof(ctx->err),
                 "uring: setup(%u) refused: %s", ctx->cfg.sq_entries,
                 strerror(errno));
        return -1;
    }
    const uint32_t entries = p.sq_entries;
    ctx->ring_fd = fd;
    ctx->ring_entries = entries;
    ctx->sq_map_len = (size_t)p.sq_off.array + entries * sizeof(unsigned);
    ctx->cq_map_len = (size_t)p.cq_off.cqes + entries * sizeof(struct io_uring_cqe);
    ctx->sqes_map_len = (size_t)entries * sizeof(struct io_uring_sqe);

    ctx->sq_map = mmap(NULL, ctx->sq_map_len, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    ctx->cq_map = mmap(NULL, ctx->cq_map_len, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    ctx->sqes_map = mmap(NULL, ctx->sqes_map_len, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (ctx->sq_map == MAP_FAILED || ctx->cq_map == MAP_FAILED ||
        ctx->sqes_map == MAP_FAILED) {
        ctx->stats.last_errno = errno;
        snprintf(ctx->err, sizeof(ctx->err), "uring: mmap refused: %s",
                 strerror(errno));
        return -1;
    }
    ctx->sq_head = (uint32_t*)((char*)ctx->sq_map + p.sq_off.head);
    ctx->sq_tail = (uint32_t*)((char*)ctx->sq_map + p.sq_off.tail);
    ctx->sq_array = (uint32_t*)((char*)ctx->sq_map + p.sq_off.array);
    ctx->sq_mask = (uint32_t*)((char*)ctx->sq_map + p.sq_off.ring_mask);
    ctx->cq_head = (uint32_t*)((char*)ctx->cq_map + p.cq_off.head);
    ctx->cq_tail = (uint32_t*)((char*)ctx->cq_map + p.cq_off.tail);
    ctx->cq_mask = (uint32_t*)((char*)ctx->cq_map + p.cq_off.ring_mask);
    ctx->cqes = (char*)ctx->cq_map + p.cq_off.cqes;
    ctx->sq_cached_head = *ctx->sq_head;
    return 0;
}

weft_cluster_status_t weft_uring_driver_init(const weft_uring_config_t* cfg,
                                             weft_uring_ctx_t* ctx) {
    if (!cfg || !ctx) return WEFT_CLUSTER_E_INVALID_ARG;
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *cfg;
    ctx->ring_fd = -1;
    ctx->udp_fd = -1;
    if (ctx->cfg.sq_entries == 0 ||
        (ctx->cfg.sq_entries & (ctx->cfg.sq_entries - 1))) {
        snprintf(ctx->err, sizeof(ctx->err),
                 "uring: sq_entries must be a power of two");
        return WEFT_CLUSTER_E_INVALID_ARG;
    }

    const weft_uring_cap_t cap = weft_uring_probe(ctx->err, sizeof(ctx->err));
    if (cap < WEFT_URING_CAP_RING) {
        return WEFT_CLUSTER_E_SYS;
    }
    if (uring_map(ctx) != 0) {
        weft_uring_driver_shutdown(ctx);
        return WEFT_CLUSTER_E_IO;
    }
    ctx->cap = WEFT_URING_CAP_RING;
    // optimistic modes; first CQE feedback downgrades stickily
    ctx->tx_mode = ctx->cfg.want_zc ? WEFT_URING_TX_ZC : WEFT_URING_TX_FIXED;
    if (!ctx->cfg.want_fixed) ctx->tx_mode = WEFT_URING_TX_PLAIN;
    ctx->rx_mode = ctx->cfg.want_fixed ? WEFT_URING_RX_FIXED
                                       : WEFT_URING_RX_PLAIN;
    ctx->err[0] = '\0';
    return WEFT_CLUSTER_OK;
}

weft_cluster_status_t weft_uring_bind_socket(weft_uring_ctx_t* ctx,
                                             uint16_t* io_port) {
    if (!ctx || ctx->ring_fd < 0) return WEFT_CLUSTER_E_STATE;
    if (ctx->udp_fd >= 0) return WEFT_CLUSTER_E_STATE;
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ctx->stats.last_errno = errno;
        snprintf(ctx->err, sizeof(ctx->err), "uring: UDP socket: %s",
                 strerror(errno));
        return WEFT_CLUSTER_E_IO;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // bench road; cluster
                                                  // peers set the real ip
    sa.sin_port = htons(io_port && *io_port ? *io_port : 0);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        ctx->stats.last_errno = errno;
        snprintf(ctx->err, sizeof(ctx->err), "uring: bind: %s",
                 strerror(errno));
        close(fd);
        return WEFT_CLUSTER_E_IO;
    }
    // non-blocking: EAGAIN is a counted, bounded event (Law 2)
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    socklen_t sl = sizeof(sa);
    getsockname(fd, (struct sockaddr*)&sa, &sl);
    if (io_port) *io_port = ntohs(sa.sin_port);
    ctx->udp_fd = fd;
    return WEFT_CLUSTER_OK;
}

weft_cluster_status_t weft_uring_connect_peer(weft_uring_ctx_t* ctx,
                                              const char* ipv4,
                                              uint16_t port) {
    if (!ctx || !ipv4 || ctx->udp_fd < 0) return WEFT_CLUSTER_E_STATE;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ipv4, &sa.sin_addr) != 1) {
        snprintf(ctx->err, sizeof(ctx->err), "uring: bad peer '%s'", ipv4);
        return WEFT_CLUSTER_E_INVALID_ARG;
    }
    if (connect(ctx->udp_fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        ctx->stats.last_errno = errno;
        snprintf(ctx->err, sizeof(ctx->err), "uring: connect: %s",
                 strerror(errno));
        return WEFT_CLUSTER_E_IO;
    }
    return WEFT_CLUSTER_OK;
}

weft_cluster_status_t weft_uring_register_region(weft_uring_ctx_t* ctx,
                                                 const weft_wcr1_region_t* r) {
    if (!ctx || !r) return WEFT_CLUSTER_E_INVALID_ARG;
    if (ctx->ring_fd < 0) return WEFT_CLUSTER_E_STATE;
    char why[128];
    if (weft_wcr1_validate(r, why, sizeof(why)) != WEFT_WCR1_OK) {
        snprintf(ctx->err, sizeof(ctx->err), "uring: %s", why);
        return WEFT_CLUSTER_E_INVALID_ARG;
    }
    if (r->chunk_count > WEFT_URING_MAX_REG) {
        snprintf(ctx->err, sizeof(ctx->err),
                 "uring: %u chunks > IORING_MAX_REGISTERED_BUFFERS (%u) — "
                 "register a sub-region (named refusal)",
                 r->chunk_count, WEFT_URING_MAX_REG);
        return WEFT_CLUSTER_E_UNSUPPORTED;
    }

    // per-chunk iovecs (setup alloc — Law 1 exempts)
    ctx->reg_iov = (struct iovec*)malloc(sizeof(struct iovec) *
                                         r->chunk_count);
    if (!ctx->reg_iov) return WEFT_CLUSTER_E_NO_MEMORY;
    for (uint32_t k = 0; k < r->chunk_count; k++) {
        ctx->reg_iov[k].iov_base = weft_wcr1_chunk(r, k);
        ctx->reg_iov[k].iov_len = r->chunk_size;
    }

    long rc = syscall(__NR_io_uring_register, ctx->ring_fd,
                      IORING_REGISTER_BUFFERS, ctx->reg_iov,
                      (unsigned)r->chunk_count);
    if (rc != 0) {
        const int e = errno;
        ctx->stats.last_errno = e;
        free(ctx->reg_iov);
        ctx->reg_iov = NULL;
        if (e == ENOMEM || e == EPERM || e == EINVAL) {
            // RLIMIT_MEMLOCK floor — the honest [FALLBACK-COPY] rung
            snprintf(ctx->err, sizeof(ctx->err),
                     "uring: REGISTER_BUFFERS(%u x %u B) refused: %s "
                     "— degrading to plain TX/RX [FALLBACK-COPY] "
                     "(RLIMIT_MEMLOCK; see RFC-0019 §5.5)",
                     r->chunk_count, r->chunk_size, strerror(e));
            ctx->stats.downgrades++;
            ctx->tx_mode = WEFT_URING_TX_PLAIN;
            ctx->rx_mode = WEFT_URING_RX_PLAIN;
            ctx->stats.tx_mode = (int)WEFT_URING_TX_PLAIN;
            ctx->stats.rx_mode = (int)WEFT_URING_RX_PLAIN;
            ctx->stats.registered = 0;
        } else {
            return WEFT_CLUSTER_E_IO;
        }
    } else {
        ctx->stats.registered = 1;
        ctx->reg_count = r->chunk_count;
        ctx->cap = WEFT_URING_CAP_FIXED;
        ctx->stats.tx_mode = (int)ctx->tx_mode;
        ctx->stats.rx_mode = (int)ctx->rx_mode;
    }
    ctx->region = *r;
    ctx->region_ok = 1;
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// SQE plumbing
// ---------------------------------------------------------------------------

static uint32_t sq_free(weft_uring_ctx_t* ctx) {
    const uint32_t tail = *ctx->sq_tail;
    ctx->sq_cached_head = __atomic_load_n(ctx->sq_head, __ATOMIC_ACQUIRE);
    return ctx->ring_entries - (tail - ctx->sq_cached_head);
}

static struct io_uring_sqe* sq_next(weft_uring_ctx_t* ctx) {
    if (sq_free(ctx) == 0) return NULL;
    const uint32_t idx = (*ctx->sq_tail) & (*ctx->sq_mask);
    struct io_uring_sqe* sqe =
        (struct io_uring_sqe*)ctx->sqes_map + idx;
    memset(sqe, 0, sizeof(*sqe));
    ctx->sq_array[idx] = idx;
    __atomic_store_n(ctx->sq_tail, *ctx->sq_tail + 1, __ATOMIC_RELEASE);
    ctx->in_flight++;
    return sqe;
}

/// Fill one TX SQE per the negotiated mode (shared by send_frame and
/// the ZC-retry road — one filler, no drift).
static weft_cluster_status_t submit_tx(weft_uring_ctx_t* ctx,
                                       uint32_t chunk, uint32_t off,
                                       uint32_t len, uint64_t user_data) {
    struct io_uring_sqe* sqe = sq_next(ctx);
    if (!sqe) {
        const weft_cluster_status_t st = weft_uring_flush(ctx);
        if (st != WEFT_CLUSTER_OK) return st;
        sqe = sq_next(ctx);
        if (!sqe) return WEFT_CLUSTER_E_BUSY;
    }

    uint8_t* buf = weft_wcr1_chunk(&ctx->region, chunk) + off;
    switch (ctx->tx_mode) {
        case WEFT_URING_TX_ZC:
            sqe->opcode = IORING_OP_SEND_ZC;
            sqe->fd = ctx->udp_fd;
            sqe->addr = (uint64_t)(uintptr_t)buf;
            sqe->len = len;
            sqe->buf_index = chunk;
            sqe->ioprio |= (1 << 4);  // IORING_RECVSEND_FIXED_BUF
            break;
        case WEFT_URING_TX_FIXED:
            sqe->opcode = IORING_OP_WRITE_FIXED;
            sqe->fd = ctx->udp_fd;
            sqe->addr = (uint64_t)(uintptr_t)buf;
            sqe->len = len;
            sqe->buf_index = chunk;
            break;
        default:
            sqe->opcode = IORING_OP_SEND;
            sqe->fd = ctx->udp_fd;
            sqe->addr = (uint64_t)(uintptr_t)buf;
            sqe->len = len;
            sqe->msg_flags = MSG_DONTWAIT;
            break;
    }
    sqe->user_data = user_data;
    return WEFT_CLUSTER_OK;
}

weft_cluster_status_t weft_uring_send_frame(weft_uring_ctx_t* ctx,
                                            uint32_t chunk, uint32_t off,
                                            uint32_t len, uint64_t user_data) {
    if (!ctx || ctx->udp_fd < 0 || !ctx->region_ok) {
        if (ctx) snprintf(ctx->err, sizeof(ctx->err),
                          "uring: send before socket+region");
        return WEFT_CLUSTER_E_STATE;
    }
    if (chunk >= ctx->region.chunk_count ||
        (uint64_t)off + len > ctx->region.chunk_size || len == 0) {
        snprintf(ctx->err, sizeof(ctx->err),
                 "uring: send geometry refused (chunk %u off %u len %u)",
                 chunk, off, len);
        return WEFT_CLUSTER_E_INVALID_ARG;
    }
    // ZC attempts are shadowed: if the kernel refuses the opcode (the
    // < 5.19 verdict arrives as a CQE, not an enter error), poll_events
    // downgrades STICKILY and resubmits THIS frame on the fallback road —
    // a capability discovery may cost a syscall, never a frame.
    if (ctx->tx_mode == WEFT_URING_TX_ZC) {
        ctx->zc_retry.pending = 1;
        ctx->zc_retry.chunk = chunk;
        ctx->zc_retry.off = off;
        ctx->zc_retry.len = len;
        ctx->zc_retry.user_data = user_data;
    }
    return submit_tx(ctx, chunk, off, len, user_data);
}

weft_cluster_status_t weft_uring_recv_arm(weft_uring_ctx_t* ctx,
                                          uint32_t chunk,
                                          uint64_t user_data) {
    if (!ctx || ctx->udp_fd < 0 || !ctx->region_ok) {
        if (ctx) snprintf(ctx->err, sizeof(ctx->err),
                          "uring: recv before socket+region");
        return WEFT_CLUSTER_E_STATE;
    }
    if (chunk >= ctx->region.chunk_count) return WEFT_CLUSTER_E_INVALID_ARG;
    if (user_data & (1ull << 62)) {
        snprintf(ctx->err, sizeof(ctx->err),
                 "uring: rx user_data bit 62 is the driver's kind tag");
        return WEFT_CLUSTER_E_INVALID_ARG;
    }

    struct io_uring_sqe* sqe = sq_next(ctx);
    if (!sqe) {
        const weft_cluster_status_t st = weft_uring_flush(ctx);
        if (st != WEFT_CLUSTER_OK) return st;
        sqe = sq_next(ctx);
        if (!sqe) return WEFT_CLUSTER_E_BUSY;
    }

    if (ctx->rx_mode == WEFT_URING_RX_FIXED) {
        sqe->opcode = IORING_OP_READ_FIXED;
        sqe->buf_index = chunk;
    } else {
        sqe->opcode = IORING_OP_READ;
    }
    sqe->fd = ctx->udp_fd;
    sqe->addr = (uint64_t)(uintptr_t)weft_wcr1_chunk(&ctx->region, chunk);
    sqe->len = ctx->region.chunk_size;
    sqe->user_data = user_data | (1ull << 62);  // kind tag: RX
    return WEFT_CLUSTER_OK;
}

weft_cluster_status_t weft_uring_flush(weft_uring_ctx_t* ctx) {
    if (!ctx || ctx->ring_fd < 0) return WEFT_CLUSTER_E_STATE;
    const uint32_t pending = *ctx->sq_tail - __atomic_load_n(
        ctx->sq_head, __ATOMIC_ACQUIRE);
    if (pending == 0) return WEFT_CLUSTER_OK;
    // submit only (min_complete = 0): NEVER a blocking enter on kernels
    // without EXT_ARG timeouts — Law 2's no-hang law for 5.10
    long rc = syscall(__NR_io_uring_enter, ctx->ring_fd, pending, 0, 0,
                      NULL);
    ctx->stats.enters++;
    ctx->stats.submits += pending;
    if (rc < 0) {
        ctx->stats.last_errno = errno;
        snprintf(ctx->err, sizeof(ctx->err), "uring: enter(submit): %s",
                 strerror(errno));
        return WEFT_CLUSTER_E_IO;
    }
    return WEFT_CLUSTER_OK;
}

weft_cluster_status_t weft_uring_poll_events(weft_uring_ctx_t* ctx,
                                             uint64_t timeout_ns,
                                             uint32_t min_events,
                                             weft_uring_ev_t* out,
                                             uint32_t cap, uint32_t* out_n) {
    if (!ctx || !out || !out_n) return WEFT_CLUSTER_E_INVALID_ARG;
    *out_n = 0;
    if (ctx->ring_fd < 0) return WEFT_CLUSTER_E_STATE;
    if (min_events > cap) min_events = cap;

    const uint64_t deadline = weft_deadline_after(timeout_ns);
    for (;;) {
        // harvest the CQ (kernel-produced; acquire on tail)
        uint32_t head = __atomic_load_n(ctx->cq_head, __ATOMIC_RELAXED);
        const uint32_t tail = __atomic_load_n(ctx->cq_tail,
                                              __ATOMIC_ACQUIRE);
        while (head != tail && *out_n < cap) {
            const struct io_uring_cqe* cqe =
                (const struct io_uring_cqe*)((char*)ctx->cqes +
                    (head & (*ctx->cq_mask)) * sizeof(struct io_uring_cqe));
            head++;
            ctx->stats.completions++;
            if (ctx->in_flight) ctx->in_flight--;

            weft_uring_ev_t* ev = &out[(*out_n)++];
            ev->kind = 0;
            if (cqe->user_data & (1ull << 62)) {
                // RX encoding (recv_arm tags bit 62)
                ev->kind = 2;
                ev->user_data = cqe->user_data & ~(1ull << 62);
            } else if (cqe->flags & IORING_CQE_F_NOTIFY) {
                ev->kind = 1;  // ZC TX notification
                ev->user_data = cqe->user_data;
                ctx->stats.tx_notifs++;
            } else {
                ev->user_data = cqe->user_data;
            }
            ev->res = cqe->res;

            // sticky downgrades on unsupported ops (Law 4, labeled).
            // A refused ZC costs its opcode attempt, never its frame:
            // the shadowed send is resubmitted on the fallback road.
            if (cqe->res == -EINVAL || cqe->res == -EOPNOTSUPP) {
                if (ctx->tx_mode == WEFT_URING_TX_ZC) {
                    ctx->tx_mode = WEFT_URING_TX_FIXED;
                    ctx->stats.tx_mode = (int)WEFT_URING_TX_FIXED;
                    ctx->stats.downgrades++;
                    snprintf(ctx->err, sizeof(ctx->err),
                             "uring: SEND_ZC refused (-EINVAL) — kernel "
                             "< 5.19; downgraded to WRITE_FIXED "
                             "[FALLBACK-COPY] (sticky; frame resubmitted)");
                } else if (ctx->rx_mode == WEFT_URING_RX_FIXED) {
                    ctx->rx_mode = WEFT_URING_RX_PLAIN;
                    ctx->stats.downgrades++;
                }
            }
            if (cqe->res == -EAGAIN) ctx->stats.eagain++;
            if (ev->kind == 2 && cqe->res > 0) ctx->stats.rx_frames++;
            if (ev->kind != 2 && cqe->res > 0) ctx->stats.tx_bytes += cqe->res;
        }
        __atomic_store_n(ctx->cq_head, head, __ATOMIC_RELEASE);

        // ZC retry: the shadowed frame goes out on the downgraded road
        // (allocation-free; the shadow is driver state, not a queue)
        if (ctx->zc_retry.pending && ctx->tx_mode != WEFT_URING_TX_ZC) {
            ctx->zc_retry.pending = 0;
            submit_tx(ctx, ctx->zc_retry.chunk, ctx->zc_retry.off,
                      ctx->zc_retry.len, ctx->zc_retry.user_data);
            weft_uring_flush(ctx);
            continue;   // re-harvest (the retry's CQE may be immediate)
        }

        if (*out_n >= min_events) return WEFT_CLUSTER_OK;
        if (weft_deadline_remaining(deadline) == 0) {
            ctx->stats.timeouts++;
            return (*out_n > 0) ? WEFT_CLUSTER_OK
                                : WEFT_CLUSTER_E_TIMEOUT;
        }
        weft_poll_yield();
    }
}

void weft_uring_driver_shutdown(weft_uring_ctx_t* ctx) {
    if (!ctx) return;
    if (ctx->udp_fd >= 0) close(ctx->udp_fd);
    if (ctx->sq_map) munmap(ctx->sq_map, ctx->sq_map_len);
    if (ctx->cq_map) munmap(ctx->cq_map, ctx->cq_map_len);
    if (ctx->sqes_map) munmap(ctx->sqes_map, ctx->sqes_map_len);
    if (ctx->ring_fd >= 0) close(ctx->ring_fd);
    free(ctx->reg_iov);
    memset(ctx, 0, sizeof(*ctx));
    ctx->ring_fd = -1;
    ctx->udp_fd = -1;
}

const weft_uring_stats_t* weft_uring_stats(const weft_uring_ctx_t* ctx) {
    return ctx ? &ctx->stats : NULL;
}

const char* weft_uring_last_error(const weft_uring_ctx_t* ctx) {
    return ctx ? ctx->err : "null ctx";
}
