// bench_spectrum_native.c — the native scoreboard + performance gates
// (Pillar 5, D-52).
//
// GATES (fail-closed: any FAIL makes the process exit non-zero):
//   G1  SIMD throughput: geometric-mean speedup of the best vector engine
//       vs the scalar reference across the 5-kernel suite at a cache-
//       resident working set must be >= 8x (the mission's Law). A DRAM-
//       streaming leg is ALSO reported — honestly, without a gate, because
//       element-wise streaming kernels are bandwidth-bound everywhere.
//   G2  GPU/NPU dispatch latency: p99 of end-to-end DEVICE dispatch
//       (map -> packet -> enqueue -> poll-ladder -> device compute ->
//       unmap -> result) per burst must be < 15 us on the mock silicon
//       rows (NPU/GPU/DSP pipelines).
//   G3  Fallback hop: p50 steady-state dispatch across a refusal walk to
//       the CPU vector engine must be < 1 us (Law 3).
//
// Timing: CLOCK_MONOTONIC via weft_backend_now_ns (vDSO). Every
// measurement is auto-calibrated to >= 20 ms of samples; percentiles come
// from a pre-allocated ns-bucket histogram (no allocation in the timed
// regions — the module heap lock stays engaged throughout).

#include "mock/weft_mock_dma.h"
#include "../../core/c/spectrum/drivers/weft_backend.h"
#include "../../core/c/spectrum/simd/weft_simd.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_gates_failed = 0;

#define GATE(ok, name, detail)                                                  \
    do {                                                                        \
        printf("GATE %-28s %s  %s\n", (name), (ok) ? "PASS" : "FAIL", (detail));\
        if (!(ok)) {                                                            \
            g_gates_failed++;                                                   \
        }                                                                       \
    } while (0)

// ---------------------------------------------------------------------------
// Auto-calibrated kernel timing (>= 20ms of samples per number)
// ---------------------------------------------------------------------------

typedef struct {
    const weft_simd_kernels_t* kt;
    float* fsrc;
    float* fdst;
    uint32_t* usrc;
    uint32_t* udst;
    size_t elems;
    uint32_t m, k, n, lda, ldb, ldc;
    uint32_t seed, stamp;
    const void* cbuf;
    size_t cbytes;
} kern_arg_t;

static void run_kern(const kern_arg_t* a, int which) {
    switch (which) {
    case 0:
        a->kt->normalize(a->fdst, a->fsrc, a->elems, -1.0f, 1.0f / 4096.0f);
        break;
    case 1:
        a->kt->delta_encode(a->udst, a->usrc, a->elems, a->seed);
        break;
    case 2:
        a->kt->delta_decode(a->udst, a->usrc, a->elems, a->seed);
        break;
    case 3:
        a->kt->dot_f32(a->fdst, a->fsrc, (const float*)a->usrc, a->m, a->k,
                       a->n, a->lda, a->ldb, a->ldc);
        break;
    case 4:
        (void)a->kt->seqlock_checksum(a->cbuf, a->cbytes, a->stamp);
        break;
    default:
        break;
    }
}

static uint64_t time_kern_ns(const kern_arg_t* a, int which) {
    run_kern(a, which);
    run_kern(a, which);
    uint64_t reps = 1;
    for (;;) {
        const uint64_t t0 = weft_backend_now_ns();
        for (uint64_t i = 0; i < reps; i++) {
            run_kern(a, which);
        }
        const uint64_t dt = weft_backend_now_ns() - t0;
        if (dt >= 20000000ull) {
            return dt / reps;
        }
        reps *= 2;
    }
}

// ---------------------------------------------------------------------------
// G1: SIMD scoreboard
// ---------------------------------------------------------------------------

#define WS_L1 (16u * 1024u)     // cache-resident working set
#define WS_STREAM (4u << 20)    // DRAM streaming leg (report-only)

static unsigned char g_pool[16u << 20] __attribute__((aligned(64)));

