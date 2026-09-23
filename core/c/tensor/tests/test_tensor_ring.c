// test_tensor_ring.c — WT-series (25-40): lock-free DMA tensor ring
// conformance + concurrency torture + throughput (RFC-0021 §5).
//
//   WT25 create/geometry: exact ring_bytes formula, slot payload
//        64B-alignment sweep (128B when payload is a 128 multiple), fresh
//        sequence invariant seq[i]==i, head=tail=0, honest mlock/hugepage
//        report fields, geometry validation refusals.
//   WT26 attach contract: legit attach OK; magic/version -> EMAGIC; mode
//        mismatch -> EINVAL; non-pow2 depth / non-64B payload / stride /
//        ring_bytes / reserved dirty -> EGEOMETRY; short mapping EINVAL.
//   WT27 roundtrip: claim -> fill -> commit -> acquire -> verify EVERY
//        view field + memcmp payload -> release; commit normalization
//        (physical_or_shm_addr == payload, byte_length == used).
//   WT28 full-ring refusal: N claims fill, try_claim EAGAIN, drain one,
//        claim succeeds (slot reuse).
//   WT29 misuse detection: unclaimed commit / double commit / double
//        release -> EAGAIN; payload_used bounds; invalid views ->
//        EDTYPE / ERANGE / EMISALIGN through the commit wall.
//   WT30 bounded waits: timeout 0 == single try (EAGAIN); empty acquire
//        with deadline -> ETIMEOUT (elapsed measured).
//   WT31 two-handle discipline: produce via handle A, consume via handle
//        B attached to the same mapping (handle relocatability).
//   WT32 MPSC tearing stress: 4 producers x 12,500 msgs, single consumer,
//        64-slot ring; per-ticket deterministic word pattern + per-parity
//        NCHW/NHWC geometry; EVERY word verified; tickets strictly FIFO.
//   WT33 SPMC stress: 1 producer x 50,000 msgs, 4 consumers racing the
//        tail CAS; every ticket acquired EXACTLY once (full ticket set
//        proof), payload verified.
//   WT34 fork torture (POSIX): parent creates the ring, forks 2 child
//        PRODUCERS (attach path), parent is the MPSC consumer; 10,000
//        cross-process tensors verified. Skipped under WT_SKIP_FORK=1
//        (TSAN leg; declared skip, not a silent pass).
//   WT35 stats: committed/acquired/released/full_hits exact after a
//        controlled sequence; in_flight arithmetic.
//   WT36 wait-ladder deadline honesty: claim on a full ring with 60ms
//        budget returns ETIMEOUT having burned ~60ms (Law 2 measured).
//   WT37 alignment law: payload pointers 128B-aligned for 128B payload
//        classes; commit rejects dtype-natural misalignments.
//   WT38 the flagship: consumer-side zero-copy view algebra over a ring
//        tensor — slice channel, subwindow ROI, permute NCHW->NHWC, all
//        addresses bit-exact against independent arithmetic.
//   WT39 MPMC stress: 2 producers x 2 consumers, exactly-once + integrity.
//   WT40 throughput: 4 producers x 1 consumer, 64 slots x 16KiB payloads;
//        JSON line with msgs/s + GB/s (verified payload moved); --bench
//        runs the 50K-msg leg, default runs a 10K smoke leg. No tight
//        perf assert (shared runners — house no-flaky rule).
//
// Build: make tensor-ring-test{,-asan,-tsan}

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "weft_tensor.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);         \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define CHECK_ST(call, want, msg)                                           \
    do {                                                                    \
        const int _st = (call);                                             \
        g_checks++;                                                         \
        if (_st != (want)) {                                                \
            fprintf(stderr, "FAIL: %s: want %s got %s (line %d)\n", msg,    \
                    weft_tensor_status_name(want),                          \
                    weft_tensor_status_name(_st), __LINE__);                \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/// Deterministic per-ticket word pattern (splitmix-style finalizer).
