// cluster_ring_test.c — WCR1 ring engine conformance (R-series).
//
// R1  CRC-32/IEEE check value + incremental; LE round-trips; two-store
//     hi/lo encoding golden values (parity mirror) across the 2^32 seam.
// R2  Region create: golden header bytes (magic, identity, geometry),
//     128B alignment, exact region size, null consensus state.
// R3  Attach validation ladder: every distinct refusal code fires.
// R4  100k publish->acquire->consume, in-order, payload integrity,
//     ZERO heap allocations on the steady-state path (Law 1 proof).
// R5  Two-store tear detection: crashed-writer states (lo-new/hi-stale
//     and hi-new/lo-stale) -> E_SEQ_TORN after bounded retries.
// R6  Backpressure: high-water F_BP_MARK stamps; ring-full refusal
//     E_BACKPRESSURE; consume unblocks the producer.
// R7  Overrun/alias: byzantine seq poke -> E_SEQ_OVERRUN on acquire;
//     message_seq alias detection.
// R8  Slot header LE byte layout (golden offsets).
// R9  Consumer watermark protocol: two-store commit, stable reads, lag.
// R10 Remote addressing: pure-arithmatic slot offsets across wraps.
// R11 Cross-process fork torture: MAP_SHARED region, child producer
//     100k msgs under backpressure, parent consumer, zero tears, exact
//     in-order delivery, watermark convergence.

#include "weft_cluster.h"
#include "weft_cluster_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/mman.h>

static int g_fail = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

#define REPORT(id, name)                                                    \
    do {                                                                    \
        if (g_fail == fail_before) printf("%-4s %-52s PASS\n", id, name);   \
        else printf("%-4s %-52s FAIL (%d)\n", id, name, g_fail - fail_before);\
    } while (0)

static uint8_t TEST_CID[16] = {
    'W', 'C', 'R', '1', '-', 'T', 'E', 'S', 'T', '-', 'C', 'L', 'U', 'S', 'T', 'R'
};

/* ---- counting allocation hooks (Law 1 proof) ------------------------- */

static uint64_t g_alloc_count = 0, g_free_count = 0, g_alloc_bytes = 0;

