// test_spectrum_dispatch.c — context lifecycle + dispatch semantics
// (Pillar 5, D-52).
//
// PROVES (with mock-injected DEVICE entries):
//   * deterministic table order (class asc, score desc) + honest dead
//     entries visible with reasons (probe-refused, init-refused)
//   * end-to-end DEVICE-path correctness: results land in the CALLER's
//     buffers, bit-exact vs the normative oracle (zero-copy, Law 2)
//   * Law 3: checksums (no device affinity) hop to the CPU vector engine
//     with fallback_hops counted; a hot-unplugged engine (EDEVICE) dies
//     deterministically and dispatch COMPLETES on the next engine
//   * Law 4: EBUSY (bus saturation) propagates — never silently rerouted;
//     the Law-2 breaker is caught by the driver's structural check
//     (ESTATE, fail-closed); argument-law violations return EINVAL with
//     zero side effects
//   * Law 1: the module heap lock is engaged and heap_bytes never moves
//     across thousands of dispatches

#include "../mock/weft_mock_dma.h"
#include "../../../core/c/spectrum/drivers/weft_backend.h"
#include "../../../core/c/spectrum/simd/weft_simd.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

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

#define POOL_BYTES (256u * 1024u)
static unsigned char g_pool[POOL_BYTES] __attribute__((aligned(64)));
static unsigned char g_ref[POOL_BYTES] __attribute__((aligned(64)));

static weft_mock_dma_cfg_t g_dev_cfgs[WEFT_VENDOR_COUNT] = {
    { .name = "fastrpc-adsp-mock",  .fixed_ns = 1500, .ps_per_byte = 50,
      .capacity_bytes = 2u << 20 },
    { .name = "neuropilot-apu-mock", .fixed_ns = 800,  .ps_per_byte = 40,
      .capacity_bytes = 1u << 20 },
    { .name = "metal3-ane-mock",     .fixed_ns = 600,  .ps_per_byte = 30,
      .capacity_bytes = 1u << 20 },
    { .name = "cuda-stream-mock",    .fixed_ns = 2500, .ps_per_byte = 20,
      .capacity_bytes = 4u << 20 },
    { .name = "sve2-rvv-host-mock",  .fixed_ns = 0,    .ps_per_byte = 0,
      .capacity_bytes = 0 },   // riscv_arm row: never injected (host row)
};

/// Build a ctx with the given set of injected mock transports.
/// mask bit i (vendor-1) injects vendor i's transport.
static weft_backend_ctx_t* make_ctx(uint32_t inject_mask, uint32_t ctx_flags) {
    weft_dma_transport_t* transports[WEFT_VENDOR_COUNT] = { 0 };
    weft_backend_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    for (uint32_t v = 0; v < WEFT_VENDOR_COUNT; v++) {
        if ((inject_mask & (1u << v)) != 0u) {
            transports[v] = weft_mock_dma_new(&g_dev_cfgs[v]);
            CHECK(transports[v] != NULL, "mock %u created", v);
            cfg.transport_overrides[v] = transports[v];
        }
    }
    cfg.flags = ctx_flags;
    weft_backend_ctx_t* ctx = weft_backend_ctx_create(&cfg);
    CHECK(ctx != NULL, "ctx created (mask=0x%x)", inject_mask);
    (void)transports;
    return ctx;
}

static void destroy_ctx(weft_backend_ctx_t* ctx, uint32_t inject_mask) {
    weft_backend_ctx_destroy(ctx);
    (void)inject_mask;
}

static weft_buffer_desc_t mk_buf(void* data, uint64_t bytes, uint32_t dtype) {
    weft_buffer_desc_t d;
    memset(&d, 0, sizeof(d));
    d.data = data;
    d.bytes = bytes;
    d.dtype = dtype;
    d.flags = WEFT_BUF_HOST | WEFT_BUF_WRITABLE;
    return d;
}

// ---------------------------------------------------------------------------
// Table order + honest death ledger
// ---------------------------------------------------------------------------