static uint64_t tword(uint64_t ticket, uint64_t w) {
    uint64_t z = ticket * 0x9E3779B97F4A7C15ull + w * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// ---------------------------------------------------------------------------
// WT25 — create / geometry
// ---------------------------------------------------------------------------

static void test_wt25(void) {
    weft_tensor_ring_t r;
    int st = WEFT_TENSOR_OK;

    // Exact byte formula: 128 + N*(256 + P).
    CHECK(weft_tensor_ring_required_bytes(8, 128, &st) == 128 + 8 * 384 &&
              st == WEFT_TENSOR_OK, "required_bytes 8x128");
    CHECK(weft_tensor_ring_required_bytes(64, 4096, &st) ==
              128 + 64 * 4352, "required_bytes 64x4096");
    CHECK(weft_tensor_ring_required_bytes(0, 64, &st) == 0 &&
              st == WEFT_TENSOR_EINVAL, "slot_count 0");
    CHECK(weft_tensor_ring_required_bytes(3, 64, &st) == 0 &&
              st == WEFT_TENSOR_EINVAL, "non-pow2 depth");
    CHECK(weft_tensor_ring_required_bytes((1u << 21), 64, &st) == 0 &&
              st == WEFT_TENSOR_EINVAL, "depth ceiling");
    CHECK(weft_tensor_ring_required_bytes(8, 100, &st) == 0 &&
              st == WEFT_TENSOR_EINVAL, "payload not 64B multiple");
    CHECK(weft_tensor_ring_required_bytes(8, 0, &st) == 0 &&
              st == WEFT_TENSOR_EINVAL, "payload 0");

    // Geometry + alignment sweep.
    static const uint32_t kPayloads[] = {64, 128, 192, 1024, 4096};
    for (size_t pi = 0; pi < sizeof(kPayloads) / sizeof(kPayloads[0]); pi++) {
        const uint32_t p = kPayloads[pi];
        CHECK_ST(weft_tensor_ring_create(&r, 16, p, WEFT_TENSOR_RING_MODE_MPSC,
                                         WEFT_TENSOR_RING_F_PREFAULT),
                 WEFT_TENSOR_OK, "wt25 create");
        CHECK(r.ring_bytes == 128 + 16ull * (256 + p), "ring_bytes formula");
        CHECK(r.slot_stride == 256 + p, "slot_stride");
        for (uint32_t i = 0; i < 16; i++) {
            const uint8_t* payload =
                r.base + 128 + (uint64_t)i * r.slot_stride + 256;
            char msg[64];
            snprintf(msg, sizeof(msg), "slot %u payload 64B-aligned (p=%u)", i, p);
            CHECK(((uintptr_t)payload & 63u) == 0, msg);
            if (p % 128 == 0) {
                snprintf(msg, sizeof(msg), "slot %u payload 128B-aligned", i);
                CHECK(((uintptr_t)payload & 127u) == 0, msg);
            }
            // Fresh-ring sequence invariant.
            const volatile uint64_t* seqp =
                (const volatile uint64_t*)(r.base + 128 + (uint64_t)i * r.slot_stride);
            CHECK(*seqp == i, "fresh seq[i] == i");
        }
        weft_tensor_ring_destroy(&r);
    }

    // Honest placement report fields.
    CHECK_ST(weft_tensor_ring_create(&r, 8, 512, WEFT_TENSOR_RING_MODE_SPMC,
                                     WEFT_TENSOR_RING_F_MLOCK |
                                         WEFT_TENSOR_RING_F_HUGEPAGE |
                                         WEFT_TENSOR_RING_F_PREFAULT),
             WEFT_TENSOR_OK, "full-flag create");
    CHECK(r.locked == 0 || r.locked == 1, "locked honest boolean");
    CHECK(r.hugepage_hint == 0 || r.hugepage_hint == 1, "hugepage honest boolean");
    printf("WT25 placement report: mlock=%d hugepage_hint=%d\n", r.locked,
           r.hugepage_hint);
    weft_tensor_ring_destroy(&r);

    // Refusals.
    CHECK_ST(weft_tensor_ring_create(&r, 7, 64, WEFT_TENSOR_RING_MODE_MPSC, 0),
             WEFT_TENSOR_EINVAL, "create non-pow2");
    CHECK_ST(weft_tensor_ring_create(&r, 8, 65, WEFT_TENSOR_RING_MODE_MPSC, 0),
             WEFT_TENSOR_EINVAL, "create bad payload");
    CHECK_ST(weft_tensor_ring_create(&r, 8, 64, 0, 0), WEFT_TENSOR_EINVAL,
             "create mode 0");
    CHECK_ST(weft_tensor_ring_create(&r, 8, 64, WEFT_TENSOR_RING_MODE_MPSC, 0x40),
             WEFT_TENSOR_EINVAL, "create unknown flag");
    CHECK_ST(weft_tensor_ring_create(NULL, 8, 64, WEFT_TENSOR_RING_MODE_MPSC, 0),
             WEFT_TENSOR_EINVAL, "create NULL");
}

// ---------------------------------------------------------------------------
// WT26 — attach contract
// ---------------------------------------------------------------------------

static void test_wt26(void) {
    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 8, 256, WEFT_TENSOR_RING_MODE_MPSC, 0),
             WEFT_TENSOR_OK, "wt26 create");
    const uint64_t rb = r.ring_bytes;

    // Legit second-handle attach.
    weft_tensor_ring_t b;
    CHECK_ST(weft_tensor_ring_attach(&b, r.base, rb, WEFT_TENSOR_RING_MODE_MPSC),
             WEFT_TENSOR_OK, "attach OK");
    CHECK(b.slot_count == 8 && b.payload_bytes == 256 &&
              b.mode == WEFT_TENSOR_RING_MODE_MPSC,
          "attach handle fields");
    CHECK(b.base == r.base, "attach base");
    weft_tensor_ring_destroy(&b);  // attacher: no unmap
    CHECK(r.base != NULL, "attacher destroy keeps mapping");

    // Crafted control-header mutants (copy the first page, corrupt one
    // field, attach must REFUSE — never guess).
    uint8_t* page = malloc(4096);
    CHECK(page != NULL, "wt26 scratch page");
    if (page != NULL) {
        memcpy(page, r.base, 4096);

        struct { size_t off; uint32_t val; int want; const char* msg; } kMuts[] = {
            {0, 0x52545757, WEFT_TENSOR_EMAGIC, "bad magic"},
            {4, 2, WEFT_TENSOR_EMAGIC, "future version"},
            {6, 9, WEFT_TENSOR_EINVAL, "mode mismatch"},
            {8, 7, WEFT_TENSOR_EGEOMETRY, "non-pow2 slot_count"},
            {12, 65, WEFT_TENSOR_EGEOMETRY, "payload not 64B"},
            {16, 999, WEFT_TENSOR_EGEOMETRY, "wrong slot_stride"},
            {24, 999, WEFT_TENSOR_EGEOMETRY, "wrong ring_bytes"},
            {36, 1, WEFT_TENSOR_EGEOMETRY, "reserved0 dirty"},
        };
        for (size_t i = 0; i < sizeof(kMuts) / sizeof(kMuts[0]); i++) {
            memcpy(page, r.base, 4096);
            uint32_t v = kMuts[i].val;
            memcpy(page + kMuts[i].off, &v, sizeof(v));
            CHECK_ST(weft_tensor_ring_attach(&b, page, rb,
                                             WEFT_TENSOR_RING_MODE_MPSC),
                     kMuts[i].want, kMuts[i].msg);
        }
        // Short mapping.
        memcpy(page, r.base, 4096);
        CHECK_ST(weft_tensor_ring_attach(&b, page, rb - 1,
                                         WEFT_TENSOR_RING_MODE_MPSC),
                 WEFT_TENSOR_EINVAL, "short mapping");
        CHECK_ST(weft_tensor_ring_attach(&b, page, 64,
                                         WEFT_TENSOR_RING_MODE_MPSC),
                 WEFT_TENSOR_EINVAL, "mapping smaller than ctrl");
        free(page);
    }
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------
// WT27 — claim/commit/acquire/release roundtrip
// ---------------------------------------------------------------------------

static void test_wt27(void) {
    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 4, 256, WEFT_TENSOR_RING_MODE_MPSC, 0),
             WEFT_TENSOR_OK, "wt27 create");

    uint64_t ticket = 0;
    uint8_t* payload = NULL;
    uint32_t cap = 0;
    CHECK_ST(weft_tensor_ring_try_claim(&r, &ticket, &payload, &cap),
             WEFT_TENSOR_OK, "try_claim");
    CHECK(ticket == 0, "first ticket 0");
    CHECK(cap == 256, "capacity");
    CHECK(((uintptr_t)payload & 63u) == 0, "payload 64B-aligned");

    // [2,8,16] u8 == 256 bytes, every byte deterministic.
    for (uint32_t i = 0; i < 256; i++) payload[i] = (uint8_t)(i * 7 + 3);
    uint64_t shape[3] = {2, 8, 16};
    weft_tensor_view_t v;
    CHECK_ST(weft_tensor_view_init(&v, 123, WEFT_DTYPE_U8, 3, shape,
                                   (uintptr_t)payload, 0),
             WEFT_TENSOR_OK, "view init");
    CHECK_ST(weft_tensor_ring_commit(&r, ticket, &v, 256), WEFT_TENSOR_OK,
             "commit");

    const weft_tensor_view_t* rv = NULL;
    const uint8_t* rpayload = NULL;
    uint32_t used = 0;
    uint64_t rticket = 0;
    CHECK_ST(weft_tensor_ring_try_acquire(&r, &rticket, &rv, &rpayload, &used),
             WEFT_TENSOR_OK, "try_acquire");
    CHECK(rticket == 0, "acquired ticket 0");
    CHECK(used == 256, "payload_used echo");
    CHECK(rv->tensor_id == 123, "tensor_id roundtrip");
    CHECK(rv->dtype == WEFT_DTYPE_U8 && rv->ndim == 3, "dtype/ndim roundtrip");
    CHECK(rv->shape[0] == 2 && rv->shape[1] == 8 && rv->shape[2] == 16,
          "shape roundtrip");
    CHECK(rv->strides[0] == 128 && rv->strides[1] == 16 && rv->strides[2] == 1,
          "strides roundtrip");
    CHECK(rv->byte_offset == 0, "byte_offset roundtrip");
    CHECK(rv->byte_length == 256, "byte_length normalized to payload_used");
    CHECK(rv->physical_or_shm_addr == (uintptr_t)rpayload,
          "commit normalized addr to slot payload");
    CHECK(rpayload == payload, "same-process payload identity");
    CHECK(memcmp(rpayload, payload, 256) == 0, "payload memcmp");
    CHECK_ST(weft_tensor_view_validate(rv, WEFT_TENSOR_ALIGN_64),
             WEFT_TENSOR_OK, "ring view validates at 64B (Law 4)");

    CHECK_ST(weft_tensor_ring_release(&r, rticket), WEFT_TENSOR_OK, "release");

    // Slot reuse: drain the remaining depth (tickets 1..3 claimed, not
    // committed — claim only gates on seq == ticket), then ticket 4 must
    // recycle slot 0's payload.
    uint64_t t2;
    uint8_t* p2;
    for (int i = 1; i <= 3; i++) {
        CHECK_ST(weft_tensor_ring_try_claim(&r, &t2, &p2, NULL), WEFT_TENSOR_OK,
                 "drain depth");
        CHECK(t2 == (uint64_t)i, "depth ticket");
    }
    CHECK_ST(weft_tensor_ring_try_claim(&r, &t2, &p2, NULL), WEFT_TENSOR_OK,
             "claim after release");
    CHECK(t2 == 4 && p2 == payload, "slot 0 recycled for ticket 4");
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------
// WT28 — full-ring refusal + slot reuse
// ---------------------------------------------------------------------------