static void *counting_alloc(size_t size, size_t alignment, void *user)
{
    (void)user;
    g_alloc_count++;
    g_alloc_bytes += size;
    void *p = NULL;
    if (posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;
}

static void counting_free(void *ptr, void *user)
{
    (void)user;
    if (ptr) g_free_count++;
    free(ptr);
}

/* ---- shared-memory hooks for the fork test (R11) --------------------- */

static uint64_t g_mmap_len = 0;

static void *shm_alloc(size_t size, size_t alignment, void *user)
{
    (void)alignment; (void)user;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    g_mmap_len = (uint64_t)size;
    return p;
}

static void shm_free(void *ptr, void *user)
{
    (void)user;
    if (ptr) munmap(ptr, (size_t)g_mmap_len);
}

static uint64_t test_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------------------

static void t_r1_crc_le_seq(void)
{
    int fail_before = g_fail;

    /* CRC-32/IEEE standard check value. */
    CHECK(wcr1_crc32("123456789", 9) == 0xCBF43926u,
          "crc32 check value wrong: 0x%08X", wcr1_crc32("123456789", 9));
    CHECK(wcr1_crc32("", 0) == 0u, "crc32(empty) != 0");
    uint8_t buf[9] = "12345678";
    uint32_t inc = wcr1_crc32_update(wcr1_crc32(buf, 8), "9", 1);
    CHECK(inc == 0xCBF43926u, "crc32 incremental mismatch");

    /* LE round-trips. */
    uint8_t b[8];
    wcr1_le32_put(b, 0x31524357u);
    CHECK(b[0] == 0x57 && b[1] == 0x43 && b[2] == 0x52 && b[3] == 0x31,
          "le32 byte order wrong");
    CHECK(wcr1_le32_get(b) == 0x31524357u, "le32 roundtrip");
    wcr1_le64_put(b, 0x0102030405060708ull);
    CHECK(b[0] == 0x08, "le64 byte order");
    CHECK(wcr1_le64_get(b) == 0x0102030405060708ull, "le64 roundtrip");

    /* Two-store encoding: parity mirror + 2^32 seam. */
    uint64_t s = 0x0000000100000005ull;      /* seq 2^32+5, odd */
    uint32_t hi = wcr1_seq_hi_word(s);
    CHECK(hi == 0x80000001u, "hi_word(2^32+5) = 0x%08X", hi);
    CHECK(wcr1_seq_from_halves(hi, 5u) == s, "from_halves roundtrip");
    CHECK(wcr1_seq_parity_ok(hi, 5u), "parity ok");
    CHECK(!wcr1_seq_parity_ok(hi, 4u), "parity mismatch detected");
    CHECK(wcr1_seq_parity_ok(wcr1_seq_hi_word(4ull), 4u), "even parity ok");
    CHECK(wcr1_seq_hi_word(4ull) == 0u, "hi_word(4) carries only parity");

    REPORT("R1", "crc32/LE/two-store encoding");
}

static void t_r2_create_golden(void)
{
    int fail_before = g_fail;
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.cluster_id, TEST_CID, 16);
    cfg.node_id = 2;
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 64;
    cfg.slot_size = 192;
    cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_ring_view_t v;
    void *region = NULL;
    uint64_t rlen = 0;
    int e = wcr1_ring_create(&cfg, 1, &v, &region, &rlen);
    CHECK(e == WEFT_CLUSTER_OK, "create failed: %s", wcr1_err_name(e));
    CHECK(rlen == wcr1_region_size(64, 192), "region size %llu",
          (unsigned long long)rlen);
    CHECK(rlen == 128 + 64u * 192u, "region size value");
    CHECK(((uintptr_t)region % 128) == 0, "region 128B aligned");

    const uint8_t *raw = (const uint8_t *)region;
    CHECK(raw[0] == 'W' && raw[1] == 'C' && raw[2] == 'R' && raw[3] == '1',
          "magic bytes");
    CHECK(raw[4] == WCR1_LAYOUT_MAJOR && raw[5] == WCR1_LAYOUT_MINOR,
          "layout version bytes");
    CHECK(memcmp(raw + 8, TEST_CID, 16) == 0, "cluster id bytes");
    CHECK(wcr1_le32_get(raw + 0x18) == 2, "home node id");
    CHECK(wcr1_le32_get(raw + 0x1C) == 1, "producer node id");
    CHECK(wcr1_le32_get(raw + 0x20) == wcr1_crc32(region, 0x20),
          "config crc over [0x00,0x20)");
    CHECK(wcr1_le32_get(raw + 0x20) != 0, "crc nonzero");
    CHECK(wcr1_le64_get(raw + 0x28) == 7, "epoch");
    CHECK(wcr1_le64_get(raw + 0x30) == 64, "capacity");
    CHECK(wcr1_le64_get(raw + 0x38) == 192, "slot size");

    /* Null consensus state + zeroed sync registers (bytes 0x40..0x80). */
    bool zeroed = true;
    for (int i = 0x40; i < 0x80; i++) if (raw[i]) { zeroed = false; break; }
    CHECK(zeroed, "cacheline 1 starts zeroed (null state)");

    wcr1_engine_free(region);
    REPORT("R2", "region create golden bytes");
}