static void test_table_order(void) {
    weft_backend_ctx_t* ctx = make_ctx(0xF, 0);   // all four device rows
    weft_backend_info_t info[WEFT_BACKEND_MAX_BACKENDS];
    const uint32_t n = weft_backend_table_info(ctx, info, WEFT_BACKEND_MAX_BACKENDS);
    CHECK(n == 6, "6 builtin entries, got %u", n);

    // Expected total order: apple(NPU,900) mediatek(NPU,800) qualcomm(DSP,720)
    // riscv_arm(CPU_VECTOR,500 — probe-dead but identity-kept) nvidia_pc
    // (CPU_VECTOR,400) cpu_simd(CPU_VECTOR,1)
    CHECK(info[0].vendor_id == WEFT_VENDOR_APPLE && info[0].state == 1,
          "entry0 apple live");
    CHECK(info[1].vendor_id == WEFT_VENDOR_MEDIATEK && info[1].state == 1,
          "entry1 mediatek live");
#if defined(__aarch64__)
    CHECK(info[2].vendor_id == WEFT_VENDOR_NVIDIA_PC && info[2].state == 0,
          "entry2 nvidia-pc probe-dead (wrong arch)");
    CHECK(info[2].death_reason == WEFT_BACKEND_EREFUSED,
          "nvidia_pc death reason EREFUSED");
    CHECK(strstr(info[2].impl_name, "absent(wrong-arch)") != NULL,
          "nvidia_pc identity kept after death, got '%s'", info[2].impl_name);
    CHECK(info[3].vendor_id == WEFT_VENDOR_QUALCOMM && info[3].state == 1,
          "entry3 qualcomm live");
    CHECK(info[4].vendor_id == WEFT_VENDOR_RISCV_ARM && info[4].state == 1,
          "entry4 riscv_arm live on aarch64");
#else
    CHECK(info[2].vendor_id == WEFT_VENDOR_QUALCOMM && info[2].state == 1,
          "entry2 qualcomm live");
    CHECK(info[3].vendor_id == WEFT_VENDOR_RISCV_ARM && info[3].state == 0,
          "entry3 riscv_arm probe-dead (wrong arch, honest)");
    CHECK(info[3].death_reason == WEFT_BACKEND_EREFUSED,
          "riscv_arm death reason EREFUSED");
    CHECK(strncmp(info[3].impl_name, "absent(wrong-arch)", 18) == 0,
          "riscv_arm identity kept after death, got '%s'", info[3].impl_name);
    CHECK(info[4].vendor_id == WEFT_VENDOR_NVIDIA_PC && info[4].state == 1,
          "entry4 nvidia-pc live");
#endif
    CHECK(info[5].vendor_id == WEFT_VENDOR_CPU_SIMD && info[5].state == 1,
          "entry5 terminal cpu live");

    // Identity honesty: the mock-injected rows say "injectable"; the PC row
    // names the SIMD engine it actually fronted.
    CHECK(strncmp(info[0].impl_name, "metal-injectable", 16) == 0,
          "apple honest identity, got '%s'", info[0].impl_name);
#if defined(__aarch64__)
    CHECK(strstr(info[4].impl_name, "arm-neon") != NULL ||
          strstr(info[4].impl_name, "arm-sve2") != NULL,
          "arm row names neon/sve2, got '%s'", info[4].impl_name);
#else
    CHECK(strncmp(info[4].impl_name, "pc-host-vector:", 15) == 0,
          "nvidia_pc host-vector identity, got '%s'", info[4].impl_name);
#endif
    CHECK(strstr(info[5].impl_name, "avx512") != NULL ||
              strstr(info[5].impl_name, "avx2") != NULL ||
              strstr(info[5].impl_name, "neon") != NULL ||
              strstr(info[5].impl_name, "sve2") != NULL ||
              strstr(info[5].impl_name, "rvv") != NULL ||
              strstr(info[5].impl_name, "scalar") != NULL,
          "terminal names its SIMD impl, got '%s'", info[5].impl_name);

    CHECK(weft_backend_heap_locked() == 1, "Law 1: heap lock engaged");
    destroy_ctx(ctx, 0xF);
    CHECK(weft_backend_heap_locked() == 0, "heap lock released on destroy");
}

// ---------------------------------------------------------------------------
// End-to-end DEVICE dispatch: zero-copy correctness through the NPU row
// ---------------------------------------------------------------------------

