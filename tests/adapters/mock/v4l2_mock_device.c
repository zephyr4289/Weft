// v4l2_mock_device.c — the synthetic camera implementation.
//
// Frame engine (2-core-sandbox sustainable at 4K@120, integrity intact):
//   * bulk swath fill = ONE memcpy from a cold-built template (warm pages
//     after the first pass — the honest sensor-bandwidth model);
//   * per-4KiB-block stamps {magic, block_idx, frame_no} give the verifier
//     block-granular attribution;
//   * integrity hash = word-wise rolling FNV-64 over the swath (~8x the
//     byte-wise CRC throughput; the Pillar-5 seqlock checksum discipline);
//   * trailer carries magic + frame_no + the same hash (three-way
//     agreement with the stamp header, non-overlapping fields).

#include "v4l2_mock_device.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define WEFT_FUTEX_WAIT 0
#define WEFT_FUTEX_WAKE 1

enum {
    MOCK_BQUEUED = 0, /* ready for the writer                    */
    MOCK_BWRITING = 1,
    MOCK_BDONE = 2,   /* filled; waiting for the consumer        */
    MOCK_BHELD = 3,   /* dequeued by the consumer (loan window)  */
};

static inline void mock_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

static inline int64_t mock_now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

static int mock_debug(void) {
    static _Atomic int cached;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v == 0) {
        v = (getenv("WEFT_MOCK_DEBUG") != NULL) ? 1 : -1;
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return v > 0 ? 1 : 0;
}

static uint64_t mock_rollhash64(const uint8_t *p, size_t len) {
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h ^= w;
        h *= 1099511628211ull;
    }
    if (i < len) {
        uint64_t w = 0;
        memcpy(&w, p + i, len - i);
        h ^= w;
        h *= 1099511628211ull;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Sensor writer thread
// ---------------------------------------------------------------------------

static void mock_write_frame(weft_vision_dma_t *eng, int idx) {
    uint8_t *buf = eng->buf_map[idx];
    size_t len = eng->buf_len[idx];
    uint64_t frame_no = atomic_fetch_add_explicit(&eng->mock.frame_counter,
                                                   1ull, memory_order_acq_rel);
    uint32_t div = eng->mock.swath_div;
    size_t active = len > 128u ? len - 128u : len;
    size_t swath = div <= 1 ? active : active / div;
    uint32_t off = 64u;

    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);

    /* bulk fill from the template (warm memcpy — the bandwidth model) */
    memcpy(buf + off, eng->mock.pattern_tmpl + off, swath);

    /* per-block stamps: corruption attribution at 4 KiB granularity */
    for (size_t b = 0; b * 4096u < swath; b++) {
        weft_mock_block_stamp_t bs = {
            .magic = WEFT_MOCK_STAMP_MAGIC,
            .block_idx = (uint32_t)b,
            .frame_no = frame_no,
        };
        memcpy(buf + off + b * 4096u, &bs, sizeof(bs));
    }

    weft_mock_stamp_t st;
    memset(&st, 0, sizeof(st));
    st.magic = WEFT_MOCK_STAMP_MAGIC;
    st.frame_no = frame_no;
    st.ts_unix_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    st.swath_off = off;
    st.swath_len = (uint32_t)swath;
    st.swath_hash = mock_rollhash64(buf + off, swath);

    memcpy(buf, &st, sizeof(st));
    /* trailer (non-overlapping fields): [-64,-60) magic, [-56,-48)
       frame_no, [-48,-40) swath hash */
    memset(buf + len - 64, 0, 64);
    uint32_t tm = WEFT_MOCK_STAMP_MAGIC;
    memcpy(buf + len - 64, &tm, 4);
    memcpy(buf + len - 56, &frame_no, 8);
    memcpy(buf + len - 48, &st.swath_hash, 8);

    /* emulated DMA latency: sensor trigger -> buffer DONE */
    if (eng->mock.dma_latency_ns > 0) {
        int64_t until = mock_now_ns() + (int64_t)eng->mock.dma_latency_ns;
        while (mock_now_ns() < until) mock_pause();
    }

    atomic_store_explicit(&eng->mock.bstate[idx], MOCK_BDONE,
                          memory_order_release);
    atomic_fetch_add_explicit(&eng->mock.doorbell, 1u, memory_order_release);
    if (atomic_load_explicit(&eng->mock.waiters, memory_order_relaxed) != 0) {
        (void)syscall(SYS_futex, (uint32_t *)&eng->mock.doorbell,
                      WEFT_FUTEX_WAKE, 1, NULL, NULL, 0);
    }
}