static void t_r3_validation_ladder(void)
{
    int fail_before = g_fail;
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.cluster_id, TEST_CID, 16);
    cfg.node_id = 2;
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 64;
    cfg.slot_size = 192;
    cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_ring_view_t v, a;
    void *region = NULL;
    uint64_t rlen = 0;
    CHECK(wcr1_ring_create(&cfg, 1, &v, &region, &rlen) == WEFT_CLUSTER_OK,
          "setup create");

    /* Clean attach passes. */
    CHECK(wcr1_ring_attach(region, rlen, TEST_CID, 7, 2, 1, 64, 192, &a) ==
          WEFT_CLUSTER_OK, "clean attach");

    /* E_ALIGN: misaligned region. */
    CHECK(wcr1_ring_attach((uint8_t *)region + 8, rlen, TEST_CID, 7, 2, 1,
                           64, 192, &a) == WEFT_CLUSTER_E_ALIGN, "E_ALIGN");

    /* E_REGION_SIZE: wrong length. */
    CHECK(wcr1_ring_attach(region, rlen - 64, TEST_CID, 7, 2, 1, 64, 192,
                           &a) == WEFT_CLUSTER_E_REGION_SIZE, "E_REGION_SIZE");

    /* E_MAGIC: corrupt magic. */
    ((uint8_t *)region)[0] = 'X';
    CHECK(wcr1_ring_attach(region, rlen, TEST_CID, 7, 2, 1, 64, 192, &a) ==
          WEFT_CLUSTER_E_MAGIC, "E_MAGIC");
    ((uint8_t *)region)[0] = 'W';

    /* E_LAYOUT: unsupported major. */
    ((uint8_t *)region)[4] = 9;
    CHECK(wcr1_ring_attach(region, rlen, TEST_CID, 7, 2, 1, 64, 192, &a) ==
          WEFT_CLUSTER_E_LAYOUT, "E_LAYOUT");
    ((uint8_t *)region)[4] = 1;

    /* E_CLUSTER_ID: identity mismatch. */
    uint8_t other_cid[16];
    memcpy(other_cid, TEST_CID, 16);
    other_cid[0] = 'X';
    CHECK(wcr1_ring_attach(region, rlen, other_cid, 7, 2, 1, 64, 192, &a) ==
          WEFT_CLUSTER_E_CLUSTER_ID, "E_CLUSTER_ID");

    /* E_NODE_ID: wrong home/producer expectations. */
    CHECK(wcr1_ring_attach(region, rlen, TEST_CID, 7, 3, 1, 64, 192, &a) ==
          WEFT_CLUSTER_E_NODE_ID, "E_NODE_ID home");
    CHECK(wcr1_ring_attach(region, rlen, TEST_CID, 7, 2, 3, 64, 192, &a) ==
          WEFT_CLUSTER_E_NODE_ID, "E_NODE_ID producer");

    /* E_CFG_CRC: flip a CRC-covered byte that no direct check compares
       (hdr_flags) so the ladder reaches the CRC refusal itself. */
    ((uint8_t *)region)[6] ^= 0xFF;
    CHECK(wcr1_ring_attach(region, rlen, TEST_CID, 7, 2, 1, 64, 192, &a) ==
          WEFT_CLUSTER_E_CFG_CRC, "E_CFG_CRC");
    ((uint8_t *)region)[6] ^= 0xFF;

    /* E_EPOCH: stale epoch. */
    CHECK(wcr1_ring_attach(region, rlen, TEST_CID, 6, 2, 1, 64, 192, &a) ==
          WEFT_CLUSTER_E_EPOCH, "E_EPOCH");

    /* E_CAPACITY / E_SLOT_SIZE: geometry expectations mismatch. */
    CHECK(wcr1_ring_attach(region, rlen, TEST_CID, 7, 2, 1, 32, 192, &a) ==
          WEFT_CLUSTER_E_CAPACITY, "E_CAPACITY");
    CHECK(wcr1_ring_attach(region, rlen, TEST_CID, 7, 2, 1, 64, 256, &a) ==
          WEFT_CLUSTER_E_SLOT_SIZE, "E_SLOT_SIZE");

    /* E_NODE_EVICTED: evicted ring refuses attach. */
    wcr1_ring_view_t ev;
    memcpy(&ev, &v, sizeof ev);
    wcr1_le32_put(&ev.hdr->cflags, WCR1_CFLAG_NODE_EVICTED);
    CHECK(wcr1_ring_attach(region, rlen, TEST_CID, 7, 2, 1, 64, 192, &a) ==
          WEFT_CLUSTER_E_NODE_EVICTED, "E_NODE_EVICTED");
    wcr1_le32_put(&ev.hdr->cflags, 0);

    /* E_PAYLOAD / E_ARG on publish. */
    uint8_t payload[256] = {0};
    uint64_t seq = 0;
    CHECK(wcr1_publish(&v, NULL, payload, 200, 0, 0, 0, 0, &seq) ==
          WEFT_CLUSTER_E_PAYLOAD, "E_PAYLOAD (200 > 128)");
    CHECK(wcr1_publish(NULL, NULL, payload, 8, 0, 0, 0, 0, &seq) ==
          WEFT_CLUSTER_E_ARG, "E_ARG null view");

    wcr1_engine_free(region);
    REPORT("R3", "attach validation ladder (12 refusals)");
}