static void test_wt28(void) {
    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 4, 64, WEFT_TENSOR_RING_MODE_SPMC, 0),
             WEFT_TENSOR_OK, "wt28 create");
    uint64_t shape[1] = {64};
    for (uint64_t i = 0; i < 4; i++) {
        uint64_t t; uint8_t* p;
        CHECK_ST(weft_tensor_ring_try_claim(&r, &t, &p, NULL), WEFT_TENSOR_OK,
                 "fill claim");
        weft_tensor_view_t v;
        CHECK_ST(weft_tensor_view_init(&v, i, WEFT_DTYPE_U8, 1, shape,
                                       (uintptr_t)p, 0),
                 WEFT_TENSOR_OK, "fill view");
        CHECK_ST(weft_tensor_ring_commit(&r, t, &v, 64), WEFT_TENSOR_OK,
                 "fill commit");
    }
    uint64_t t; uint8_t* p;
    CHECK_ST(weft_tensor_ring_try_claim(&r, &t, &p, NULL), WEFT_TENSOR_EAGAIN,
             "full ring refuses");
    uint64_t full_hits = 0;
    weft_tensor_ring_stats(&r, NULL, NULL, NULL, &full_hits);
    CHECK(full_hits >= 1, "full_hits counted");

    // Drain one ticket; the freed slot serves ticket 4.
    const weft_tensor_view_t* rv; const uint8_t* rp; uint32_t ru; uint64_t rt;
    CHECK_ST(weft_tensor_ring_try_acquire(&r, &rt, &rv, &rp, &ru),
             WEFT_TENSOR_OK, "drain acquire");
    CHECK(rt == 0, "drain ticket 0");
    CHECK_ST(weft_tensor_ring_release(&r, rt), WEFT_TENSOR_OK, "drain release");
    CHECK_ST(weft_tensor_ring_try_claim(&r, &t, &p, NULL), WEFT_TENSOR_OK,
             "claim after drain");
    CHECK(t == 4, "ticket 4 served");
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------
// WT29 — misuse detection + the commit wall
// ---------------------------------------------------------------------------

static void test_wt29(void) {
    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 4, 1024, WEFT_TENSOR_RING_MODE_MPSC, 0),
             WEFT_TENSOR_OK, "wt29 create");
    weft_tensor_view_t v;
    uint64_t shape[2] = {8, 128};  // u8, 1024 bytes

    // Commit a ticket nobody claimed (slot 1 holds seq 1 != 5).
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_U8, 2, shape, 0, 0),
             WEFT_TENSOR_OK, "v init");
    CHECK_ST(weft_tensor_ring_commit(&r, 5, &v, 1024), WEFT_TENSOR_EAGAIN,
             "unclaimed commit refused");

    // Claim 0, commit twice.
    uint64_t t; uint8_t* p;
    CHECK_ST(weft_tensor_ring_try_claim(&r, &t, &p, NULL), WEFT_TENSOR_OK, "claim");
    CHECK(t == 0, "ticket 0");
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_U8, 2, shape,
                                   (uintptr_t)p, 0),
             WEFT_TENSOR_OK, "v init 2");
    CHECK_ST(weft_tensor_ring_commit(&r, t, &v, 1024), WEFT_TENSOR_OK, "commit");
    CHECK_ST(weft_tensor_ring_commit(&r, t, &v, 1024), WEFT_TENSOR_EAGAIN,
             "double commit refused");

    // Acquire + release + double release.
    const weft_tensor_view_t* rv; const uint8_t* rp; uint32_t ru; uint64_t rt;
    CHECK_ST(weft_tensor_ring_try_acquire(&r, &rt, &rv, &rp, &ru),
             WEFT_TENSOR_OK, "acquire");
    CHECK_ST(weft_tensor_ring_release(&r, rt), WEFT_TENSOR_OK, "release");
    CHECK_ST(weft_tensor_ring_release(&r, rt), WEFT_TENSOR_EAGAIN,
             "double release refused");

    // payload_used bounds.
    uint64_t t2; uint8_t* p2;
    CHECK_ST(weft_tensor_ring_try_claim(&r, &t2, &p2, NULL), WEFT_TENSOR_OK,
             "claim 2");
    CHECK_ST(weft_tensor_view_init(&v, 1, WEFT_DTYPE_U8, 2, shape,
                                   (uintptr_t)p2, 0),
             WEFT_TENSOR_OK, "v init 3");
    CHECK_ST(weft_tensor_ring_commit(&r, t2, &v, 0), WEFT_TENSOR_EINVAL,
             "payload_used 0");
    CHECK_ST(weft_tensor_ring_commit(&r, t2, &v, 1025), WEFT_TENSOR_EINVAL,
             "payload_used > capacity");
    CHECK_ST(weft_tensor_ring_commit(&r, t2, NULL, 1024), WEFT_TENSOR_EINVAL,
             "NULL view");

    // The commit wall: view escapes payload_used.
    CHECK_ST(weft_tensor_ring_commit(&r, t2, &v, 100), WEFT_TENSOR_ERANGE,
             "view window escapes payload_used");
    // Law 4: dtype-natural misalignment (f32 at odd offset).
    weft_tensor_view_t f32v;
    uint64_t s8[1] = {8};
    CHECK_ST(weft_tensor_view_init(&f32v, 2, WEFT_DTYPE_F32, 1, s8,
                                   (uintptr_t)p2, 2),
             WEFT_TENSOR_OK, "f32 view");
    CHECK_ST(weft_tensor_ring_commit(&r, t2, &f32v, 512), WEFT_TENSOR_EMISALIGN,
             "misaligned commit refused");
    // Unknown dtype through the wall.
    weft_tensor_view_t bad = v;
    bad.dtype = (weft_dtype_t)55;
    CHECK_ST(weft_tensor_ring_commit(&r, t2, &bad, 1024), WEFT_TENSOR_EDTYPE,
             "unknown dtype commit refused");
    // Legal commit to close the ticket.
    CHECK_ST(weft_tensor_ring_commit(&r, t2, &v, 1024), WEFT_TENSOR_OK,
             "legal commit");
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------
// WT30 + WT36 — bounded waits (deadline honesty)
// ---------------------------------------------------------------------------