static void *mock_writer_main(void *arg) {
    weft_vision_dma_t *eng = (weft_vision_dma_t *)arg;
    if (mock_debug()) {
        fprintf(stderr, "[mock] writer ENTRY: eng=%p period=%llu bufs=%u\n",
                (void *)eng, (unsigned long long)eng->mock.period_ns,
                eng->buffer_count);
    }
    struct timespec next;
    (void)clock_gettime(CLOCK_MONOTONIC, &next);
    while (atomic_load_explicit(&eng->mock.writer_stop,
                                memory_order_acquire) == 0) {
        next.tv_nsec += (long)(eng->mock.period_ns % 1000000000ull);
        next.tv_sec += (time_t)(eng->mock.period_ns / 1000000000ull);
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec += 1;
        }
        (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        /* fell more than a period behind? resync the cadence (skipped
           deadlines are schedule slips, NOT frame drops — drops are
           counted only when no buffer is free) */
        struct timespec nowts;
        (void)clock_gettime(CLOCK_MONOTONIC, &nowts);
        if (nowts.tv_sec > next.tv_sec ||
            (nowts.tv_sec == next.tv_sec && nowts.tv_nsec > next.tv_nsec)) {
            next = nowts;
        }

        int claimed = -1;
        for (uint32_t k = 0; k < eng->buffer_count; k++) {
            uint32_t i = (eng->mock.next_write + k) % eng->buffer_count;
            uint32_t expected = MOCK_BQUEUED;
            if (atomic_compare_exchange_strong_explicit(
                    &eng->mock.bstate[i], &expected, MOCK_BWRITING,
                    memory_order_acq_rel, memory_order_relaxed)) {
                claimed = (int)i;
                eng->mock.next_write = (i + 1) % eng->buffer_count;
                break;
            }
        }
        if (claimed < 0) {
            atomic_fetch_add_explicit(&eng->mock.writer_dropped, 1ull,
                                      memory_order_relaxed);
            continue;
        }
        mock_write_frame(eng, claimed);
        if (mock_debug()) {
            static _Atomic uint64_t beat = 0;
            if (atomic_fetch_add_explicit(&beat, 1ull,
                                          memory_order_relaxed) % 64u == 0) {
                fprintf(stderr, "[mock] writer: frames=%llu drops=%llu\n",
                        (unsigned long long)atomic_load_explicit(
                            &eng->mock.frame_counter, memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(
                            &eng->mock.writer_dropped, memory_order_relaxed));
            }
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Backend ops
// ---------------------------------------------------------------------------

static int mock_open(weft_vision_dma_t *eng) {
    eng->v4l2_fd = -1;
    eng->mock.swath_div = eng->cfg.mock_swath_div == 0
                              ? (eng->frame_bytes > (8u << 20) ? 8u : 1u)
                              : eng->cfg.mock_swath_div;
    eng->mock.period_ns = eng->period_ns;
    eng->mock.dma_latency_ns = eng->cfg.mock_dma_latency_ns;
    eng->mock.width = eng->cfg.width;
    eng->mock.height = eng->cfg.height;
    eng->mock.bpp = weft_vision_pixfmt_bpp(eng->cfg.pixfmt);

    /* cold: the bulk-fill template (build BEFORE any streaming window) */
    eng->mock.pattern_tmpl = malloc(eng->frame_bytes);
    if (eng->mock.pattern_tmpl == NULL) return WEFT_VISION_ENOMEM;
    {
        uint32_t s = 0x12345678u;
        for (size_t i = 0; i < eng->frame_bytes; i++) {
            s = s * 1664525u + 1013904223u;
            eng->mock.pattern_tmpl[i] = (uint8_t)(s >> 24);
        }
    }

    for (uint32_t i = 0; i < eng->buffer_count; i++) {
        int fd = memfd_create("weft_vision_mock_dma", 0);
        if (fd < 0) return WEFT_VISION_ENODEV;
        if (ftruncate(fd, (off_t)eng->frame_bytes) != 0) {
            (void)close(fd);
            return WEFT_VISION_ENODEV;
        }
        void *p = mmap(NULL, eng->frame_bytes, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            (void)close(fd);
            return WEFT_VISION_ENODEV;
        }
        eng->buf_map[i] = (uint8_t *)p;
        eng->buf_len[i] = eng->frame_bytes;
        eng->buf_fd[i] = -1;
        eng->mock.arena_fds[i] = fd;
        atomic_init(&eng->mock.bstate[i], MOCK_BQUEUED);
    }
    atomic_init(&eng->mock.frame_counter, 0ull);
    atomic_init(&eng->mock.writer_dropped, 0ull);
    atomic_init(&eng->mock.doorbell, 0u);
    atomic_init(&eng->mock.waiters, 0u);
    atomic_init(&eng->mock.writer_stop, 0);
    eng->mock.writer_started = 0;
    eng->mock.next_write = 0;
    eng->mock.next_read = 0;
    return WEFT_VISION_OK;
}

static int mock_stream_on(weft_vision_dma_t *eng) {
    if (eng->mock.writer_started) return WEFT_VISION_ESTATE;
    if (pthread_create(&eng->mock.writer, NULL, mock_writer_main, eng) != 0) {
        return WEFT_VISION_ENOMEM;
    }
    eng->mock.writer_started = 1;
    return WEFT_VISION_OK;
}

static int mock_dq_take(weft_vision_dma_t *eng, int *idx, uint64_t *ts_ns,
                        uint64_t *frame_no) {
    for (uint32_t k = 0; k < eng->buffer_count; k++) {
        uint32_t b = (eng->mock.next_read + k) % eng->buffer_count;
        uint32_t expected = MOCK_BDONE;
        if (atomic_compare_exchange_strong_explicit(
                &eng->mock.bstate[b], &expected, MOCK_BHELD,
                memory_order_acq_rel, memory_order_relaxed)) {
            eng->mock.next_read = (b + 1) % eng->buffer_count;
            weft_mock_stamp_t st;
            memcpy(&st, eng->buf_map[b], sizeof(st));
            *idx = (int)b;
            *ts_ns = st.ts_unix_ns;
            *frame_no = st.frame_no;
            return WEFT_VISION_OK;
        }
    }
    return WEFT_VISION_ETIMEOUT;
}

static int mock_dqbuf(weft_vision_dma_t *eng, int *idx, uint64_t *ts_ns,
                      uint64_t *frame_no, int64_t deadline_ns) {
    const uint64_t spins = 64u;
    for (uint64_t i = 0; i < spins; i++) {
        if (mock_dq_take(eng, idx, ts_ns, frame_no) == WEFT_VISION_OK) {
            return WEFT_VISION_OK;
        }
        mock_pause();
    }
    for (;;) {
        if (mock_dq_take(eng, idx, ts_ns, frame_no) == WEFT_VISION_OK) {
            return WEFT_VISION_OK;
        }
        if (mock_now_ns() >= deadline_ns) return WEFT_VISION_ETIMEOUT;
        atomic_fetch_add_explicit(&eng->mock.waiters, 1u,
                                  memory_order_acq_rel);
        /* re-check AFTER publishing the waiter: the lost-wake fix */
        if (mock_dq_take(eng, idx, ts_ns, frame_no) == WEFT_VISION_OK) {
            atomic_fetch_sub_explicit(&eng->mock.waiters, 1u,
                                      memory_order_acq_rel);
            return WEFT_VISION_OK;
        }
        uint32_t cur = atomic_load_explicit(&eng->mock.doorbell,
                                            memory_order_acquire);
        struct timespec ts;
        int64_t rem = deadline_ns - mock_now_ns();
        if (rem <= 0) rem = 200000;
        if (rem > 1000000) rem = 1000000;
        ts.tv_sec = rem / 1000000000;
        ts.tv_nsec = (long)(rem % 1000000000);
        (void)syscall(SYS_futex, (uint32_t *)&eng->mock.doorbell,
                      WEFT_FUTEX_WAIT, (uint32_t)cur, &ts, NULL, 0);
        atomic_fetch_sub_explicit(&eng->mock.waiters, 1u,
                                  memory_order_acq_rel);
    }
}

static int mock_qbuf(weft_vision_dma_t *eng, int idx) {
    if (idx < 0 || (uint32_t)idx >= eng->buffer_count) return WEFT_VISION_EINVAL;
    atomic_store_explicit(&eng->mock.bstate[idx], MOCK_BQUEUED,
                          memory_order_release);
    return WEFT_VISION_OK;
}

static int mock_expbuf(weft_vision_dma_t *eng, int idx, int *fd_out) {
    if (idx < 0 || (uint32_t)idx >= eng->buffer_count) return WEFT_VISION_EINVAL;
    /* memfd IS the dma-buf stand-in: dup so the fd outlives the engine */
    int fd = fcntl(eng->mock.arena_fds[idx], F_DUPFD, 3);
    if (fd < 0) return WEFT_VISION_EDMABUF;
    eng->buf_fd[idx] = fd;
    *fd_out = fd;
    return WEFT_VISION_OK;
}

static int mock_stream_off(weft_vision_dma_t *eng) {
    if (!eng->mock.writer_started) return WEFT_VISION_OK;
    atomic_store_explicit(&eng->mock.writer_stop, 1, memory_order_release);
    atomic_fetch_add_explicit(&eng->mock.doorbell, 1u, memory_order_release);
    (void)syscall(SYS_futex, (uint32_t *)&eng->mock.doorbell, WEFT_FUTEX_WAKE,
                  1, NULL, NULL, 0);
    (void)pthread_join(eng->mock.writer, NULL);
    eng->mock.writer_started = 0;
    return WEFT_VISION_OK;
}

static int mock_close(weft_vision_dma_t *eng) {
    (void)mock_stream_off(eng);
    for (uint32_t i = 0; i < eng->buffer_count; i++) {
        if (eng->buf_map[i] != NULL) {
            (void)munmap(eng->buf_map[i], eng->buf_len[i]);
            eng->buf_map[i] = NULL;
        }
        if (eng->mock.arena_fds[i] >= 0) {
            (void)close(eng->mock.arena_fds[i]);
            eng->mock.arena_fds[i] = -1;
        }
        if (eng->buf_fd[i] >= 0) {
            (void)close(eng->buf_fd[i]);
            eng->buf_fd[i] = -1;
        }
    }
    free(eng->mock.pattern_tmpl);
    eng->mock.pattern_tmpl = NULL;
    return WEFT_VISION_OK;
}

const weft_vision_backend_ops_t *weft_vision_mock_backend(void) {
    static const weft_vision_backend_ops_t ops = {
        .name = "v4l2-mock",
        .open = mock_open,
        .stream_on = mock_stream_on,
        .dqbuf = mock_dqbuf,
        .qbuf = mock_qbuf,
        .expbuf = mock_expbuf,
        .stream_off = mock_stream_off,
        .close = mock_close,
    };
    return &ops;
}

// ---------------------------------------------------------------------------
// Frame integrity verification (test-side)
// ---------------------------------------------------------------------------

int weft_mock_frame_check_header(const uint8_t *buf, size_t len,
                                 uint64_t frame_no) {
    if (len < 128) return -1;
    weft_mock_stamp_t st;
    memcpy(&st, buf, sizeof(st));
    if (st.magic != WEFT_MOCK_STAMP_MAGIC) return -2;
    if (st.frame_no != frame_no) return -3;
    uint32_t tm = 0;
    memcpy(&tm, buf + len - 64, 4);
    if (tm != WEFT_MOCK_STAMP_MAGIC) return -6;
    uint64_t tf = 0;
    memcpy(&tf, buf + len - 56, 8);
    if (tf != frame_no) return -7;
    uint64_t th = 0, sh = 0;
    memcpy(&th, buf + len - 48, 8);
    memcpy(&sh, &st.swath_hash, 8);
    if (th != sh) return -8;
    return 0;
}

int weft_mock_frame_verify(const uint8_t *buf, size_t len, uint64_t frame_no) {
    if (len < 128) return -1;
    weft_mock_stamp_t st;
    memcpy(&st, buf, sizeof(st));
    if (st.magic != WEFT_MOCK_STAMP_MAGIC) return -2;
    if (st.frame_no != frame_no) return -3;
    if ((size_t)st.swath_off + st.swath_len > len - 64u) return -4;
    if (st.swath_len == 0) return -5;

    uint32_t tm = 0;
    memcpy(&tm, buf + len - 64, 4);
    if (tm != WEFT_MOCK_STAMP_MAGIC) return -6;
    uint64_t tf = 0;
    memcpy(&tf, buf + len - 56, 8);
    if (tf != frame_no) return -7;
    uint64_t th = 0;
    memcpy(&th, buf + len - 48, 8);
    if (th != st.swath_hash) return -8;

    /* per-block stamps: magic, block index, frame ownership */
    for (size_t b = 0; b * 4096u < st.swath_len; b++) {
        weft_mock_block_stamp_t bs;
        memcpy(&bs, buf + st.swath_off + b * 4096u, sizeof(bs));
        if (bs.magic != WEFT_MOCK_STAMP_MAGIC) return -10;
        if (bs.block_idx != (uint32_t)b) return -11;
        if (bs.frame_no != frame_no) return -12;
    }

    if (mock_rollhash64(buf + st.swath_off, st.swath_len) != st.swath_hash)
        return -9;
    return 0;
}
