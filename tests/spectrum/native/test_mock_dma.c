// test_mock_dma.c — synthetic transport conformance (Pillar 5, D-52).
//
// Verifies the device model MECHANICS the dispatch/torture batteries
// build on: map/unmap laws, the latency formula, saturation backpressure,
// register-poll semantics, zero-copy device compute, and both fault
// injections. The end-to-end DRIVER-path proofs live in
// test_spectrum_dispatch.c; this file pins the transport itself.

#include "../mock/weft_mock_dma.h"
#include "../../../core/c/spectrum/drivers/weft_backend.h"
#include "../../../core/c/spectrum/simd/weft_simd.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

static uint64_t mono_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ns(uint64_t ns) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)ns };
    (void)nanosleep(&ts, NULL);
}

// ---------------------------------------------------------------------------
// map / unmap laws
// ---------------------------------------------------------------------------

static void test_map_unmap(void) {
    weft_mock_dma_cfg_t cfg = {
        .name = "conformance", .fixed_ns = 1000, .ps_per_byte = 50,
        .capacity_bytes = 1u << 20,
    };
    weft_dma_transport_t* t = weft_mock_dma_new(&cfg);
    CHECK(t != NULL, "create");

    static unsigned char buf[4096] __attribute__((aligned(64)));
    weft_buffer_desc_t d;
    memset(&d, 0, sizeof(d));
    d.data = buf;
    d.bytes = sizeof(buf);
    d.dtype = WEFT_BACKEND_DTYPE_U32;
    d.flags = WEFT_BUF_HOST | WEFT_BUF_WRITABLE;

    CHECK(t->map(t->transport_ctx, &d) == WEFT_BACKEND_OK, "map");
    CHECK(d.data == (void*)buf, "LAW 2: host pointer untouched by map()");
    CHECK(d.dma_tag == 1, "first tag is slot+1");
    CHECK((d.flags & WEFT_BUF_DEVICE) != 0, "DEVICE flag set");
    CHECK((d.flags & WEFT_BUF_UNIFIED) != 0, "UNIFIED flag set (UMA alias)");
    CHECK(weft_mock_dma_would_copies(t) == 0, "no copies on the honest path");

    // bounded table: 255 more maps fit, the 257th refuses EBUSY
    weft_buffer_desc_t extra[256];
    memset(extra, 0, sizeof(extra));
    for (int i = 0; i < 256; i++) {
        extra[i].data = buf;
        extra[i].bytes = 16;
        extra[i].dtype = WEFT_BACKEND_DTYPE_U32;
    }
    int ok = 0, busy = 0;
    for (int i = 0; i < 256; i++) {
        weft_backend_status_t st = t->map(t->transport_ctx, &extra[i]);
        if (st == WEFT_BACKEND_OK) ok++;
        if (st == WEFT_BACKEND_EBUSY) busy++;
    }
    CHECK(ok == 255 && busy == 1, "bounded map table: 255 ok + 1 EBUSY, got %d/%d", ok, busy);

    // unmap laws: double-unmap is EINVAL
    CHECK(t->unmap(t->transport_ctx, &d) == WEFT_BACKEND_OK, "unmap");
    CHECK(t->unmap(t->transport_ctx, &d) == WEFT_BACKEND_EINVAL, "double unmap EINVAL");

    weft_mock_dma_destroy(t);
}

// ---------------------------------------------------------------------------
// Latency model + register polling + zero-copy device compute
// ---------------------------------------------------------------------------