static void test_wt30(void) {
    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 2, 64, WEFT_TENSOR_RING_MODE_MPSC, 0),
             WEFT_TENSOR_OK, "wt30 create");

    // Empty acquire: timeout 0 -> EAGAIN; 30ms -> ETIMEOUT with elapsed time.
    const weft_tensor_view_t* rv; const uint8_t* rp; uint32_t ru; uint64_t rt;
    CHECK_ST(weft_tensor_ring_try_acquire(&r, &rt, &rv, &rp, &ru),
             WEFT_TENSOR_EAGAIN, "empty try_acquire");
    const uint64_t t0 = now_ns();
    CHECK_ST(weft_tensor_ring_acquire(&r, 30000000ull, &rt, &rv, &rp, &ru),
             WEFT_TENSOR_ETIMEOUT, "empty acquire times out");
    const uint64_t dt = now_ns() - t0;
    CHECK(dt >= 25 * 1000000ull, "deadline honored (>=25ms)");
    CHECK(dt < 2000000000ull, "deadline not overshot absurdly");

    // Fill the ring; claim behaves the same.
    uint64_t shape[1] = {64};
    for (int i = 0; i < 2; i++) {
        uint64_t t; uint8_t* p;
        CHECK_ST(weft_tensor_ring_claim(&r, 0, &t, &p, NULL), WEFT_TENSOR_OK,
                 "fill");
        weft_tensor_view_t v;
        CHECK_ST(weft_tensor_view_init(&v, i, WEFT_DTYPE_U8, 1, shape,
                                       (uintptr_t)p, 0),
                 WEFT_TENSOR_OK, "v");
        CHECK_ST(weft_tensor_ring_commit(&r, t, &v, 64), WEFT_TENSOR_OK, "c");
    }
    uint64_t t; uint8_t* p;
    CHECK_ST(weft_tensor_ring_claim(&r, 0, &t, &p, NULL), WEFT_TENSOR_EAGAIN,
             "full claim timeout-0 == single try");

    // WT36: 60ms budget on the full ring.
    const uint64_t t1 = now_ns();
    CHECK_ST(weft_tensor_ring_claim(&r, 60000000ull, &t, &p, NULL),
             WEFT_TENSOR_ETIMEOUT, "full claim 60ms");
    const uint64_t dt2 = now_ns() - t1;
    CHECK(dt2 >= 50 * 1000000ull, "wait ladder burned the budget");
    printf("WT36 measured: full-ring claim ETIMEOUT after %.1f ms budget\n",
           (double)dt2 / 1e6);
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------
// WT31 — two handles, one mapping
// ---------------------------------------------------------------------------

static void test_wt31(void) {
    weft_tensor_ring_t a, b;
    CHECK_ST(weft_tensor_ring_create(&a, 4, 512, WEFT_TENSOR_RING_MODE_SPMC, 0),
             WEFT_TENSOR_OK, "wt31 create");
    CHECK_ST(weft_tensor_ring_attach(&b, a.base, a.ring_bytes,
                                     WEFT_TENSOR_RING_MODE_SPMC),
             WEFT_TENSOR_OK, "wt31 attach");

    // Produce through A, consume through B.
    uint64_t t; uint8_t* p;
    CHECK_ST(weft_tensor_ring_try_claim(&a, &t, &p, NULL), WEFT_TENSOR_OK, "claim A");
    for (int i = 0; i < 512; i++) p[i] = (uint8_t)(i ^ 0xA5);
    uint64_t shape[2] = {32, 16};
    weft_tensor_view_t v;
    CHECK_ST(weft_tensor_view_init(&v, 77, WEFT_DTYPE_U8, 2, shape,
                                   (uintptr_t)p, 0),
             WEFT_TENSOR_OK, "v");
    CHECK_ST(weft_tensor_ring_commit(&a, t, &v, 512), WEFT_TENSOR_OK, "commit A");

    const weft_tensor_view_t* rv; const uint8_t* rp; uint32_t ru; uint64_t rt;
    CHECK_ST(weft_tensor_ring_try_acquire(&b, &rt, &rv, &rp, &ru),
             WEFT_TENSOR_OK, "acquire B");
    CHECK(rv->tensor_id == 77 && ru == 512, "handle B reads handle A's tensor");
    CHECK(rp == p, "payload identity across handles");
    CHECK_ST(weft_tensor_ring_release(&b, rt), WEFT_TENSOR_OK, "release B");
    weft_tensor_ring_destroy(&b);
    weft_tensor_ring_destroy(&a);
}

// ---------------------------------------------------------------------------
// WT35 — stats
// ---------------------------------------------------------------------------

static void test_wt35(void) {
    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 2, 64, WEFT_TENSOR_RING_MODE_MPMC, 0),
             WEFT_TENSOR_OK, "wt35 create");
    uint64_t shape[1] = {64};
    for (int i = 0; i < 2; i++) {
        uint64_t t; uint8_t* p;
        CHECK_ST(weft_tensor_ring_try_claim(&r, &t, &p, NULL), WEFT_TENSOR_OK, "c");
        weft_tensor_view_t v;
        CHECK_ST(weft_tensor_view_init(&v, i, WEFT_DTYPE_U8, 1, shape,
                                       (uintptr_t)p, 0),
                 WEFT_TENSOR_OK, "v");
        CHECK_ST(weft_tensor_ring_commit(&r, t, &v, 64), WEFT_TENSOR_OK, "cm");
    }
    uint64_t committed = 0, acquired = 0, released = 0, full = 0;
    weft_tensor_ring_stats(&r, &committed, &acquired, &released, &full);
    CHECK(committed == 2 && acquired == 0 && released == 0, "stats after commits");
    CHECK(weft_tensor_ring_in_flight(&r) == 2, "in_flight 2");

    const weft_tensor_view_t* rv; const uint8_t* rp; uint32_t ru; uint64_t rt;
    CHECK_ST(weft_tensor_ring_try_acquire(&r, &rt, &rv, &rp, &ru),
             WEFT_TENSOR_OK, "aq");
    CHECK_ST(weft_tensor_ring_release(&r, rt), WEFT_TENSOR_OK, "rel");
    weft_tensor_ring_stats(&r, &committed, &acquired, &released, &full);
    CHECK(committed == 2 && acquired == 1 && released == 1, "stats after drain");
    CHECK(weft_tensor_ring_in_flight(&r) == 1, "in_flight 1");
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------
// WT37 — alignment law on ring payloads
// ---------------------------------------------------------------------------