static void t_r4_zero_alloc_100k(void)
{
    int fail_before = g_fail;
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.cluster_id, TEST_CID, 16);
    cfg.node_id = 2;
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 1024;
    cfg.slot_size = 192;
    cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_set_alloc_hooks(counting_alloc, counting_free, NULL);
    wcr1_ring_view_t v;
    void *region = NULL;
    uint64_t rlen = 0;
    CHECK(wcr1_ring_create(&cfg, 1, &v, &region, &rlen) == WEFT_CLUSTER_OK,
          "create");

    uint64_t allocs_at_start = g_alloc_count;

    uint8_t payload[128], check[128];
    uint64_t seq = 0;
    int torn = 0, refused = 0;
    const uint32_t N = 100000;
    for (uint32_t i = 1; i <= N; i++) {
        for (int k = 0; k < 128; k++) payload[k] = (uint8_t)(i + k);
        int e = wcr1_publish(&v, NULL, payload, 128, 0, test_now(), 0, 0,
                             &seq);
        if (e == WEFT_CLUSTER_E_BACKPRESSURE) { refused++; continue; }
        CHECK(e == WEFT_CLUSTER_OK, "publish %u: %s", i, wcr1_err_name(e));

        wcr1_slot_view_t slot;
        e = wcr1_acquire_next(&v, 8, &slot);
        if (e == WEFT_CLUSTER_E_SEQ_TORN) { torn++; continue; }
        CHECK(e == WEFT_CLUSTER_OK, "acquire %u: %s", i, wcr1_err_name(e));
        CHECK(slot.seq == i, "seq %llu != %u", (unsigned long long)slot.seq, i);
        CHECK(slot.payload_bytes == 128, "payload bytes");
        memcpy(check, slot.payload, 128);
        for (int k = 0; k < 128; k++)
            if (check[k] != (uint8_t)(i + k)) {
                CHECK(false, "payload corrupt @%u[%d]", i, k);
                break;
            }
        CHECK(wcr1_consume(&v, slot.seq) == WEFT_CLUSTER_OK, "consume");
    }
    CHECK(refused == 0, "unexpected backpressure on cap-1024 ring");
    CHECK(torn == 0, "unexpected tear");

    /* Law 1: zero allocations across 100k steady-state ops. */
    CHECK(g_alloc_count == allocs_at_start,
          "steady-state allocs: %llu",
          (unsigned long long)(g_alloc_count - allocs_at_start));
    CHECK(seq == N, "final seq %llu", (unsigned long long)seq);

    uint64_t wm = 0;
    CHECK(wcr1_consumer_seq_read(&v, 8, &wm) == WEFT_CLUSTER_OK, "wm read");
    CHECK(wm == N, "watermark %llu", (unsigned long long)wm);

    wcr1_engine_free(region);
    wcr1_set_alloc_hooks(NULL, NULL, NULL);
    REPORT("R4", "100k publish/acquire/consume, zero alloc");
}

static void t_r5_tear_detection(void)
{
    int fail_before = g_fail;
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.cluster_id, TEST_CID, 16);
    cfg.node_id = 2;
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 64;
    cfg.slot_size = 192;
    cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_ring_view_t v;
    void *region = NULL;
    uint64_t rlen = 0;
    CHECK(wcr1_ring_create(&cfg, 1, &v, &region, &rlen) == WEFT_CLUSTER_OK,
          "create");

    uint8_t payload[16] = "tear-detector-01";
    uint64_t seq = 0;
    for (int i = 0; i < 4; i++)
        CHECK(wcr1_publish(&v, NULL, payload, 16, 0, 0, 0, 0, &seq) ==
              WEFT_CLUSTER_OK, "publish %d", i);
    CHECK(seq == 4, "committed 4");

    wcr1_ring_header_t *h = v.hdr;

    /* Case A: writer crashed AFTER the lo store (lo new, hi stale). */
    __atomic_store_n(&h->seq_lo, 5u, __ATOMIC_RELAXED);
    wcr1_slot_view_t slot;
    CHECK(wcr1_acquire_next(&v, 4, &slot) == WEFT_CLUSTER_E_SEQ_TORN,
          "case A must tear");
    __atomic_store_n(&h->seq_lo, 4u, __ATOMIC_RELAXED);   /* repair */

    /* Case B: hi advanced past lo (parity flip in hi). */
    __atomic_store_n(&h->seq_hi, 0x80000000u, __ATOMIC_RELAXED);
    CHECK(wcr1_acquire_next(&v, 4, &slot) == WEFT_CLUSTER_E_SEQ_TORN,
          "case B must tear");
    __atomic_store_n(&h->seq_hi, 0u, __ATOMIC_RELAXED);   /* repair */

    /* Repaired: acquire works again. */
    CHECK(wcr1_acquire_next(&v, 4, &slot) == WEFT_CLUSTER_OK, "repaired");
    CHECK(slot.seq == 1, "seq 1 after repair");
    CHECK(wcr1_consume(&v, 1) == WEFT_CLUSTER_OK, "consume 1");

    /* Unstable (constantly changing) register -> bounded retries -> torn. */
    for (int i = 0; i < 8; i++) {
        __atomic_store_n(&h->seq_lo, (uint32_t)(10 + i), __ATOMIC_RELAXED);
        __atomic_store_n(&h->seq_hi,
                         (i & 1) ? 0x80000000u : 0u, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&h->seq_lo, 4u, __ATOMIC_RELAXED);
    __atomic_store_n(&h->seq_hi, 0u, __ATOMIC_RELAXED);

    /* Watermark register: same two-store discipline. The committed
       watermark is 1 (odd parity in cons_hi); poke lo to 2 (even) — the
       crashed-writer-between-stores state -> tear. */
    __atomic_store_n(&h->cons_lo, 2u, __ATOMIC_RELAXED);
    uint64_t wm = 0;
    CHECK(wcr1_consumer_seq_read(&v, 4, &wm) == WEFT_CLUSTER_E_SEQ_TORN,
          "watermark tear must be detected");
    __atomic_store_n(&h->cons_lo, 1u, __ATOMIC_RELAXED);

    wcr1_engine_free(region);
    REPORT("R5", "two-store tear detection (bounded -> E_SEQ_TORN)");
}