static void test_latency_and_compute(void) {
    weft_mock_dma_cfg_t cfg = {
        .name = "latmodel", .fixed_ns = 200000, .ps_per_byte = 500,
        .capacity_bytes = 1u << 20,
    };
    weft_dma_transport_t* t = weft_mock_dma_new(&cfg);

    static float src[512] __attribute__((aligned(64)));
    static float dst[512] __attribute__((aligned(64)));
    static float ref[512] __attribute__((aligned(64)));
    for (int i = 0; i < 512; i++) {
        uint32_t u = (uint32_t)(i * 2654435761u);
        float f;
        memcpy(&f, &u, 4);
        src[i] = f;
    }
    memset(dst, 0, sizeof(dst));

    weft_buffer_desc_t s, d2;
    memset(&s, 0, sizeof(s));
    memset(&d2, 0, sizeof(d2));
    s.data = src; s.bytes = sizeof(src); s.dtype = WEFT_BACKEND_DTYPE_F32;
    d2.data = dst; d2.bytes = sizeof(dst); d2.dtype = WEFT_BACKEND_DTYPE_F32;
    CHECK(t->map(t->transport_ctx, &s) == WEFT_BACKEND_OK, "map src");
    CHECK(t->map(t->transport_ctx, &d2) == WEFT_BACKEND_OK, "map dst");

    weft_cmd_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op_kind = WEFT_OP_NORMALIZE_F32;
    pkt.in_tag = (uint32_t)s.dma_tag;
    pkt.out_tag = (uint32_t)d2.dma_tag;
    pkt.m = 512;
    pkt.f0 = 0.0f;
    pkt.f1 = 1.0f / 512.0f;

    const uint64_t t0 = mono_ns();
    CHECK(t->enqueue(t->transport_ctx, &pkt, 1) == WEFT_BACKEND_OK, "enqueue");
    CHECK(weft_mock_dma_inflight_bytes(t) == sizeof(src) + sizeof(dst),
          "in-flight bytes counted");

    // Poll BEFORE the deadline: not done (the register has not advanced)
    uint32_t done = 0;
    CHECK(t->poll(t->transport_ctx, &done) == WEFT_BACKEND_OK, "poll");
    CHECK(done == 0, "deadline not cleared yet (fixed_ns=200us)");
    CHECK(weft_mock_dma_completed_pkts(t) == 0, "no completion yet");
    CHECK(weft_mock_dma_poll_count(t) == 1, "polls counted");

    // Deadline = 200us fixed + 4096B * 500ps = ~202us. Wait past it.
    while (mono_ns() - t0 < 250000) {
        sleep_ns(20000);
    }
    CHECK(t->poll(t->transport_ctx, &done) == WEFT_BACKEND_OK, "poll 2");
    CHECK(done == 1, "completion register advanced past deadline");
    CHECK(weft_mock_dma_completed_pkts(t) == 1, "one completion");
    CHECK(weft_mock_dma_zero_copy_completions(t) == 1,
          "compute executed through the ALIASED pointer");
    CHECK(weft_mock_dma_inflight_bytes(t) == 0, "tokens refunded on completion");

    // The device compute is the scalar oracle — bit-exact by construction
    weft_simd_scalar_normalize(ref, src, 512, 0.0f, 1.0f / 512.0f);
    CHECK(memcmp(dst, ref, sizeof(dst)) == 0,
          "device compute == normative oracle (zero-copy into caller buffer)");
    CHECK(weft_mock_dma_would_copies(t) == 0, "still zero copies");

    weft_mock_dma_destroy(t);
}

// ---------------------------------------------------------------------------
// Saturation: all-or-nothing burst admission + token refund
// ---------------------------------------------------------------------------

