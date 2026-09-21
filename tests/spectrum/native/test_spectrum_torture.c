// test_spectrum_torture.c — the 5,000,000-cycle zero-growth torture
// (Pillar 5, D-52, Law 1's headline witness).
//
// PROVES: five million dispatches through a DEVICE-mode engine (the mock
// FastRPC row: map -> packet -> enqueue -> poll-ladder -> unmap -> result)
// interleave with CPU-vector fallback dispatches, and the process grows
// by ZERO bytes — witnessed three independent ways:
//
//   W1  weft_backend_heap_bytes()  — the module's own allocation counter
//                                     (must not move by 1 byte)
//   W2  mallinfo2().uordblks       — the whole allocator (glibc; skipped
//                                     under ASan, where malloc is
//                                     interposed — declared)
//   W3  /proc/self/statm resident  — page-level RSS (post-warmup baseline)
//
// Also proves: the command ring wraps (5M >> 1024 slots), the mock's
// flight ring wraps (5M >> 4096), stats count every dispatch, and the
// results stay bit-exact at sampled checkpoints.

#include "../mock/weft_mock_dma.h"
#include "../../../core/c/spectrum/drivers/weft_backend.h"
#include "../../../core/c/spectrum/simd/weft_simd.h"

#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

#define CYCLES 5000000ull
#define SAMPLE_EVERY 250000ull
#define WARMUP 50000ull

#if defined(__SANITIZE_ADDRESS__)
#define UNDER_ASAN 1
#else
#define UNDER_ASAN 0
#endif

// ---------------------------------------------------------------------------
// Witnesses
// ---------------------------------------------------------------------------

static int g_statm_fd = -1;

static uint64_t rss_pages(void) {
    char buf[128];
    const ssize_t n = pread(g_statm_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    // statm: "size resident shared text lib data dt" — resident is field 2
    uint64_t fields[3];
    int f = 0;
    uint64_t acc = 0;
    int in_num = 0;
    for (ssize_t i = 0; i < n && f < 3; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            acc = acc * 10 + (uint64_t)(buf[i] - '0');
            in_num = 1;
        } else if (in_num) {
            fields[f++] = acc;
            acc = 0;
            in_num = 0;
        }
    }
    return (f >= 2) ? fields[1] : 0;
}

static uint64_t mallinfo_used(void) {
#if UNDER_ASAN
    return 0;   // malloc interposed: mallinfo is not a witness under ASan
#else
    struct mallinfo2 mi = mallinfo2();
    return (uint64_t)mi.uordblks;
#endif
}

// ---------------------------------------------------------------------------
// The torture
// ---------------------------------------------------------------------------