static void test_device_dispatch(void) {
    weft_backend_ctx_t* ctx = make_ctx(0xF, 0);

    float* src = (float*)g_pool;
    float* dst = (float*)(g_pool + 16384);
    float* ref = (float*)g_ref;
    const uint32_t n = 4096;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t u = i * 2654435761u + 12345u;
        float f;
        memcpy(&f, &u, 4);
        src[i] = f;
    }
    memset(dst, 0, 16384);

    weft_op_desc_t op;
    memset(&op, 0, sizeof(op));
    op.kind = WEFT_OP_NORMALIZE_F32;
    op.m = n;
    op.f0 = -1.0f;
    op.f1 = 1.0f / 4096.0f;
    op.bufs[0] = mk_buf(src, 16384, WEFT_BACKEND_DTYPE_F32);
    op.bufs[2] = mk_buf(dst, 16384, WEFT_BACKEND_DTYPE_F32);

    weft_dispatch_result_t res;
    memset(&res, 0, sizeof(res));
    const uint64_t heap0 = weft_backend_heap_bytes();
    CHECK(weft_backend_dispatch(ctx, &op, &res) == WEFT_BACKEND_OK, "dispatch");
    CHECK(res.engine_class == WEFT_ENGINE_NPU, "NPU row executed");
    CHECK(res.backend_index == 0, "apple (top NPU) was first");
    CHECK(res.fallback_hops == 0, "no hops on the happy path");
    CHECK(res.device_ns > 0, "device time honestly non-zero (DMA model)");

    weft_simd_scalar_normalize(ref, src, n, -1.0f, 1.0f / 4096.0f);
    CHECK(memcmp(dst, ref, 16384) == 0,
          "DEVICE path bit-exact into the caller's buffer (zero-copy)");

    weft_backend_stats_t st;
    weft_backend_stats(ctx, &st);
    CHECK(st.dispatches == 1 && st.fallback_hops == 0, "stats: 1 dispatch");
    CHECK(weft_backend_heap_bytes() == heap0, "Law 1: heap untouched");

    // ---- DOT through the device (aux operand) ---------------------------
    float* a = (float*)(g_pool + 32768);
    float* b = (float*)(g_pool + 40960);
    float* c = (float*)(g_pool + 49152);
    float* cref = (float*)(g_ref + 16384);
    for (int i = 0; i < 128; i++) {
        uint32_t u = (uint32_t)(i * 97 + 3);
        float f;
        memcpy(&f, &u, 4);
        a[i] = f * 0.5f;
        b[i] = f * 0.25f;
    }
    memset(&op, 0, sizeof(op));
    op.kind = WEFT_OP_DOT_F32;
    op.m = 8; op.k = 8; op.n = 8;
    op.lda = 8; op.ldb = 8; op.ldc = 8;
    op.bufs[0] = mk_buf(a, 256, WEFT_BACKEND_DTYPE_F32);
    op.bufs[1] = mk_buf(b, 256, WEFT_BACKEND_DTYPE_F32);
    op.bufs[2] = mk_buf(c, 256, WEFT_BACKEND_DTYPE_F32);
    memset(&res, 0, sizeof(res));
    CHECK(weft_backend_dispatch(ctx, &op, &res) == WEFT_BACKEND_OK, "dot dispatch");
    weft_simd_scalar_dot_f32(cref, a, b, 8, 8, 8, 8, 8, 8);
    CHECK(memcmp(c, cref, 256) == 0, "DEVICE dot bit-exact");

    // ---- 10k mixed dispatches: heap witness ------------------------------
    for (int i = 0; i < 10000; i++) {
        op.kind = (i & 1) ? WEFT_OP_NORMALIZE_F32 : WEFT_OP_DOT_F32;
        memset(&res, 0, sizeof(res));
        CHECK(weft_backend_dispatch(ctx, &op, &res) == WEFT_BACKEND_OK,
              "mixed dispatch %d", i);
    }
    CHECK(weft_backend_heap_bytes() == heap0, "Law 1: 10k dispatches, 0 heap");

    destroy_ctx(ctx, 0xF);
}

// ---------------------------------------------------------------------------
// Law 3: fallback hops (checksum -> CPU) + EDEVICE hot-unplug death
// ---------------------------------------------------------------------------