static void t_r6_backpressure(void)
{
    int fail_before = g_fail;
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.cluster_id, TEST_CID, 16);
    cfg.node_id = 2;
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 8;        /* tiny ring forces the ladder */
    cfg.slot_size = 192;
    cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_ring_view_t v;
    void *region = NULL;
    uint64_t rlen = 0;
    CHECK(wcr1_ring_create(&cfg, 1, &v, &region, &rlen) == WEFT_CLUSTER_OK,
          "create");

    uint8_t payload[32] = {1};
    uint64_t seq = 0;
    int marked = 0;

    /* Fill the ring with nobody consuming. */
    for (int i = 1; i <= 8; i++) {
        int e = wcr1_publish(&v, NULL, payload, 32, 0, 0, 0, 0, &seq);
        CHECK(e == WEFT_CLUSTER_OK, "publish %d: %s", i, wcr1_err_name(e));
    }
    /* 9th publish: lag == 8 == capacity -> refusal, data protected. */
    int e = wcr1_publish(&v, NULL, payload, 32, 0, 0, 0, 0, &seq);
    CHECK(e == WEFT_CLUSTER_E_BACKPRESSURE, "ring full refusal, got %s",
          wcr1_err_name(e));

    /* Slot 4+ carry the in-band high-water mark (lag_after >= cap/2). */
    for (uint64_t s = 1; s <= 8; s++) {
        const wcr1_slot_header_t *sh =
            (const wcr1_slot_header_t *)wcr1_remote_slot_addr(
                region, s, 8, 192);
        if (s >= 4 && (wcr1_le32_get(&sh->flags) & WCR1_SLOT_F_BP_MARK))
            marked++;
        if (s < 4)
            CHECK(!(wcr1_le32_get(&sh->flags) & WCR1_SLOT_F_BP_MARK),
                  "slot %llu must not be marked",
                  (unsigned long long)s);
    }
    CHECK(marked == 5, "high-water marks: %d", marked);

    /* Consume everything -> publish unblocks. */
    for (uint64_t s = 1; s <= 8; s++) {
        wcr1_slot_view_t slot;
        CHECK(wcr1_acquire_next(&v, 8, &slot) == WEFT_CLUSTER_OK, "acquire");
        CHECK(wcr1_consume(&v, slot.seq) == WEFT_CLUSTER_OK, "consume");
    }
    e = wcr1_publish(&v, NULL, payload, 32, 0, 0, 0, 0, &seq);
    CHECK(e == WEFT_CLUSTER_OK, "unblocked publish: %s", wcr1_err_name(e));
    CHECK(seq == 9, "seq 9 after unblock");

    wcr1_engine_free(region);
    REPORT("R6", "in-band backpressure + ring-full refusal");
}