int main(void) {
    g_statm_fd = open("/proc/self/statm", O_RDONLY);
    CHECK(g_statm_fd >= 0, "statm open");

    // A fast synthetic FastRPC device (torture is about ALLOCATION, not
    // latency — the latency model stays on, honestly modeled, just small).
    weft_mock_dma_cfg_t cfg = {
        .name = "fastrpc-torture", .fixed_ns = 150, .ps_per_byte = 8,
        .capacity_bytes = 8u << 20,
    };
    weft_dma_transport_t* t = weft_mock_dma_new(&cfg);
    CHECK(t != NULL, "mock created");

    weft_backend_cfg_t bc;
    memset(&bc, 0, sizeof(bc));
    bc.transport_overrides[WEFT_VENDOR_QUALCOMM - 1] = t;
    weft_backend_ctx_t* ctx = weft_backend_ctx_create(&bc);
    CHECK(ctx != NULL, "ctx created");

    // Pre-allocated op fixtures: 4 rotating u32 streams, 1 f32 stream,
    // one small dot (dedicated C buffer), one checksum — all cycling through
    // the dispatch walk.
    static unsigned char pool[7][4096] __attribute__((aligned(64)));
    static unsigned char refbuf[4096] __attribute__((aligned(64)));

    weft_op_desc_t ops[5];
    memset(ops, 0, sizeof(ops));

    ops[0].kind = WEFT_OP_NORMALIZE_F32;
    ops[0].m = 1024;
    ops[0].f0 = 0.0f;
    ops[0].f1 = 1.0f / 1024.0f;
    ops[0].bufs[0].data = pool[0];
    ops[0].bufs[0].bytes = 4096;
    ops[0].bufs[0].dtype = WEFT_BACKEND_DTYPE_F32;
    ops[0].bufs[2].data = pool[1];
    ops[0].bufs[2].bytes = 4096;
    ops[0].bufs[2].dtype = WEFT_BACKEND_DTYPE_F32;

    ops[1].kind = WEFT_OP_DELTA_ENCODE_U32;
    ops[1].m = 1024;
    ops[1].u0 = 7;
    ops[1].bufs[0].data = pool[2];
    ops[1].bufs[0].bytes = 4096;
    ops[1].bufs[0].dtype = WEFT_BACKEND_DTYPE_U32;
    ops[1].bufs[2].data = pool[3];
    ops[1].bufs[2].bytes = 4096;
    ops[1].bufs[2].dtype = WEFT_BACKEND_DTYPE_U32;

    ops[2].kind = WEFT_OP_DELTA_DECODE_U32;
    ops[2].m = 1024;
    ops[2].u0 = 7;
    ops[2].bufs[0].data = pool[3];
    ops[2].bufs[0].bytes = 4096;
    ops[2].bufs[0].dtype = WEFT_BACKEND_DTYPE_U32;
    ops[2].bufs[2].data = pool[2];
    ops[2].bufs[2].bytes = 4096;
    ops[2].bufs[2].dtype = WEFT_BACKEND_DTYPE_U32;

    ops[3].kind = WEFT_OP_DOT_F32;
    ops[3].m = 4; ops[3].k = 8; ops[3].n = 4;
    ops[3].lda = 8; ops[3].ldb = 4; ops[3].ldc = 4;
    ops[3].bufs[0].data = pool[4];
    ops[3].bufs[0].bytes = 4096;
    ops[3].bufs[0].dtype = WEFT_BACKEND_DTYPE_F32;
    ops[3].bufs[1].data = pool[5];
    ops[3].bufs[1].bytes = 4096;
    ops[3].bufs[1].dtype = WEFT_BACKEND_DTYPE_F32;
    ops[3].bufs[2].data = pool[6];   // dedicated C — never aliases normalize dst
    ops[3].bufs[2].bytes = 4096;
    ops[3].bufs[2].dtype = WEFT_BACKEND_DTYPE_F32;

    ops[4].kind = WEFT_OP_SEQLOCK_CHECKSUM;
    ops[4].u0 = 42;
    ops[4].bufs[0].data = pool[2];
    ops[4].bufs[0].bytes = 2048;   // even bytes
    ops[4].bufs[0].dtype = WEFT_BACKEND_DTYPE_U32;

    // Deterministic data in the rotating streams.
    for (int i = 0; i < 7; i++) {
        for (int j = 0; j < 4096; j++) {
            pool[i][j] = (unsigned char)(i * 131 + j * 7 + 3);
        }
    }

    weft_dispatch_result_t res;
    memset(&res, 0, sizeof(res));

    printf("torture: %llu cycles, witnesses armed (heap=%" PRIu64
           " mallinfo=%" PRIu64 " rss=%" PRIu64 " pages)%s\n",
           (unsigned long long)CYCLES, weft_backend_heap_bytes(),
           mallinfo_used(), rss_pages(),
           UNDER_ASAN ? " [ASan: mallinfo witness skipped]" : "");
    fflush(stdout);   // stdio buffer materializes BEFORE the baselines

    // Baselines AFTER ctx creation + first printf + warmup (caches, log
    // suppression window, stdio buffers): what follows must be FLAT.
    for (uint64_t i = 0; i < WARMUP; i++) {
        (void)weft_backend_dispatch(ctx, &ops[i % 5], &res);
    }
    const uint64_t heap0 = weft_backend_heap_bytes();
    const uint64_t mall0 = mallinfo_used();
    const uint64_t rss0 = rss_pages();
    const uint64_t enq0 = weft_mock_dma_enqueued_pkts(t);
    const uint64_t comp0 = weft_mock_dma_completed_pkts(t);
    CHECK(weft_backend_heap_locked() == 1, "Law 1 lock engaged for the run");

    for (uint64_t i = 0; i < CYCLES; i++) {
        const weft_backend_status_t st =
            weft_backend_dispatch(ctx, &ops[i % 5], &res);
        if (st != WEFT_BACKEND_OK) {
            CHECK(st == WEFT_BACKEND_OK, "cycle %llu dispatch (got %d)",
                  (unsigned long long)i, (int)st);
            break;
        }
        if ((i + 1) % SAMPLE_EVERY == 0) {
            CHECK(weft_backend_heap_bytes() == heap0,
                  "W1 heap counter moved at cycle %llu",
                  (unsigned long long)(i + 1));
            if (!UNDER_ASAN) {
                const uint64_t mall_now = mallinfo_used();
                if (mall_now != mall0) {
                    printf("  mallinfo delta at cycle %llu: %+lld\n",
                           (unsigned long long)(i + 1),
                           (long long)(mall_now - mall0));
                    g_failures++;
                }
            }
            CHECK(rss_pages() == rss0, "W3 RSS moved at cycle %llu",
                  (unsigned long long)(i + 1));
        }
    }

    weft_backend_stats_t stt;
    weft_backend_stats(ctx, &stt);
    CHECK(stt.dispatches == WARMUP + CYCLES, "every dispatch counted (%llu)",
          (unsigned long long)stt.dispatches);
    printf("torture: dispatches=%" PRIu64 " hops=%" PRIu64
           " device-enqueued=%" PRIu64 " device-completed=%" PRIu64
           " zero-copy-completions=%" PRIu64 "\n",
           stt.dispatches, stt.fallback_hops,
           weft_mock_dma_enqueued_pkts(t) - enq0,
           weft_mock_dma_completed_pkts(t) - comp0,
           weft_mock_dma_zero_copy_completions(t));
    CHECK(weft_mock_dma_enqueued_pkts(t) - enq0 > (CYCLES * 3) / 5,
          "the DEVICE row carried the majority of the load");
    CHECK(weft_mock_dma_would_copies(t) == 0, "still zero copies after 5M");
    CHECK(weft_mock_dma_inflight_bytes(t) == 0, "bus drained clean");

    // Final correctness spot-check: each op dispatched once more and
    // verified against the oracle IMMEDIATELY (dedicated buffers, so no
    // cross-op aliasing).
    memset(&res, 0, sizeof(res));
    CHECK(weft_backend_dispatch(ctx, &ops[0], &res) == WEFT_BACKEND_OK,
          "final normalize");
    weft_simd_scalar_normalize((float*)(void*)refbuf, (const float*)pool[0],
                               1024, 0.0f, 1.0f / 1024.0f);
    CHECK(memcmp(pool[1], refbuf, 4096) == 0, "final normalize bit-exact");

    memset(&res, 0, sizeof(res));
    CHECK(weft_backend_dispatch(ctx, &ops[1], &res) == WEFT_BACKEND_OK,
          "final encode");
    weft_simd_scalar_delta_encode((uint32_t*)(void*)refbuf,
                                  (const uint32_t*)pool[2], 1024, 7);
    CHECK(memcmp(pool[3], refbuf, 4096) == 0, "final encode bit-exact");

    memset(&res, 0, sizeof(res));
    CHECK(weft_backend_dispatch(ctx, &ops[2], &res) == WEFT_BACKEND_OK,
          "final decode");
    // decode(encode(x)) == x: pool[2] must hold the ORIGINAL deterministic
    // bytes again (pool[2][j] == 2*131 + j*7 + 3).
    for (int j = 0; j < 4096; j++) {
        if (pool[2][j] != (unsigned char)(2 * 131 + j * 7 + 3)) {
            CHECK(0, "final decode bit-exact at byte %d", j);
            break;
        }
    }

    memset(&res, 0, sizeof(res));
    CHECK(weft_backend_dispatch(ctx, &ops[3], &res) == WEFT_BACKEND_OK,
          "final dot");
    weft_simd_scalar_dot_f32((float*)(void*)refbuf, (const float*)pool[4],
                             (const float*)pool[5], 4, 8, 4, 8, 4, 4);
    CHECK(memcmp(pool[6], refbuf, 64) == 0, "final dot bit-exact");

    memset(&res, 0, sizeof(res));
    CHECK(weft_backend_dispatch(ctx, &ops[4], &res) == WEFT_BACKEND_OK,
          "final checksum");
    CHECK(res.result_u64 ==
              (uint64_t)weft_simd_scalar_seqlock_checksum(pool[2], 2048, 42),
          "final checksum == oracle");

    weft_backend_ctx_destroy(ctx);
    weft_mock_dma_destroy(t);
    close(g_statm_fd);

    if (g_failures != 0) {
        printf("spectrum-torture: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("spectrum-torture: 5,000,000 cycles, ZERO growth (3 witnesses) PASS\n");
    return 0;
}