static void simd_scoreboard(void) {
    static const char* KNAMES[5] = {
        "normalize_f32", "delta_encode_u32", "delta_decode_u32",
        "dot_f32(128x64x64)", "seqlock_checksum",
    };
    const uint32_t impls[3] = { WEFT_SIMD_SCALAR, WEFT_SIMD_AVX2,
                                WEFT_SIMD_AVX512 };
    const char* inames[3] = { "scalar", "avx2", "avx512" };

    printf("\n=== SIMD throughput scoreboard (working set %u B, "
           "cache-resident) ===\n",
           WS_L1);
    printf("%-22s %14s %14s %8s %14s %8s\n", "kernel", "scalar ns/op",
           "avx2 ns/op", "x", "avx512 ns/op", "x");

    kern_arg_t a;
    memset(&a, 0, sizeof(a));
    a.elems = WS_L1 / 4;   // f32/u32 elements
    a.fsrc = (float*)g_pool;
    a.fdst = (float*)(g_pool + (4u << 20));    // 4MiB regions, room for the
    a.usrc = (uint32_t*)(g_pool + (8u << 20)); // streaming leg too
    a.udst = (uint32_t*)(g_pool + (12u << 20));
    a.m = 128; a.k = 64; a.n = 64;
    a.lda = 64; a.ldb = 64; a.ldc = 64;
    a.seed = 7;
    a.stamp = 42;
    a.cbuf = g_pool;
    a.cbytes = WS_L1;

    uint64_t t_best[5] = { 0 };
    uint64_t t_scal[5] = { 0 };
    double log_sum = 0.0;

    for (int w = 0; w < 5; w++) {
        uint64_t times[3] = { 0 };
        int have[3] = { 0 };
        for (int i = 0; i < 3; i++) {
            const weft_simd_kernels_t* kt =
                weft_simd_impl_kernels((weft_simd_impl_t)impls[i]);
            if (kt == NULL) {
                continue;
            }
            a.kt = kt;
            times[i] = time_kern_ns(&a, w);
            have[i] = 1;
        }
        t_scal[w] = times[0];
        int best = 0;
        for (int i = 1; i < 3; i++) {
            if (have[i] && (times[i] < times[best] || !have[best])) {
                best = i;
            }
        }
        t_best[w] = times[best];
        const double speedup = (double)t_scal[w] / (double)t_best[w];
        log_sum += log(speedup);

        printf("%-22s %14" PRIu64 " %14s %8s %14s %8s\n", KNAMES[w], times[0],
               have[1] ? "" : "-", have[1] ? "" : "-",
               have[2] ? "" : "-", have[2] ? "" : "-");
        (void)inames;
        // (per-impl columns printed below in detail lines)
        for (int i = 0; i < 3; i++) {
            if (have[i]) {
                const double gbps =
                    (w == 3)
                        ? (128.0 * 64 + 64 * 64 + 128 * 64) * 4.0 /
                          (double)times[i]
                        : ((w == 4) ? (double)a.cbytes
                                    : 2.0 * (double)WS_L1) /
                              (double)times[i];
                printf("    %-8s %10" PRIu64 " ns/op  %8.2f GB/s  %.2fx\n",
                       inames[i], times[i], gbps,
                       (double)times[0] / (double)times[i]);
            }
        }
    }

    const double geomean = exp(log_sum / 5.0);
    char detail[96];
    snprintf(detail, sizeof(detail), "geomean(best-vs-scalar) = %.2fx", geomean);
    GATE(geomean >= 8.0, "simd-geomean>=8x", detail);

    // ---- Streaming honesty leg (report-only, no gate) --------------------
    printf("\n=== Streaming leg (%u B, DRAM-bound — report only) ===\n",
           (unsigned)WS_STREAM);
    a.elems = WS_STREAM / 4;
    a.cbytes = WS_STREAM;
    for (int w = 0; w < 5; w++) {
        if (w == 3) {
            printf("    dot_f32           (skipped: chain-bound, not "
                   "bandwidth-representative)\n");
            continue;
        }
        uint64_t ts = 0, tb = 0;
        for (int i = 0; i < 3; i++) {
            const weft_simd_kernels_t* kt =
                weft_simd_impl_kernels((weft_simd_impl_t)impls[i]);
            if (kt == NULL) {
                continue;
            }
            a.kt = kt;
            const uint64_t t = time_kern_ns(&a, w);
            if (i == 0) {
                ts = t;
            }
            if (i == 2) {
                tb = t;
            }
        }
        const double gbps =
            ((w == 4) ? (double)WS_STREAM : 2.0 * (double)WS_STREAM) /
            (double)tb;
        printf("    %-18s avx512 %10" PRIu64
               " ns/op  %8.2f GB/s  %.2fx vs scalar\n",
               KNAMES[w], tb, gbps, (double)ts / (double)tb);
    }
}