static void test_fallback(void) {
    weft_backend_ctx_t* ctx = make_ctx(0xF, 0);

    // Checksums: NO device row claims them — the walk lands on the PC
    // host-vector row, which refuses ENOTSUP at runtime (its probe-time
    // affinity was set for HOST_VECTOR mode; the mock puts it in DEVICE
    // mode), then the terminal CPU engine executes. Honest hop counting.
    static unsigned char cbuf[128] __attribute__((aligned(64)));
    for (int i = 0; i < 128; i++) {
        cbuf[i] = (unsigned char)(i * 31 + 7);
    }
    weft_op_desc_t op;
    memset(&op, 0, sizeof(op));
    op.kind = WEFT_OP_SEQLOCK_CHECKSUM;
    op.u0 = 42;
    op.bufs[0] = mk_buf(cbuf, 128, WEFT_BACKEND_DTYPE_U32);

    weft_dispatch_result_t res;
    memset(&res, 0, sizeof(res));
    CHECK(weft_backend_dispatch(ctx, &op, &res) == WEFT_BACKEND_OK, "checksum");
    CHECK(res.result_u64 ==
          (uint64_t)weft_simd_scalar_seqlock_checksum(cbuf, 128, 42),
          "checksum digest == oracle");
    CHECK(res.engine_class == WEFT_ENGINE_CPU_VECTOR, "CPU row executed");
    weft_backend_stats_t st;
#if defined(__x86_64__) || defined(_M_X64)
    CHECK(res.fallback_hops >= 1, "at least one honest hop, got %u",
          res.fallback_hops);
    weft_backend_stats(ctx, &st);
    CHECK(st.fallback_hops >= 1 && st.refusals >= 1, "hops/refusals counted");
#else
    CHECK(res.fallback_hops == 0, "direct CPU vector row execution, got %u",
          res.fallback_hops);
#endif
    destroy_ctx(ctx, 0xF);

    // ---- EDEVICE: hot-unplug kills the engine deterministically ----------
    // Mediatek ONLY injected: entries are mediatek(NPU live),
    // apple/qualcomm(dead-init), nvidia(host-vector live), riscv(dead-probe),
    // cpu(live) — so the unplug must hop past mediatek to the host rows.
    weft_mock_dma_cfg_t cfg = g_dev_cfgs[1];   // mediatek device model
    weft_dma_transport_t* t = weft_mock_dma_new(&cfg);
    weft_backend_cfg_t bc;
    memset(&bc, 0, sizeof(bc));
    bc.transport_overrides[WEFT_VENDOR_MEDIATEK - 1] = t;
    weft_backend_ctx_t* ctx3 = weft_backend_ctx_create(&bc);
    CHECK(ctx3 != NULL, "ctx3");

    static unsigned char ubuf[256] __attribute__((aligned(64)));
    for (int i = 0; i < 256; i++) {
        ubuf[i] = (unsigned char)(i * 13 + 5);
    }
    weft_op_desc_t uop;
    memset(&uop, 0, sizeof(uop));
    uop.kind = WEFT_OP_DELTA_ENCODE_U32;
    uop.m = 64;
    uop.u0 = 9;
    uop.bufs[0] = mk_buf(ubuf, 256, WEFT_BACKEND_DTYPE_U32);
    static unsigned char ubuf2[256] __attribute__((aligned(64)));
    uop.bufs[2] = mk_buf(ubuf2, 256, WEFT_BACKEND_DTYPE_U32);

    weft_mock_dma_inject_device_gone(t);
    memset(&res, 0, sizeof(res));
    CHECK(weft_backend_dispatch(ctx3, &uop, &res) == WEFT_BACKEND_OK,
          "dispatch COMPLETES after hot-unplug (Law 3)");
    CHECK(res.engine_class == WEFT_ENGINE_CPU_VECTOR,
          "completed on the CPU vector row");
    CHECK(res.fallback_hops == 1, "one hop past the dead engine");
    weft_backend_stats(ctx3, &st);
    CHECK(st.device_gone == 1, "device_gone counted");

    // The dead entry stays visible with its reason (scan by vendor — the
    // apple row sorts above mediatek even while dead-init).
    weft_backend_info_t info[WEFT_BACKEND_MAX_BACKENDS];
    const uint32_t nn = weft_backend_table_info(ctx3, info, WEFT_BACKEND_MAX_BACKENDS);
    int mtk_seen = 0;
    for (uint32_t i = 0; i < nn; i++) {
        if (info[i].vendor_id == WEFT_VENDOR_MEDIATEK) {
            mtk_seen = 1;
            CHECK(info[i].state == -1, "mediatek dead-after-init (runtime death)");
            CHECK(info[i].death_reason == WEFT_BACKEND_EDEVICE,
                  "death reason EDEVICE");
        }
    }
    CHECK(mtk_seen, "mediatek entry visible");

    // Second dispatch: dead engine skipped entirely (no repeated death).
    memset(&res, 0, sizeof(res));
    CHECK(weft_backend_dispatch(ctx3, &uop, &res) == WEFT_BACKEND_OK, "next dispatch");
    CHECK(res.fallback_hops == 0, "no hop after the death was recorded");

    weft_backend_ctx_destroy(ctx3);
    weft_mock_dma_destroy(t);
}