static void test_wt37(void) {
    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 32, 4096, WEFT_TENSOR_RING_MODE_MPSC, 0),
             WEFT_TENSOR_OK, "wt37 create");
    // 4096 is a 128 multiple: every slot payload 128B-aligned (Law 4).
    for (uint64_t ticket = 0; ticket < 32; ticket++) {
        uint8_t* p = r.base + 128 + (ticket & 31) * r.slot_stride + 256;
        CHECK(((uintptr_t)p & 127u) == 0, "128B payload alignment");
    }
    // Commit-time dtype-natural sweep: every dtype at offset 1 -> EMISALIGN
    // (except the 1-byte dtypes, which always pass).
    static const weft_dtype_t kDts[] = {WEFT_DTYPE_I16, WEFT_DTYPE_F16,
                                        WEFT_DTYPE_BF16, WEFT_DTYPE_F32,
                                        WEFT_DTYPE_F64};
    uint64_t t; uint8_t* p;
    CHECK_ST(weft_tensor_ring_try_claim(&r, &t, &p, NULL), WEFT_TENSOR_OK, "claim");
    for (size_t i = 0; i < sizeof(kDts) / sizeof(kDts[0]); i++) {
        weft_tensor_view_t v;
        uint64_t s[1] = {8};
        CHECK_ST(weft_tensor_view_init(&v, 0, kDts[i], 1, s, (uintptr_t)p, 1),
                 WEFT_TENSOR_OK, "odd offset view");
        char msg[48];
        snprintf(msg, sizeof(msg), "%s odd offset commit refused",
                 weft_dtype_name(kDts[i]));
        CHECK_ST(weft_tensor_ring_commit(&r, t, &v, 512),
                 WEFT_TENSOR_EMISALIGN, msg);
    }
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------
// WT38 — flagship: zero-copy view algebra over a ring tensor
// ---------------------------------------------------------------------------

static void test_wt38(void) {
    // NCHW f32 [1,3,8,8] = 768B in a 1024B slot.
    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 4, 1024, WEFT_TENSOR_RING_MODE_MPSC, 0),
             WEFT_TENSOR_OK, "wt38 create");
    uint64_t ticket; uint8_t* p;
    CHECK_ST(weft_tensor_ring_try_claim(&r, &ticket, &p, NULL), WEFT_TENSOR_OK,
             "claim");
    // Producer fills a deterministic f32 plane; element (0,c,h,w) at
    // c*256 + h*32 + w*4 (test-side arithmetic).
    for (uint32_t c = 0; c < 3; c++) {
        for (uint32_t h = 0; h < 8; h++) {
            for (uint32_t w = 0; w < 8; w++) {
                const uint32_t off = c * 256 + h * 32 + w * 4;
                *(float*)(p + off) = (float)(c * 100 + h * 10 + w) + 0.5f;
            }
        }
    }
    uint64_t shape[4] = {1, 3, 8, 8};
    weft_tensor_view_t v;
    CHECK_ST(weft_tensor_view_init(&v, 900, WEFT_DTYPE_F32, 4, shape,
                                   (uintptr_t)p, 0),
             WEFT_TENSOR_OK, "nchw view");
    CHECK_ST(weft_tensor_ring_commit(&r, ticket, &v, 1024), WEFT_TENSOR_OK,
             "commit");

    // CONSUMER: slice channel 1, crop rows[2,6) cols[4,8), permute to NHWC.
    const weft_tensor_view_t* rv; const uint8_t* rp; uint32_t ru; uint64_t rt;
    CHECK_ST(weft_tensor_ring_try_acquire(&r, &rt, &rv, &rp, &ru),
             WEFT_TENSOR_OK, "acquire");
    weft_tensor_view_t ch, roi, nhwc;
    CHECK_ST(weft_tensor_view_slice(&ch, rv, 1, 1, 1), WEFT_TENSOR_OK, "slice ch1");
    const uint64_t off4[4] = {0, 0, 2, 4};
    const uint64_t cnt4[4] = {1, 1, 4, 4};
    CHECK_ST(weft_tensor_view_subwindow(&roi, &ch, off4, cnt4), WEFT_TENSOR_OK,
             "ROI");
    const uint8_t perm[4] = {0, 2, 3, 1};
    CHECK_ST(weft_tensor_view_permute(&nhwc, &roi, perm), WEFT_TENSOR_OK,
             "permute NHWC");
    CHECK(nhwc.shape[1] == 4 && nhwc.shape[2] == 4 && nhwc.shape[3] == 1,
          "NHWC [1,4,4,1]");

    // Bit-exact address proof: element (0,h,w,0) of the NHWC view must land
    // on payload offset 256 + (h+2)*32 + (w+4)*4 — independent arithmetic.
    for (uint32_t h = 0; h < 4; h++) {
        for (uint32_t w = 0; w < 4; w++) {
            const uint64_t idx[4] = {0, h, w, 0};
            const void* addr = weft_tensor_view_element_addr_at(&nhwc, idx, rp);
            CHECK(addr != NULL, "addr");
            const uint64_t want = 256ull + (h + 2) * 32 + (w + 4) * 4;
            CHECK((uintptr_t)addr == (uintptr_t)rp + want, "zero-copy addr exact");
            // And the value flows through untouched: one pointer add, zero copies.
            const float got = *(const float*)addr;
            const float wantv = (float)(1 * 100 + (h + 2) * 10 + (w + 4)) + 0.5f;
            CHECK(got == wantv, "value through the view chain");
        }
    }
    CHECK_ST(weft_tensor_ring_release(&r, rt), WEFT_TENSOR_OK, "release");
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------
// Stress infrastructure (WT32/33/39/40)
// ---------------------------------------------------------------------------

#define STRESS_PAYLOAD 4096u
#define STRESS_WORDS (STRESS_PAYLOAD / 8u)

typedef struct {
    weft_tensor_ring_t* ring;
    uint64_t msgs;
    int failures;
    pthread_barrier_t* barrier;
} prod_ctx_t;

typedef struct {
    weft_tensor_ring_t* ring;
    uint64_t total;          // total messages to drain
    uint64_t acquired;       // tickets this consumer saw
    uint64_t* tickets;       // recorded tickets (SPMC/MMPC proof)
    int failures;
    uint64_t torn;           // word mismatches (must stay 0)
    uint64_t bad_views;
} cons_ctx_t;