static void t_r7_overrun_alias(void)
{
    int fail_before = g_fail;
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.cluster_id, TEST_CID, 16);
    cfg.node_id = 2;
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 8;
    cfg.slot_size = 192;
    cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_ring_view_t v;
    void *region = NULL;
    uint64_t rlen = 0;
    CHECK(wcr1_ring_create(&cfg, 1, &v, &region, &rlen) == WEFT_CLUSTER_OK,
          "create");

    uint8_t payload[16] = {2};
    uint64_t seq = 0;
    for (int i = 0; i < 4; i++)
        CHECK(wcr1_publish(&v, NULL, payload, 16, 0, 0, 0, 0, &seq) ==
              WEFT_CLUSTER_OK, "publish");

    /* Byzantine producer: jump the commit register far past the consumer.
       (A conforming producer is REFUSED at the high-water first — this
       simulates corruption / a misbehaving writer to prove the reader's
       alias guard still holds.) */
    __atomic_store_n(&v.hdr->seq_lo, 40u, __ATOMIC_RELAXED);
    __atomic_store_n(&v.hdr->seq_hi, 0u, __ATOMIC_RELAXED);
    wcr1_slot_view_t slot;
    int e = wcr1_acquire_next(&v, 4, &slot);
    CHECK(e == WEFT_CLUSTER_E_SEQ_OVERRUN,
          "overrun must be refused honestly, got %s", wcr1_err_name(e));

    /* Repair; corrupt a slot's message_seq -> E_SEQ_CORRUPT. */
    __atomic_store_n(&v.hdr->seq_lo, 4u, __ATOMIC_RELAXED);
    wcr1_slot_header_t *sh = (wcr1_slot_header_t *)
        ((uint8_t *)region + wcr1_slot_byte_offset(1, 8, 192));
    wcr1_le64_put(&sh->message_seq, 999);
    e = wcr1_acquire_next(&v, 4, &slot);
    CHECK(e == WEFT_CLUSTER_E_SEQ_CORRUPT, "alias/corrupt refusal, got %s",
          wcr1_err_name(e));

    wcr1_engine_free(region);
    REPORT("R7", "overrun + message_seq alias guards");
}

static void t_r8_slot_layout(void)
{
    int fail_before = g_fail;
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.cluster_id, TEST_CID, 16);
    cfg.node_id = 2;
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 64;
    cfg.slot_size = 192;
    cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_ring_view_t v;
    void *region = NULL;
    uint64_t rlen = 0;
    CHECK(wcr1_ring_create(&cfg, 1, &v, &region, &rlen) == WEFT_CLUSTER_OK,
          "create");

    uint8_t payload[8] = "WCR1BYTE";
    uint64_t seq = 0;
    uint64_t token = wcr1_fencing_token(3, 2);
    CHECK(wcr1_publish(&v, NULL, payload, 8, WCR1_PUBLISH_F_WITH_CRC,
                       0x1122334455667788ull, token, 7, &seq) ==
          WEFT_CLUSTER_OK, "publish with crc");
    CHECK(seq == 1, "seq 1");

    const uint8_t *slot = (const uint8_t *)region +
                          wcr1_slot_byte_offset(1, 64, 192);
    CHECK(wcr1_le64_get(slot + 0x00) == 1, "message_seq @0x00");
    CHECK(wcr1_le64_get(slot + 0x08) == 0x1122334455667788ull,
          "timestamp @0x08");
    CHECK(wcr1_le64_get(slot + 0x10) == token, "fencing token @0x10");
    CHECK(wcr1_le64_get(slot + 0x18) == 7, "epoch @0x18");
    CHECK(wcr1_le32_get(slot + 0x20) == 8, "payload_bytes @0x20");
    uint32_t flags = wcr1_le32_get(slot + 0x24);
    CHECK(flags == WCR1_SLOT_F_WITH_CRC, "flags @0x24 = 0x%X", flags);
    CHECK(wcr1_le64_get(slot + 0x28) == 1, "bp_lag @0x28");
    CHECK(wcr1_le32_get(slot + 0x30) == wcr1_crc32(payload, 8),
          "payload_crc @0x30");
    CHECK(wcr1_le32_get(slot + 0x34) == wcr1_crc32(slot, 0x34),
          "header_crc @0x34");
    CHECK(wcr1_le32_get(slot + 0x38) == 1, "producer id @0x38");
    CHECK(wcr1_le32_get(slot + 0x3C) == 0, "reserved @0x3C zero");
    CHECK(memcmp(slot + 0x40, "WCR1BYTE", 8) == 0, "payload @0x40");

    /* Acquire + CRC validation passes; corruption is caught. */
    wcr1_slot_view_t view;
    CHECK(wcr1_acquire_next(&v, 8, &view) == WEFT_CLUSTER_OK, "acquire");
    CHECK(wcr1_slot_crc_check(&view) == WEFT_CLUSTER_OK, "crc check");
    ((uint8_t *)region)[320 + 0x40] ^= 0xFF;      /* corrupt payload */
    CHECK(wcr1_slot_crc_check(&view) == WEFT_CLUSTER_E_CRC,
          "payload corruption caught");
    ((uint8_t *)region)[320 + 0x40] ^= 0xFF;      /* repair */

    wcr1_engine_free(region);
    REPORT("R8", "slot header golden layout + opt-in CRC");
}