// ---------------------------------------------------------------------------
// Law 4: transient EBUSY propagates (no silent reroute); the Law-2 breaker
// is caught structurally (ESTATE); argument law is fail-closed (EINVAL)
// ---------------------------------------------------------------------------

static void test_error_ledger(void) {
    // EBUSY propagation: tiny capacity on the top NPU row.
    weft_mock_dma_cfg_t cfg = {
        .name = "saturated-ane", .fixed_ns = 400000, .ps_per_byte = 30,
        .capacity_bytes = 1024,
    };
    weft_dma_transport_t* t = weft_mock_dma_new(&cfg);
    weft_backend_cfg_t bc;
    memset(&bc, 0, sizeof(bc));
    bc.transport_overrides[WEFT_VENDOR_APPLE - 1] = t;
    weft_backend_ctx_t* ctx = weft_backend_ctx_create(&bc);
    CHECK(ctx != NULL, "ctx");

    static unsigned char dbuf[8192] __attribute__((aligned(64)));
    memset(dbuf, 0xAB, sizeof(dbuf));
    weft_op_desc_t op;
    memset(&op, 0, sizeof(op));
    op.kind = WEFT_OP_NORMALIZE_F32;
    op.m = 2048;
    op.f0 = 0.0f;
    op.f1 = 0.5f;
    op.bufs[0] = mk_buf(dbuf, 8192, WEFT_BACKEND_DTYPE_F32);

    static unsigned char dbuf2[8192] __attribute__((aligned(64)));
    memset(dbuf2, 0xAB, sizeof(dbuf2));
    op.bufs[2] = mk_buf(dbuf2, 8192, WEFT_BACKEND_DTYPE_F32);

    weft_dispatch_result_t res;
    memset(&res, 0, sizeof(res));
    const weft_backend_status_t st =
        weft_backend_dispatch(ctx, &op, &res);
    CHECK(st == WEFT_BACKEND_EBUSY,
          "saturated bus propagates EBUSY (got %d)", (int)st);
    weft_backend_stats_t stats;
    weft_backend_stats(ctx, &stats);
    CHECK(stats.busy_propagated == 1, "busy_propagated counted");
    CHECK(stats.dispatches == 0, "NOT silently dispatched elsewhere");
    for (int i = 0; i < 8192; i++) {
        CHECK(dbuf2[i] == 0xAB, "dst untouched on EBUSY (i=%d)", i);
        if (dbuf2[i] != 0xAB) break;
    }
    weft_backend_ctx_destroy(ctx);
    weft_mock_dma_destroy(t);

    // Law-2 breaker: the driver's structural check must refuse it.
    weft_mock_dma_cfg_t cfg2 = g_dev_cfgs[1];
    weft_dma_transport_t* t2 = weft_mock_dma_new(&cfg2);
    memset(&bc, 0, sizeof(bc));
    bc.transport_overrides[WEFT_VENDOR_MEDIATEK - 1] = t2;
    weft_backend_ctx_t* ctx2 = weft_backend_ctx_create(&bc);
    CHECK(ctx2 != NULL, "ctx2");
    weft_mock_dma_break_zero_copy(t2);
    static unsigned char bbuf[256] __attribute__((aligned(64)));
    memset(&op, 0, sizeof(op));
    op.kind = WEFT_OP_DELTA_ENCODE_U32;
    op.m = 64;
    op.u0 = 0;
    op.bufs[0] = mk_buf(bbuf, 256, WEFT_BACKEND_DTYPE_U32);
    static unsigned char bbuf2[256] __attribute__((aligned(64)));
    op.bufs[2] = mk_buf(bbuf2, 256, WEFT_BACKEND_DTYPE_U32);
    memset(&res, 0, sizeof(res));
    CHECK(weft_backend_dispatch(ctx2, &op, &res) == WEFT_BACKEND_ESTATE,
          "Law-2 breaker caught structurally (ESTATE)");
    weft_backend_ctx_destroy(ctx2);
    weft_mock_dma_destroy(t2);

    // Argument law: NULL/unknown/malformed -> EINVAL, zero side effects.
    weft_backend_ctx_t* ctx3 = make_ctx(0, 0);
    weft_op_desc_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.kind = 99;   // unknown kind: never walks
    bad.bufs[0] = mk_buf(bbuf, 256, WEFT_BACKEND_DTYPE_U32);
    CHECK(weft_backend_dispatch(ctx3, &bad, &res) == WEFT_BACKEND_EINVAL,
          "unknown kind EINVAL");
    CHECK(weft_backend_dispatch(NULL, &bad, &res) == WEFT_BACKEND_EINVAL,
          "NULL ctx EINVAL");
    CHECK(weft_backend_dispatch(ctx3, NULL, &res) == WEFT_BACKEND_EINVAL,
          "NULL op EINVAL");
    memset(&bad, 0, sizeof(bad));
    bad.kind = WEFT_OP_NORMALIZE_F32;
    bad.m = 0;   // zero dims
    bad.bufs[0] = mk_buf(bbuf, 256, WEFT_BACKEND_DTYPE_F32);
    bad.bufs[2] = mk_buf(bbuf2, 256, WEFT_BACKEND_DTYPE_F32);
    CHECK(weft_backend_dispatch(ctx3, &bad, &res) == WEFT_BACKEND_EINVAL,
          "m=0 EINVAL");
    bad.m = 1024;   // exceeds buffer bytes
    CHECK(weft_backend_dispatch(ctx3, &bad, &res) == WEFT_BACKEND_ERANGE,
          "size law ERANGE");
    destroy_ctx(ctx3, 0);
}