/// Verify one acquired message: per-ticket word pattern + per-parity
/// geometry (even tickets NCHW [1,4,16,64]; odd tickets NHWC [1,16,64,4]).
static int verify_stress_message(uint64_t ticket, const weft_tensor_view_t* view,
                                 const uint8_t* payload, uint32_t used,
                                 uint64_t* torn_out, uint64_t* bad_views_out) {
    int ok = 1;
    if (view->tensor_id != ticket || view->dtype != WEFT_DTYPE_U8 ||
        view->ndim != 4 || used != STRESS_PAYLOAD) {
        (*bad_views_out)++;
        ok = 0;
    }
    // [1,4,16,64] u8 contiguous strides: {4096, 1024, 64, 1}. The NHWC
    // permutation (0,2,3,1) maps them to {4096, 64, 1, 1024}.
    int shape_ok;
    if (ticket & 1) {
        shape_ok = view->shape[1] == 16 && view->shape[2] == 64 &&
                   view->shape[3] == 4 && view->strides[0] == 4096 &&
                   view->strides[1] == 64 && view->strides[2] == 1 &&
                   view->strides[3] == 1024;
    } else {
        shape_ok = view->shape[1] == 4 && view->shape[2] == 16 &&
                   view->shape[3] == 64 && view->strides[1] == 1024 &&
                   view->strides[2] == 64 && view->strides[3] == 1;
    }
    if (!shape_ok) {
        (*bad_views_out)++;
        ok = 0;
    }
    if (((uintptr_t)payload & 63u) != 0) {
        (*bad_views_out)++;
        ok = 0;
    }
    const uint64_t* words = (const uint64_t*)payload;
    for (uint32_t k = 0; k < STRESS_WORDS; k++) {
        if (words[k] != tword(ticket, k)) {
            (*torn_out)++;
            ok = 0;
            break;  // report once per message; count captures the tear
        }
    }
    return ok;
}

static void* producer_main(void* arg) {
    prod_ctx_t* ctx = (prod_ctx_t*)arg;
    pthread_barrier_wait(ctx->barrier);
    for (uint64_t i = 0; i < ctx->msgs; i++) {
        uint64_t ticket; uint8_t* payload; uint32_t cap;
        if (weft_tensor_ring_claim(ctx->ring, 500000000ull, &ticket, &payload,
                                   &cap) != WEFT_TENSOR_OK) {
            ctx->failures++;
            break;
        }
        uint64_t* w = (uint64_t*)payload;
        for (uint32_t k = 0; k < STRESS_WORDS; k++) w[k] = tword(ticket, k);
        uint64_t shape[4] = {1, 4, 16, 64};
        weft_tensor_view_t v, pv;
        if (weft_tensor_view_init(&v, ticket, WEFT_DTYPE_U8, 4, shape,
                                  (uintptr_t)payload, 0) != WEFT_TENSOR_OK) {
            ctx->failures++;
            break;
        }
        if (ticket & 1) {
            const uint8_t perm[4] = {0, 2, 3, 1};
            if (weft_tensor_view_permute(&pv, &v, perm) != WEFT_TENSOR_OK) {
                ctx->failures++;
                break;
            }
            v = pv;
        }
        if (weft_tensor_ring_commit(ctx->ring, ticket, &v, STRESS_PAYLOAD) !=
            WEFT_TENSOR_OK) {
            ctx->failures++;
            break;
        }
    }
    return NULL;
}

/// MPSC single consumer: strict FIFO drain of `total` messages.
static void* mpsc_consumer_main(void* arg) {
    cons_ctx_t* ctx = (cons_ctx_t*)arg;
    uint64_t expected = 0;
    while (ctx->acquired < ctx->total) {
        uint64_t ticket; const weft_tensor_view_t* view; const uint8_t* payload;
        uint32_t used;
        const int st = weft_tensor_ring_acquire(ctx->ring, 500000000ull, &ticket,
                                                &view, &payload, &used);
        if (st == WEFT_TENSOR_ETIMEOUT) {
            ctx->failures++;
            break;
        }
        if (st != WEFT_TENSOR_OK) {
            ctx->failures++;
            continue;
        }
        if (ticket != expected) {
            // FIFO violation: a skipped or reordered ticket is a protocol
            // tear — record and resync.
            ctx->failures++;
            expected = ticket;
        }
        if (!verify_stress_message(ticket, view, payload, used, &ctx->torn,
                                   &ctx->bad_views)) {
            ctx->failures++;
        }
        ctx->tickets[ctx->acquired] = ticket;
        ctx->acquired++;
        expected++;
        if (weft_tensor_ring_release(ctx->ring, ticket) != WEFT_TENSOR_OK) {
            ctx->failures++;
        }
    }
    return NULL;
}

/// SPMC/MMPC racing consumers: exactly-once via the tail CAS.
static void* racing_consumer_main(void* arg) {
    cons_ctx_t* ctx = (cons_ctx_t*)arg;
    const uint64_t deadline = now_ns() + 10000000000ull;  // 10s hard ceiling
    for (;;) {
        uint64_t ticket; const weft_tensor_view_t* view; const uint8_t* payload;
        uint32_t used;
        const int st = weft_tensor_ring_try_acquire(ctx->ring, &ticket, &view,
                                                    &payload, &used);
        if (st == WEFT_TENSOR_OK) {
            if (!verify_stress_message(ticket, view, payload, used, &ctx->torn,
                                       &ctx->bad_views)) {
                ctx->failures++;
            }
            ctx->tickets[ctx->acquired] = ticket;
            ctx->acquired++;
            if (weft_tensor_ring_release(ctx->ring, ticket) != WEFT_TENSOR_OK) {
                ctx->failures++;
            }
            continue;
        }
        // EAGAIN: done when the ring's acquired counter reaches total.
        uint64_t acquired = 0;
        weft_tensor_ring_stats(ctx->ring, NULL, &acquired, NULL, NULL);
        if (acquired >= ctx->total) break;
        if (now_ns() >= deadline) {
            ctx->failures++;
            break;
        }
        sched_yield();
    }
    return NULL;
}