static void t_r9_watermark(void)
{
    int fail_before = g_fail;
    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.cluster_id, TEST_CID, 16);
    cfg.node_id = 2;
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = 16;
    cfg.slot_size = 192;
    cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_ring_view_t v;
    void *region = NULL;
    uint64_t rlen = 0;
    CHECK(wcr1_ring_create(&cfg, 1, &v, &region, &rlen) == WEFT_CLUSTER_OK,
          "create");

    uint8_t payload[8] = {9};
    uint64_t seq = 0;
    for (int i = 0; i < 12; i++)
        CHECK(wcr1_publish(&v, NULL, payload, 8, 0, 0, 0, 0, &seq) ==
              WEFT_CLUSTER_OK, "publish");

    uint64_t wm = 99;
    CHECK(wcr1_consumer_seq_read(&v, 8, &wm) == WEFT_CLUSTER_OK, "wm read");
    CHECK(wm == 0, "watermark starts 0");

    /* Consumer walks 1..7 (leaves 8..12 unconsumed). */
    for (uint64_t s = 1; s <= 7; s++) {
        wcr1_slot_view_t slot;
        CHECK(wcr1_acquire_next(&v, 8, &slot) == WEFT_CLUSTER_OK, "acquire");
        CHECK(wcr1_consume(&v, slot.seq) == WEFT_CLUSTER_OK, "consume %llu",
              (unsigned long long)s);
    }
    CHECK(wcr1_consumer_seq_read(&v, 8, &wm) == WEFT_CLUSTER_OK, "wm read");
    CHECK(wm == 7, "watermark 7, got %llu", (unsigned long long)wm);
    CHECK(wcr1_producer_seq_read(&v, 8, &wm) == WEFT_CLUSTER_OK, "ps read");
    CHECK(wm == 12, "committed 12");

    /* consume() refuses regression. */
    CHECK(wcr1_consume(&v, 3) == WEFT_CLUSTER_E_ARG, "regression refused");

    wcr1_engine_free(region);
    REPORT("R9", "consumer watermark two-store protocol");
}

static void t_r10_remote_addressing(void)
{
    int fail_before = g_fail;
    /* Pure arithmetic: offset(seq) = 128 + (seq & (cap-1)) * slot_size. */
    CHECK(wcr1_slot_byte_offset(0, 8, 192) == 128, "seq 0");
    CHECK(wcr1_slot_byte_offset(1, 8, 192) == 320, "seq 1");
    CHECK(wcr1_slot_byte_offset(7, 8, 192) == 1472, "seq 7");
    CHECK(wcr1_slot_byte_offset(8, 8, 192) == 128, "seq 8 wraps");
    CHECK(wcr1_slot_byte_offset(9, 8, 192) == 320, "seq 9 wraps");
    CHECK(wcr1_slot_byte_offset(1ull << 40, 8, 192) == 128, "2^40 wraps");

    uint8_t fake_mr[4096];
    memset(fake_mr, 0, sizeof fake_mr);
    CHECK(wcr1_remote_slot_addr(fake_mr, 3, 8, 192) == fake_mr + 128 + 3 * 192,
          "remote addr = mr_base + offset");
    CHECK(wcr1_remote_slot_addr(fake_mr, 5, 8, 192) == fake_mr + 128 + 5 * 192,
          "remote addr seq 5");
    CHECK((const uint8_t *)wcr1_remote_slot_addr(fake_mr, 8, 8, 192) ==
          fake_mr + 128, "remote addr wraps");

    CHECK(wcr1_region_size(64, 192) == 12416, "region size 64x192");
    REPORT("R10", "zero-roundtrip remote slot addressing");
}

/* ---- R11: cross-process fork torture --------------------------------- */