static void test_saturation(void) {
    weft_mock_dma_cfg_t cfg = {
        .name = "saturated", .fixed_ns = 1000, .ps_per_byte = 10,
        .capacity_bytes = 8192,   // exactly ONE 4096B op (in+out) in flight
    };
    weft_dma_transport_t* t = weft_mock_dma_new(&cfg);

    static unsigned char bufs[4][4096] __attribute__((aligned(64)));
    weft_buffer_desc_t desc[4];
    memset(desc, 0, sizeof(desc));
    for (int i = 0; i < 4; i++) {
        desc[i].data = bufs[i];
        desc[i].bytes = 4096;
        desc[i].dtype = WEFT_BACKEND_DTYPE_U32;
        CHECK(t->map(t->transport_ctx, &desc[i]) == WEFT_BACKEND_OK, "map %d", i);
    }

    weft_cmd_pkt_t pkts[4];
    memset(pkts, 0, sizeof(pkts));
    for (int i = 0; i < 4; i++) {
        pkts[i].op_kind = WEFT_OP_DELTA_ENCODE_U32;
        pkts[i].in_tag = (uint32_t)desc[i].dma_tag;
        pkts[i].out_tag = (uint32_t)desc[i].dma_tag;   // inplace
        pkts[i].m = 1024;
        pkts[i].u0 = 7;
    }
    // 2 x (in 4096 + out 4096) = 16384 > 8192 capacity: EBUSY, and NOTHING
    // was enqueued (all-or-nothing admission).
    CHECK(t->enqueue(t->transport_ctx, pkts, 2) == WEFT_BACKEND_EBUSY,
          "burst exceeds capacity -> honest EBUSY");
    CHECK(weft_mock_dma_enqueued_pkts(t) == 0, "all-or-nothing: zero enqueued");

    // One op alone (8192 bytes) exactly fits the window.
    CHECK(t->enqueue(t->transport_ctx, pkts, 1) == WEFT_BACKEND_OK,
          "single op admitted at capacity boundary");
    CHECK(weft_mock_dma_inflight_bytes(t) == 8192, "8192B in flight");

    // While the window is full, the next burst is refused.
    CHECK(t->enqueue(t->transport_ctx, pkts, 1) == WEFT_BACKEND_EBUSY,
          "window full -> EBUSY backpressure");

    // Drain: wait past the deadline, tokens refund, admission reopens.
    sleep_ns(8000);
    uint32_t done = 0;
    CHECK(t->poll(t->transport_ctx, &done) == WEFT_BACKEND_OK, "drain poll");
    CHECK(done == 1, "drained");
    CHECK(weft_mock_dma_inflight_bytes(t) == 0, "tokens refunded");
    CHECK(t->enqueue(t->transport_ctx, pkts, 1) == WEFT_BACKEND_OK,
          "after refund, submission admitted");
    weft_mock_dma_destroy(t);
}

// ---------------------------------------------------------------------------
// Fault injections
// ---------------------------------------------------------------------------

static void test_faults(void) {
    weft_mock_dma_cfg_t cfg = {
        .name = "faults", .fixed_ns = 100, .ps_per_byte = 1,
        .capacity_bytes = 1u << 20,
    };
    weft_dma_transport_t* t = weft_mock_dma_new(&cfg);

    static unsigned char buf[128] __attribute__((aligned(64)));
    weft_buffer_desc_t d;
    memset(&d, 0, sizeof(d));
    d.data = buf; d.bytes = sizeof(buf); d.dtype = WEFT_BACKEND_DTYPE_U32;
    CHECK(t->map(t->transport_ctx, &d) == WEFT_BACKEND_OK, "map");

    weft_cmd_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op_kind = WEFT_OP_DELTA_ENCODE_U32;
    pkt.in_tag = (uint32_t)d.dma_tag;
    pkt.out_tag = (uint32_t)d.dma_tag;
    pkt.m = 32;

    // Hot-unplug: enqueue AND poll both answer EDEVICE.
    weft_mock_dma_inject_device_gone(t);
    CHECK(t->enqueue(t->transport_ctx, &pkt, 1) == WEFT_BACKEND_EDEVICE,
          "enqueue after unplug -> EDEVICE");
    uint32_t done = 0;
    CHECK(t->poll(t->transport_ctx, &done) == WEFT_BACKEND_EDEVICE,
          "poll after unplug -> EDEVICE");

    // Law-2 breaker: the injected staging copy is COUNTED and visible.
    weft_buffer_desc_t d2;
    memset(&d2, 0, sizeof(d2));
    d2.data = buf; d2.bytes = sizeof(buf); d2.dtype = WEFT_BACKEND_DTYPE_U32;
    weft_mock_dma_break_zero_copy(t);
    CHECK(t->map(t->transport_ctx, &d2) == WEFT_BACKEND_OK, "broken map runs");
    CHECK(d2.data != (void*)buf, "injected breaker moves the pointer");
    CHECK(weft_mock_dma_would_copies(t) == 1, "the would-be copy is counted");
    CHECK(t->unmap(t->transport_ctx, &d2) == WEFT_BACKEND_OK, "unmap broken");

    weft_mock_dma_destroy(t);
}

int main(void) {
    test_map_unmap();
    test_latency_and_compute();
    test_saturation();
    test_faults();
    if (g_failures != 0) {
        printf("mock-dma: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("mock-dma: transport conformance PASS\n");
    return 0;
}