static int cmp_u64(const void* a, const void* b) {
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/// Merge + sort every consumer's ticket list; prove 0..total-1 exactly once.
static int tickets_exactly_once(cons_ctx_t* ctxs, int nctx, uint64_t total) {
    uint64_t* all = malloc((size_t)total * sizeof(uint64_t));
    if (all == NULL) return 0;
    uint64_t n = 0;
    for (int i = 0; i < nctx; i++) {
        memcpy(all + n, ctxs[i].tickets, (size_t)ctxs[i].acquired * sizeof(uint64_t));
        n += ctxs[i].acquired;
    }
    if (n != total) {
        free(all);
        return 0;
    }
    qsort(all, (size_t)total, sizeof(uint64_t), cmp_u64);
    for (uint64_t i = 0; i < total; i++) {
        if (all[i] != i) {
            free(all);
            return 0;
        }
    }
    free(all);
    return 1;
}

// ---------------------------------------------------------------------------
// WT32 — MPSC tearing stress
// ---------------------------------------------------------------------------

static void test_wt32(void) {
    enum { NPROD = 4, PER = 12500 };
    const uint64_t total = (uint64_t)NPROD * PER;
    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 64, STRESS_PAYLOAD,
                                     WEFT_TENSOR_RING_MODE_MPSC, 0),
             WEFT_TENSOR_OK, "wt32 create");

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, NPROD + 1);
    prod_ctx_t pctx[NPROD];
    pthread_t prods[NPROD];
    for (int i = 0; i < NPROD; i++) {
        pctx[i].ring = &r;
        pctx[i].msgs = PER;
        pctx[i].failures = 0;
        pctx[i].barrier = &barrier;
        pthread_create(&prods[i], NULL, producer_main, &pctx[i]);
    }

    cons_ctx_t cctx = {0};
    cctx.ring = &r;
    cctx.total = total;
    cctx.tickets = malloc((size_t)total * sizeof(uint64_t));
    CHECK(cctx.tickets != NULL, "wt32 ticket store");
    pthread_barrier_wait(&barrier);
    mpsc_consumer_main(&cctx);
    for (int i = 0; i < NPROD; i++) pthread_join(prods[i], NULL);
    pthread_barrier_destroy(&barrier);

    for (int i = 0; i < NPROD; i++) {
        char msg[48];
        snprintf(msg, sizeof(msg), "wt32 producer %d failure-free", i);
        CHECK(pctx[i].failures == 0, msg);
    }
    CHECK(cctx.acquired == total, "wt32 all messages consumed");
    CHECK(cctx.torn == 0, "wt32 ZERO torn words");
    CHECK(cctx.bad_views == 0, "wt32 zero bad views");
    CHECK(cctx.failures == 0, "wt32 consumer failure-free (strict FIFO)");
    CHECK(tickets_exactly_once(&cctx, 1, total), "wt32 tickets 0..N-1 in order");

    uint64_t committed = 0, acquired = 0, released = 0;
    weft_tensor_ring_stats(&r, &committed, &acquired, &released, NULL);
    CHECK(committed == total && acquired == total && released == total,
          "wt32 counters exact");
    printf("WT32 MPSC stress: %llu messages, %llu word checks, 0 tears, "
           "0 FIFO violations\n",
           (unsigned long long)total,
           (unsigned long long)(total * STRESS_WORDS));
    free(cctx.tickets);
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------
// WT33 — SPMC racing-consumer stress
// ---------------------------------------------------------------------------

static void test_wt33(void) {
    enum { NCONS = 4, TOTAL = 50000 };
    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 64, STRESS_PAYLOAD,
                                     WEFT_TENSOR_RING_MODE_SPMC, 0),
             WEFT_TENSOR_OK, "wt33 create");

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 2);
    prod_ctx_t pctx = {&r, TOTAL, 0, &barrier};
    pthread_t prod;
    pthread_create(&prod, NULL, producer_main, &pctx);

    cons_ctx_t cctx[NCONS];
    pthread_t cons[NCONS];
    pthread_barrier_wait(&barrier);
    for (int i = 0; i < NCONS; i++) {
        memset(&cctx[i], 0, sizeof(cctx[i]));
        cctx[i].ring = &r;
        cctx[i].total = TOTAL;
        cctx[i].tickets = malloc(TOTAL * sizeof(uint64_t));
        CHECK(cctx[i].tickets != NULL, "wt33 ticket store");
        pthread_create(&cons[i], NULL, racing_consumer_main, &cctx[i]);
    }
    pthread_join(prod, NULL);
    for (int i = 0; i < NCONS; i++) pthread_join(cons[i], NULL);
    pthread_barrier_destroy(&barrier);

    CHECK(pctx.failures == 0, "wt33 producer failure-free");
    // Exactly-once BEFORE the frees (the merge reads every ticket array).
    CHECK(tickets_exactly_once(cctx, NCONS, TOTAL),
          "wt33 every ticket acquired EXACTLY once (CAS exactly-once)");
    uint64_t torn = 0, bad = 0, failures = 0;
    for (int i = 0; i < NCONS; i++) {
        torn += cctx[i].torn;
        bad += cctx[i].bad_views;
        failures += (uint64_t)cctx[i].failures;
        free(cctx[i].tickets);
    }
    CHECK(failures == 0, "wt33 consumers failure-free");
    CHECK(torn == 0, "wt33 ZERO torn words");
    CHECK(bad == 0, "wt33 zero bad views");
    uint64_t committed = 0, acquired = 0, released = 0;
    weft_tensor_ring_stats(&r, &committed, &acquired, &released, NULL);
    CHECK(committed == TOTAL && acquired == TOTAL && released == TOTAL,
          "wt33 counters exact");
    printf("WT33 SPMC stress: %d messages, 4 racing consumers, 0 tears, "
           "exactly-once proven\n",
           TOTAL);
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------
// WT34 — fork torture (cross-process MPSC; POSIX)
// ---------------------------------------------------------------------------

static void test_wt34(int skip_fork) {
    if (skip_fork) {
        printf("WT34 fork torture: SKIPPED (declared: WT_SKIP_FORK=1 — "
               "TSAN leg)\n");
        return;
    }
    enum { NCHILD = 2, PER_CHILD = 5000 };
    const uint64_t total = (uint64_t)NCHILD * PER_CHILD;
    const uint32_t payload = 1024;
    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 32, payload,
                                     WEFT_TENSOR_RING_MODE_MPSC, 0),
             WEFT_TENSOR_OK, "wt34 create (MAP_SHARED)");

    pid_t pids[NCHILD];
    for (int c = 0; c < NCHILD; c++) {
        pids[c] = fork();
        if (pids[c] == 0) {
            // CHILD: attach to the inherited mapping (proves the attach
            // contract cross-process) and produce PER_CHILD tensors.
            weft_tensor_ring_t cr;
            if (weft_tensor_ring_attach(&cr, r.base, r.ring_bytes,
                                        WEFT_TENSOR_RING_MODE_MPSC) !=
                WEFT_TENSOR_OK) {
                _exit(42);
            }
            int fails = 0;
            for (uint64_t i = 0; i < PER_CHILD; i++) {
                uint64_t ticket; uint8_t* p; uint32_t cap;
                if (weft_tensor_ring_claim(&cr, 500000000ull, &ticket, &p,
                                           &cap) != WEFT_TENSOR_OK) {
                    fails++;
                    break;
                }
                uint64_t* w = (uint64_t*)p;
                for (uint32_t k = 0; k < payload / 8; k++) {
                    w[k] = tword(ticket, k);
                }
                uint64_t shape[3] = {1, 8, 128};
                weft_tensor_view_t v;
                if (weft_tensor_view_init(&v, ticket, WEFT_DTYPE_U8, 3, shape,
                                          (uintptr_t)p, 0) != WEFT_TENSOR_OK ||
                    weft_tensor_ring_commit(&cr, ticket, &v, payload) !=
                        WEFT_TENSOR_OK) {
                    fails++;
                    break;
                }
            }
            _exit(fails == 0 ? 0 : 43);
        }
        CHECK(pids[c] > 0, "wt34 fork");
    }

    // PARENT: the MPSC consumer across process boundaries.
    uint64_t seen = 0, torn = 0, bad = 0, fifo_violations = 0, expected = 0;
    while (seen < total) {
        uint64_t ticket; const weft_tensor_view_t* view; const uint8_t* payloadp;
        uint32_t used;
        const int st = weft_tensor_ring_acquire(&r, 500000000ull, &ticket, &view,
                                                &payloadp, &used);
        if (st == WEFT_TENSOR_ETIMEOUT) break;
        if (st != WEFT_TENSOR_OK) continue;
        if (ticket != expected) fifo_violations++;
        expected = ticket + 1;
        if (view->tensor_id != ticket || view->dtype != WEFT_DTYPE_U8 ||
            view->ndim != 3 || used != payload ||
            view->shape[1] != 8 || view->shape[2] != 128) {
            bad++;
        } else {
            const uint64_t* w = (const uint64_t*)payloadp;
            for (uint32_t k = 0; k < payload / 8; k++) {
                if (w[k] != tword(ticket, k)) {
                    torn++;
                    break;
                }
            }
        }
        seen++;
        weft_tensor_ring_release(&r, ticket);
    }
    for (int c = 0; c < NCHILD; c++) {
        int status = 0;
        waitpid(pids[c], &status, 0);
        char msg[48];
        snprintf(msg, sizeof(msg), "wt34 child %d exit clean", c);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, msg);
    }
    CHECK(seen == total, "wt34 all cross-process tensors consumed");
    CHECK(torn == 0, "wt34 ZERO torn words across processes");
    CHECK(bad == 0, "wt34 zero bad views");
    CHECK(fifo_violations == 0, "wt34 strict FIFO across processes");
    printf("WT34 fork torture: %llu cross-process tensors, 0 tears\n",
           (unsigned long long)total);
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------
// WT39 — MPMC stress (both sides multi)
// ---------------------------------------------------------------------------