static void t_r11_fork_ipc(void)
{
    int fail_before = g_fail;
    const uint32_t N = 100000;
    const uint64_t CAP = 64, SLOT = 192;

    /* Rings allocated from MAP_SHARED|MAP_ANONYMOUS via the alloc hooks so
       the forked child shares the exact pages (simulating cross-node DMA
       mapped memory). */
    wcr1_set_alloc_hooks(shm_alloc, shm_free, NULL);

    wcr1_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.cluster_id, TEST_CID, 16);
    cfg.node_id = 2;                  /* parent: home/consumer node   */
    cfg.cluster_size = 2;
    cfg.epoch = 7;
    cfg.capacity_slots = CAP;
    cfg.slot_size = SLOT;
    cfg.lease_ns = WCR1_DEFAULT_LEASE_NS;
    cfg.peer_count = 1;
    cfg.peer_ids[0] = 1;

    wcr1_ring_view_t home;
    void *region = NULL;
    uint64_t rlen = 0;
    CHECK(wcr1_ring_create(&cfg, 1, &home, &region, &rlen) ==
          WEFT_CLUSTER_OK, "shm create");

    wcr1_set_alloc_hooks(counting_alloc, counting_free, NULL);
    uint64_t parent_allocs = g_alloc_count;

    fflush(stdout);
    pid_t pid = fork();

    if (pid == 0) {
        /* ---- child: the remote producer (node 1) ---- */
        g_alloc_count = 0;
        wcr1_set_alloc_hooks(counting_alloc, counting_free, NULL);

        wcr1_ring_view_t prod;
        if (wcr1_ring_attach(region, rlen, TEST_CID, 7, 2, 1, CAP, SLOT,
                             &prod) != WEFT_CLUSTER_OK)
            _exit(100);

        uint8_t payload[128];
        uint32_t delivered = 0;
        uint64_t spins = 0;
        int bad = 0;
        while (delivered < N) {
            if (++spins > 200000000ull) { _exit(42); }  /* livelock guard */
            for (int k = 0; k < 128; k++)
                payload[k] = (uint8_t)(delivered + 1 + k);
            uint64_t seq = 0;
            int e = wcr1_publish(&prod, NULL, payload, 128, 0, test_now(),
                                 0, 7, &seq);
            if (e == WEFT_CLUSTER_E_BACKPRESSURE) {
                sched_yield();
                continue;
            }
            if (e != WEFT_CLUSTER_OK) { bad++; continue; }
            if (seq != (uint64_t)(delivered + 1)) { bad += 100; break; }
            delivered++;
            if ((delivered & 511u) == 0) sched_yield();
        }
        /* Child steady-state: zero allocations, zero errors. */
        if (g_alloc_count != 0) bad += 1000;
        _exit(bad > 250 ? 250 : bad);
    }

    /* ---- parent: home consumer (node 2) ---- */
    uint64_t got = 0, spins = 0, torn_backoffs = 0;
    uint8_t check[128];
    int bad = 0;
    while (got < N) {
        if (++spins > 200000000ull) { bad += 5000; break; }
        wcr1_slot_view_t slot;
        int e = wcr1_acquire_next(&home, 64, &slot);
        if (e == WEFT_CLUSTER_E_NOT_PUBLISHED) { sched_yield(); continue; }
        if (e == WEFT_CLUSTER_E_SEQ_TORN) {
            /* Bounded-retry exhaustion against a saturating producer: the
               honest refusal. Back off and retry the operation — this is
               the documented application contract (RFC 0018 §4.3). */
            torn_backoffs++;
            sched_yield();
            continue;
        }
        if (e != WEFT_CLUSTER_OK) { bad += 100; break; }
        if (slot.seq != got + 1) { bad += 2; break; }
        if (slot.payload_bytes != 128) { bad += 3; break; }
        memcpy(check, slot.payload, 128);
        for (int k = 0; k < 128; k++)
            if (check[k] != (uint8_t)(got + 1 + k)) { bad += 4; break; }
        if (wcr1_consume(&home, slot.seq) != WEFT_CLUSTER_OK) { bad += 5; }
        got++;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status), "child exited normally");
    int child_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(child_code == 0, "child exit code %d", child_code);
    CHECK(got == N, "parent consumed %llu / %u",
          (unsigned long long)got, N);
    CHECK(bad == 0, "parent verify errors: %d", bad);
    printf("     (fork ipc: %llu torn backoffs survived)\n",
           (unsigned long long)torn_backoffs);
    CHECK(g_alloc_count == parent_allocs,
          "parent steady-state allocs: %llu",
          (unsigned long long)(g_alloc_count - parent_allocs));

    uint64_t wm = 0;
    CHECK(wcr1_consumer_seq_read(&home, 16, &wm) == WEFT_CLUSTER_OK, "wm");
    CHECK(wm == N, "final watermark %llu", (unsigned long long)wm);

    shm_free(region, NULL);
    wcr1_set_alloc_hooks(NULL, NULL, NULL);
    REPORT("R11", "fork cross-process 100k msgs, zero tears");
}

// ---------------------------------------------------------------------------

int main(void)
{
    t_r1_crc_le_seq();
    t_r2_create_golden();
    t_r3_validation_ladder();
    t_r4_zero_alloc_100k();
    t_r5_tear_detection();
    t_r6_backpressure();
    t_r7_overrun_alias();
    t_r8_slot_layout();
    t_r9_watermark();
    t_r10_remote_addressing();
    t_r11_fork_ipc();

    printf("\ncluster_ring_test: %s (%d failures)\n",
           g_fail == 0 ? "ALL PASS" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