// ---------------------------------------------------------------------------
// G2: DEVICE dispatch latency per burst (mock silicon rows)
// ---------------------------------------------------------------------------

static void dispatch_latency(void) {
    weft_mock_dma_cfg_t cfgs[WEFT_VENDOR_COUNT] = {
        { .name = "fastrpc-adsp-mock",  .fixed_ns = 1500, .ps_per_byte = 50,
          .capacity_bytes = 2u << 20 },
        { .name = "neuropilot-apu-mock", .fixed_ns = 800,  .ps_per_byte = 40,
          .capacity_bytes = 1u << 20 },
        { .name = "metal3-ane-mock",     .fixed_ns = 600,  .ps_per_byte = 30,
          .capacity_bytes = 1u << 20 },
        { .name = "cuda-stream-mock",    .fixed_ns = 2500, .ps_per_byte = 20,
          .capacity_bytes = 4u << 20 },
    };
    weft_dma_transport_t* t[4];
    weft_backend_cfg_t bc;
    memset(&bc, 0, sizeof(bc));
    for (int i = 0; i < 4; i++) {
        t[i] = weft_mock_dma_new(&cfgs[i]);
        bc.transport_overrides[i] = t[i];
    }
    weft_backend_ctx_t* ctx = weft_backend_ctx_create(&bc);
    if (ctx == NULL) {
        GATE(0, "dispatch-ctx", "ctx create failed");
        return;
    }

    printf("\n=== GPU/NPU dispatch latency per burst (DEVICE rows, "
           "end-to-end) ===\n");
    static const uint32_t BURSTS[3] = { 1024, 4096, 16384 };   // elements
    float* src = (float*)g_pool;
    float* dst = (float*)(g_pool + (4u << 20));
    int worst_ok = 1;
    uint64_t worst_p99 = 0;
    for (int b = 0; b < 3; b++) {
        const uint32_t elems = BURSTS[b];
        for (uint32_t i = 0; i < elems; i++) {
            src[i] = (float)i * 0.5f;
        }
        weft_op_desc_t op;
        memset(&op, 0, sizeof(op));
        op.kind = WEFT_OP_NORMALIZE_F32;
        op.m = elems;
        op.f0 = 0.0f;
        op.f1 = 1.0f / (float)elems;
        op.bufs[0].data = src;
        op.bufs[0].bytes = (uint64_t)elems * 4;
        op.bufs[0].dtype = WEFT_BACKEND_DTYPE_F32;
        op.bufs[2].data = dst;
        op.bufs[2].bytes = (uint64_t)elems * 4;
        op.bufs[2].dtype = WEFT_BACKEND_DTYPE_F32;

        weft_dispatch_result_t res;
        memset(&res, 0, sizeof(res));
        enum { N = 4000 };
        static uint64_t samples[N];
        for (int i = 0; i < 200; i++) {   // warmup
            (void)weft_backend_dispatch(ctx, &op, &res);
        }
        for (int i = 0; i < N; i++) {
            const uint64_t t0 = weft_backend_now_ns();
            (void)weft_backend_dispatch(ctx, &op, &res);
            samples[i] = weft_backend_now_ns() - t0;
        }
        // ns-bucket histogram (pre-allocated)
        enum { B = 65536 };
        static uint32_t hist[B];
        memset(hist, 0, sizeof(hist));
        uint32_t over = 0;
        for (int i = 0; i < N; i++) {
            if (samples[i] < B) {
                hist[samples[i]]++;
            } else {
                over++;
            }
        }
        uint32_t seen = 0;
        uint64_t p50 = 0, p99 = 0;
        for (uint32_t x = 0; x < B; x++) {
            seen += hist[x];
            if (p50 == 0 && seen >= N / 2) {
                p50 = x;
            }
            if (p99 == 0 && seen >= (uint32_t)(N * 99) / 100) {
                p99 = x;
            }
        }
        if (p99 == 0 && over > 0) {
            p99 = B;   // 1%+ of samples beyond the histogram: report >= B
        }
        if (p50 == 0 && over >= (uint32_t)N / 2) {
            p50 = B;
        }
        if (p99 > worst_p99) {
            worst_p99 = p99;
        }
        printf("    burst %2u KiB: p50=%6" PRIu64 " ns  p99=%6" PRIu64
               " ns  (over-64us outliers: %u)\n",
               (unsigned)(elems * 4 / 1024), p50, p99, over);
        if (p99 >= 15000) {
            worst_ok = 0;
        }
    }
    char detail[96];
    snprintf(detail, sizeof(detail), "worst p99 = %" PRIu64 " ns", worst_p99);
    GATE(worst_ok, "dispatch-p99<15us", detail);

    weft_backend_ctx_destroy(ctx);
    for (int i = 0; i < 4; i++) {
        weft_mock_dma_destroy(t[i]);
    }
}