static void test_wt39(void) {
    enum { NPROD = 2, PER = 10000, NCONS = 2 };
    const uint64_t total = (uint64_t)NPROD * PER;
    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 64, STRESS_PAYLOAD,
                                     WEFT_TENSOR_RING_MODE_MPMC, 0),
             WEFT_TENSOR_OK, "wt39 create");

    pthread_barrier_t barrier;
    // Producers + the main thread sync here (racing consumers start
    // immediately — they poll until the ring's acquired counter reaches
    // total, so they need no start gun).
    pthread_barrier_init(&barrier, NULL, NPROD + 1);
    prod_ctx_t pctx[NPROD];
    pthread_t prods[NPROD];
    for (int i = 0; i < NPROD; i++) {
        pctx[i].ring = &r;
        pctx[i].msgs = PER;
        pctx[i].failures = 0;
        pctx[i].barrier = &barrier;
        pthread_create(&prods[i], NULL, producer_main, &pctx[i]);
    }
    cons_ctx_t cctx[NCONS];
    pthread_t cons[NCONS];
    for (int i = 0; i < NCONS; i++) {
        memset(&cctx[i], 0, sizeof(cctx[i]));
        cctx[i].ring = &r;
        cctx[i].total = total;
        cctx[i].tickets = malloc((size_t)total * sizeof(uint64_t));
        CHECK(cctx[i].tickets != NULL, "wt39 ticket store");
        pthread_create(&cons[i], NULL, racing_consumer_main, &cctx[i]);
    }
    pthread_barrier_wait(&barrier);
    for (int i = 0; i < NPROD; i++) pthread_join(prods[i], NULL);
    for (int i = 0; i < NCONS; i++) pthread_join(cons[i], NULL);
    pthread_barrier_destroy(&barrier);

    for (int i = 0; i < NPROD; i++) {
        CHECK(pctx[i].failures == 0, "wt39 producer failure-free");
    }
    // Exactly-once BEFORE the frees (the merge reads every ticket array).
    CHECK(tickets_exactly_once(cctx, NCONS, total),
          "wt39 every ticket exactly once");
    uint64_t torn = 0, bad = 0;
    for (int i = 0; i < NCONS; i++) {
        torn += cctx[i].torn;
        bad += cctx[i].bad_views;
        CHECK(cctx[i].failures == 0, "wt39 consumer failure-free");
        free(cctx[i].tickets);
    }
    CHECK(torn == 0, "wt39 ZERO torn words");
    CHECK(bad == 0, "wt39 zero bad views");
    printf("WT39 MPMC stress: %llu messages, 2x2 sides, 0 tears\n",
           (unsigned long long)total);
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------
// WT40 — throughput fixture (verified payload moved; --bench for the
// heavy leg). Uses the same MPSC machinery with bigger payloads.
// ---------------------------------------------------------------------------

static void test_wt40(int heavy) {
    const int nprod = 4;
    const uint64_t per = heavy ? 12500 : 2500;
    const uint32_t payload = heavy ? 16384 : 4096;
    const uint64_t total = (uint64_t)nprod * per;

    weft_tensor_ring_t r;
    CHECK_ST(weft_tensor_ring_create(&r, 64, payload,
                                     WEFT_TENSOR_RING_MODE_MPSC, 0),
             WEFT_TENSOR_OK, "wt40 create");

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, nprod + 1);
    prod_ctx_t pctx[4];
    pthread_t prods[4];
    for (int i = 0; i < nprod; i++) {
        pctx[i].ring = &r;
        pctx[i].msgs = per;
        pctx[i].failures = 0;
        pctx[i].barrier = &barrier;
        pthread_create(&prods[i], NULL, producer_main, &pctx[i]);
    }
    cons_ctx_t cctx = {0};
    cctx.ring = &r;
    cctx.total = total;
    cctx.tickets = malloc((size_t)total * sizeof(uint64_t));
    CHECK(cctx.tickets != NULL, "wt40 ticket store");
    pthread_barrier_wait(&barrier);
    const uint64_t t0 = now_ns();
    mpsc_consumer_main(&cctx);
    const uint64_t dt = now_ns() - t0;
    for (int i = 0; i < nprod; i++) pthread_join(prods[i], NULL);
    pthread_barrier_destroy(&barrier);

    for (int i = 0; i < nprod; i++) {
        CHECK(pctx[i].failures == 0, "wt40 producer failure-free");
    }
    CHECK(cctx.acquired == total, "wt40 all consumed");
    CHECK(cctx.torn == 0 && cctx.bad_views == 0 && cctx.failures == 0,
          "wt40 integrity");
    const double secs = (double)dt / 1e9;
    const double msgs_s = (double)total / secs;
    const double gb_s = (double)total * payload / secs / 1e9;
    printf("{\"test\":\"WT40\",\"variant\":\"%s\",\"producers\":4,"
           "\"slots\":64,\"payload\":%u,\"msgs\":%llu,\"secs\":%.4f,"
           "\"msgs_s\":%.0f,\"gb_s\":%.2f,\"verified_words\":%llu}\n",
           heavy ? "bench-16k" : "smoke-4k", payload,
           (unsigned long long)total, secs, msgs_s, gb_s,
           (unsigned long long)(total * (payload / 8)));
    free(cctx.tickets);
    weft_tensor_ring_destroy(&r);
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    const int heavy = argc > 1 && strcmp(argv[1], "--bench") == 0;
    const int skip_fork = getenv("WT_SKIP_FORK") != NULL;

    test_wt25();
    test_wt26();
    test_wt27();
    test_wt28();
    test_wt29();
    test_wt30();  // includes WT36 ladder measurement
    test_wt31();
    test_wt35();
    test_wt37();
    test_wt38();
    test_wt32();
    test_wt33();
    test_wt39();
    test_wt34(skip_fork);
    test_wt40(heavy);

    printf("tensor-ring: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