// ---------------------------------------------------------------------------
// The < 1 us fallback hop: steady-state dispatch latency through the
// refusal walk to the terminal CPU engine (Law 3's headline number)
// ---------------------------------------------------------------------------

static void test_fallback_latency(void) {
    weft_backend_ctx_t* ctx = make_ctx(0xF, 0);
    static unsigned char tiny[8] __attribute__((aligned(64)));
    weft_op_desc_t op;
    memset(&op, 0, sizeof(op));
    op.kind = WEFT_OP_SEQLOCK_CHECKSUM;
    op.u0 = 1;
    op.bufs[0] = mk_buf(tiny, 8, WEFT_BACKEND_DTYPE_U32);

    weft_dispatch_result_t res;
    memset(&res, 0, sizeof(res));
    // Warmup (branch predictors, cache lines, log suppression window).
    for (int i = 0; i < 10000; i++) {
        (void)weft_backend_dispatch(ctx, &op, &res);
    }
    // Steady state: 200k timed dispatches; median + p99.
    enum { N = 200000 };
    static uint64_t samples[N];
    for (int i = 0; i < N; i++) {
        const uint64_t t0 = weft_backend_now_ns();
        (void)weft_backend_dispatch(ctx, &op, &res);
        samples[i] = weft_backend_now_ns() - t0;
    }
    // Insertion sort would be O(n^2) — counting sort over ns buckets (the
    // histogram is pre-allocated; Law 1 spirit even in the test battery).
    enum { BUCKETS = 8192 };
    static uint32_t hist[BUCKETS];
    memset(hist, 0, sizeof(hist));
    uint32_t over = 0;
    for (int i = 0; i < N; i++) {
        if (samples[i] < BUCKETS) {
            hist[samples[i]]++;
        } else {
            over++;
        }
    }
    uint32_t seen = 0;
    uint64_t p50 = 0, p99 = 0;
    for (uint32_t b = 0; b < BUCKETS; b++) {
        seen += hist[b];
        if (p50 == 0 && seen >= N / 2) {
            p50 = b;
        }
        if (p99 == 0 && seen >= (uint32_t)(N * 99) / 100) {
            p99 = b;
        }
    }
    printf("fallback-hop steady state: p50=%" PRIu64 "ns p99=%" PRIu64
           "ns (over-%d bucket count: %u)\n",
           p50, p99, BUCKETS, over);
    CHECK(p50 < 1000, "Law 3: p50 fallback dispatch < 1000ns (got %" PRIu64 ")",
          p50);
    destroy_ctx(ctx, 0xF);
}

int main(void) {
    printf("step 1: test_table_order\n"); fflush(stdout);
    test_table_order();
    printf("step 2: test_device_dispatch\n"); fflush(stdout);
    test_device_dispatch();
    printf("step 3: test_fallback\n"); fflush(stdout);
    test_fallback();
    printf("step 4: test_error_ledger\n"); fflush(stdout);
    test_error_ledger();
    printf("step 5: test_fallback_latency\n"); fflush(stdout);
    test_fallback_latency();
    if (g_failures != 0) {
        printf("spectrum-dispatch: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("spectrum-dispatch: lifecycle+dispatch semantics PASS\n");
    return 0;
}