// ---------------------------------------------------------------------------
// G3: fallback hop steady state (Law 3)
// ---------------------------------------------------------------------------

static void fallback_latency(void) {
    // ctx WITHOUT device transports: every dispatch walks dead-init rows
    // (honest absence) straight to the PC host-vector engine — the exact
    // steady state a refused engine leaves behind.
    weft_backend_ctx_t* ctx = weft_backend_ctx_create(NULL);
    if (ctx == NULL) {
        GATE(0, "fallback-ctx", "ctx create failed");
        return;
    }
    static unsigned char tiny[8] __attribute__((aligned(64)));
    weft_op_desc_t op;
    memset(&op, 0, sizeof(op));
    op.kind = WEFT_OP_SEQLOCK_CHECKSUM;
    op.u0 = 1;
    op.bufs[0].data = tiny;
    op.bufs[0].bytes = 8;
    op.bufs[0].dtype = WEFT_BACKEND_DTYPE_U32;
    weft_dispatch_result_t res;
    memset(&res, 0, sizeof(res));

    for (int i = 0; i < 10000; i++) {
        (void)weft_backend_dispatch(ctx, &op, &res);
    }
    enum { N = 200000 };
    static uint64_t samples[N];
    for (int i = 0; i < N; i++) {
        const uint64_t t0 = weft_backend_now_ns();
        (void)weft_backend_dispatch(ctx, &op, &res);
        samples[i] = weft_backend_now_ns() - t0;
    }
    enum { B = 8192 };
    static uint32_t hist[B];
    memset(hist, 0, sizeof(hist));
    uint32_t over = 0;
    for (int i = 0; i < N; i++) {
        if (samples[i] < B) {
            hist[samples[i]]++;
        } else {
            over++;
        }
    }
    uint32_t seen = 0;
    uint64_t p50 = 0, p99 = 0;
    for (uint32_t x = 0; x < B; x++) {
        seen += hist[x];
        if (p50 == 0 && seen >= N / 2) {
            p50 = x;
        }
        if (p99 == 0 && seen >= (uint32_t)(N * 99) / 100) {
            p99 = x;
        }
    }
    if (p99 == 0 && over > 0) {
        p99 = B;
    }
    if (p50 == 0 && over >= (uint32_t)N / 2) {
        p50 = B;
    }
    printf("\n=== Fallback hop steady state (refused engine -> CPU vector) "
           "===\n");
    printf("    p50=%" PRIu64 " ns  p99=%" PRIu64 " ns  (outliers>%dns: %u)\n",
           p50, p99, B, over);
    char detail[96];
    snprintf(detail, sizeof(detail), "p50 = %" PRIu64 " ns", p50);
    GATE(p50 < 1000, "fallback-p50<1us", detail);

    weft_backend_stats_t st;
    weft_backend_stats(ctx, &st);
    printf("    [witnesses] heap-locked=%d heap-bytes=%" PRIu64
           " dispatches=%" PRIu64 "\n",
           weft_backend_heap_locked(), weft_backend_heap_bytes(),
           st.dispatches);
    weft_backend_ctx_destroy(ctx);
}

int main(void) {
    printf("weft-spectrum native bench — impl=%s caps=0x%X\n",
           weft_simd_active_impl_name(), weft_simd_compiled_caps());
    simd_scoreboard();
    dispatch_latency();
    fallback_latency();
    if (g_gates_failed != 0) {
        printf("\nSPECTRUM-NATIVE-VERDICT: FAIL (%d gate(s))\n", g_gates_failed);
        return 1;
    }
    printf("\nSPECTRUM-NATIVE-VERDICT: PASS\n");
    return 0;
}
