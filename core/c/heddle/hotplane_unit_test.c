// hotplane_unit_test.c — WHP2 conformance suite (Pillar 4, H-series).
//
// Gates (each prints PASS/FAIL; non-zero exit on any failure):
//   H1  CRC-32/IEEE check value + LE helper round-trips
//   H2  create/attach round-trip + byte-level identity checks
//   H3  determinism: two independently created planes are byte-identical
//   H4  the full attach validation ladder (12 distinct refusals)
//   H5  session state machine (double-open / close-without-open / ...)
//   H6  state write/read round-trips + bounds ladder
//   H7  torn-read REFUSAL while a session is held open (E_SEQ_TORN)
//   H8  dirty mask lifecycle (harvest/reset/remark/refuse)
//   H9  bounding-box registers (expand/harvest/reset)
//   H10 lane stats across u64 / i64 / f64 kinds
//   H11 ring mode push/read/ordinal semantics
//   H12 ring overrun + in-band backpressure marks
//   H13 multi-producer lane sessions + mode refusals
//   H14 lane-session misuse ladder
//   H15 frame protocol + role gates
//   H16 zero-heap steady state (mallinfo2/sbrk deltas == 0, plain build)
//   H17 100k+ continuous single-producer updates, read-after-commit
//   H18 fork cross-process MAP_SHARED: 100k msgs, zero tears

#include "heddle_hotplane_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#  if __GLIBC_PREREQ(2, 33)
#    include <malloc.h>
#    define HPLANE_HAVE_MALLINFO2 1
#  endif
#endif

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name)                                                  \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s (line %d)\n", (name), __LINE__);      \
            failures++;                                                    \
        } else {                                                           \
            printf("PASS %s\n", (name));                                   \
        }                                                                  \
    } while (0)

#define FIXED_STAMP 1234567890123ull

static void *alloc64(size_t n)
{
    void *p = NULL;
    if (posix_memalign(&p, 64, n) != 0) {
        abort();
    }
    memset(p, 0xAA, n);
    return p;
}

// --- H1 -------------------------------------------------------------------

static void h1_crc_and_le(void)
{
    CHECK(hplane_crc32("123456789", 9) == 0xCBF43926u,
          "H1 crc32 check value 0xCBF43926");

    uint8_t b[8];
    hplane_le16_put(b, 0xABCD);
    CHECK(b[0] == 0xCD && b[1] == 0xAB, "H1 le16 byte order");
    CHECK(hplane_le16_get(b) == 0xABCD, "H1 le16 round-trip");
    hplane_le32_put(b, 0x11223344u);
    CHECK(b[0] == 0x44 && b[1] == 0x33 && b[2] == 0x22 && b[3] == 0x11,
          "H1 le32 byte order");
    CHECK(hplane_le32_get(b) == 0x11223344u, "H1 le32 round-trip");
    hplane_le64_put(b, 0x0102030405060708ull);
    CHECK(b[0] == 0x08 && b[7] == 0x01, "H1 le64 byte order");
    CHECK(hplane_le64_get(b) == 0x0102030405060708ull, "H1 le64 round-trip");

    CHECK(hplane_align64(1) == 64 && hplane_align64(64) == 64 &&
          hplane_align64(65) == 128, "H1 align64");
    CHECK(hplane_slot_stride(8) == 64 && hplane_slot_stride(48) == 64 &&
          hplane_slot_stride(49) == 128, "H1 slot stride derivation");
    CHECK(hplane_cell_stride(8) == 64 && hplane_cell_stride(64) == 64 &&
          hplane_cell_stride(65) == 128, "H1 cell stride derivation");
}

// --- H2 / H3 --------------------------------------------------------------

static void h2_create_attach(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_STATE, 4, 8, 16, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    CHECK(sz == 128 + 4 * 64 + 4 * 16 * 64, "H2 plane size arithmetic");

    uint8_t *mem = alloc64(sz);
    int rc = hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);
    CHECK(rc == HEDDLE_OK, "H2 create ok");

    /* Byte-level identity: magic is the u32 0x57485032, serialized
     * little-endian -> bytes 32 50 48 57 at +0x00. */
    CHECK(mem[0] == 0x32 && mem[1] == 0x50 && mem[2] == 0x48 && mem[3] == 0x57,
          "H2 magic bytes LE at +0x00");
    CHECK(mem[4] == 2 && mem[5] == 0, "H2 version 2.0 at +0x04");
    CHECK(mem[8] == 128 && mem[9] == 0 && mem[10] == 0 && mem[11] == 0,
          "H2 header_size 128 at +0x08");

    hplane_ctx_t c;
    rc = hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER);
    CHECK(rc == HEDDLE_OK, "H2 attach ok");
    CHECK(c.mode == WHP2_MODE_STATE && c.lane_count == 4 &&
          c.sample_size == 8 && c.slot_capacity == 16 &&
          c.lane_stride == 16 * 64, "H2 attach decoded config");
    CHECK(hplane_epoch_get(&c) == 0, "H2 fresh epoch 0");

    CHECK(hplane_validate(mem, sz) == HEDDLE_OK, "H2 validate ok");

    /* Determinism: a second, independently created plane is identical
     * byte-for-byte (whole region, deterministic zeroing + fields). */
    uint8_t *mem2 = alloc64(sz);
    CHECK(hplane_plane_create(mem2, sz, &cfg, FIXED_STAMP) == HEDDLE_OK,
          "H2 second create ok");
    CHECK(memcmp(mem, mem2, sz) == 0, "H2 byte-identical determinism");

    free(mem);
    free(mem2);
}

// --- H4 validation ladder --------------------------------------------------

static void h4_ladder(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_STATE, 4, 8, 16, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *good = alloc64(sz);
    uint8_t *scratch = alloc64(sz);
    hplane_plane_create(good, sz, &cfg, FIXED_STAMP);
    hplane_ctx_t ctx;

    CHECK(hplane_attach(NULL, sz, &ctx, WHP2_ROLE_CONSUMER) == HEDDLE_E_ARG,
          "H4 null mem -> E_ARG");
    CHECK(hplane_attach(good, 16, &ctx, WHP2_ROLE_CONSUMER) == HEDDLE_E_ARG,
          "H4 tiny len -> E_ARG");
    CHECK(hplane_attach(good, sz, NULL, WHP2_ROLE_CONSUMER) == HEDDLE_E_ARG,
          "H4 null ctx -> E_ARG");
    CHECK(hplane_attach(good, sz, &ctx, 0) == HEDDLE_E_ARG,
          "H4 bad role 0 -> E_ARG");
    CHECK(hplane_attach(good, sz, &ctx, 7) == HEDDLE_E_ARG,
          "H4 bad role 7 -> E_ARG");
    {
        void *misaligned = (void *)((uint8_t *)scratch + 8);
        CHECK(hplane_attach(misaligned, sz, &ctx, WHP2_ROLE_CONSUMER) ==
              HEDDLE_E_ARG, "H4 misaligned mem -> E_ARG");
    }

    memcpy(scratch, good, sz);
    scratch[0] ^= 0xFF;   /* magic */
    CHECK(hplane_validate(scratch, sz) == HEDDLE_E_MAGIC,
          "H4 bad magic -> E_MAGIC");

    memcpy(scratch, good, sz);
    scratch[4] = 9;       /* ver_major */
    CHECK(hplane_validate(scratch, sz) == HEDDLE_E_VERSION,
          "H4 bad major -> E_VERSION");

    memcpy(scratch, good, sz);
    hplane_le32_put(scratch + 8, 256);   /* header_size, outside CRC */
    CHECK(hplane_validate(scratch, sz) == HEDDLE_E_LAYOUT,
          "H4 bad header_size -> E_LAYOUT");

    memcpy(scratch, good, sz);
    hplane_le32_put(scratch + 0x10, 2);  /* layout_rev (inside CRC: re-CRC) */
    hplane_le32_put(scratch + 0x0C,
                    hplane_crc32(scratch + 0x10, 0x30));
    CHECK(hplane_validate(scratch, sz) == HEDDLE_E_LAYOUT,
          "H4 bad layout_rev -> E_LAYOUT");

    memcpy(scratch, good, sz);
    hplane_le32_put(scratch + 0x70, 0xDEADC0DEu);   /* canary */
    CHECK(hplane_validate(scratch, sz) == HEDDLE_E_ENDIAN,
          "H4 flipped canary -> E_ENDIAN");

    memcpy(scratch, good, sz);
    scratch[0x20] ^= 0x01;              /* sample_size byte, inside CRC */
    CHECK(hplane_validate(scratch, sz) == HEDDLE_E_CFG_CRC,
          "H4 static corruption -> E_CFG_CRC");

    /* Geometry lie with a self-consistent CRC. */
    memcpy(scratch, good, sz);
    hplane_le32_put(scratch + 0x1C, 16 * 64 + 64);   /* lane_stride lie */
    hplane_le32_put(scratch + 0x0C, hplane_crc32(scratch + 0x10, 0x30));
    CHECK(hplane_validate(scratch, sz) == HEDDLE_E_LAYOUT,
          "H4 self-consistent geometry lie -> E_LAYOUT");

    memcpy(scratch, good, sz);
    hplane_le32_put(scratch + 0x18, 0);             /* lane_count 0 */
    hplane_le32_put(scratch + 0x0C, hplane_crc32(scratch + 0x10, 0x30));
    CHECK(hplane_validate(scratch, sz) == HEDDLE_E_CAPACITY,
          "H4 lane_count 0 -> E_CAPACITY");

    CHECK(hplane_validate(good, sz - 1) == HEDDLE_E_REGION_SIZE,
          "H4 short region -> E_REGION_SIZE");

    /* create-time config ladder */
    hplane_cfg_t bad = cfg;
    bad.mode = 5;
    CHECK(hplane_plane_create(scratch, sz, &bad, FIXED_STAMP) == HEDDLE_E_MODE,
          "H4 create bad mode -> E_MODE");
    bad = cfg;
    bad.lane_count = 65;
    CHECK(hplane_plane_create(scratch, sz, &bad, FIXED_STAMP) ==
          HEDDLE_E_CAPACITY, "H4 create 65 lanes -> E_CAPACITY");
    bad = cfg;
    bad.sample_size = 0;
    CHECK(hplane_plane_create(scratch, sz, &bad, FIXED_STAMP) ==
          HEDDLE_E_PAYLOAD, "H4 create sample_size 0 -> E_PAYLOAD");
    bad = cfg;
    bad.slot_capacity = 1;   /* ring needs >= 2; this cfg is state -> ok */
    CHECK(hplane_plane_create(scratch, sz, &bad, FIXED_STAMP) == HEDDLE_OK,
          "H4 state capacity 1 legal");
    bad.mode = WHP2_MODE_RING;
    CHECK(hplane_plane_create(scratch, sz, &bad, FIXED_STAMP) ==
          HEDDLE_E_CAPACITY, "H4 ring capacity 1 -> E_CAPACITY");
    bad = cfg;
    bad.flags = 1u << 31;
    CHECK(hplane_plane_create(scratch, sz, &bad, FIXED_STAMP) ==
          HEDDLE_E_UNSUPPORTED, "H4 unknown flag -> E_UNSUPPORTED");
    bad = cfg;
    bad.stat_kind = 3;
    CHECK(hplane_plane_create(scratch, sz, &bad, FIXED_STAMP) == HEDDLE_E_ARG,
          "H4 stat_kind 3 -> E_ARG");
    CHECK(hplane_plane_create(scratch, sz - 1, &cfg, FIXED_STAMP) ==
          HEDDLE_E_REGION_SIZE, "H4 create short region -> E_REGION_SIZE");

    free(good);
    free(scratch);
}

// --- H5/H6/H7 state plane --------------------------------------------------

static void h5_state_plane(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_STATE, 4, 8, 16, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = alloc64(sz);
    hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);

    hplane_ctx_t p, c;
    CHECK(hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER) == HEDDLE_OK,
          "H5 producer attach");
    CHECK(hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER) == HEDDLE_OK,
          "H5 consumer attach");

    CHECK(hplane_commit_end(&p) == HEDDLE_E_STATE,
          "H5 end without begin -> E_STATE");
    CHECK(hplane_commit_begin(&p) == HEDDLE_OK, "H5 begin ok");
    CHECK(hplane_commit_begin(&p) == HEDDLE_E_STATE,
          "H5 double begin -> E_STATE");

    uint64_t v = 0x1111222233334444ull;
    CHECK(hplane_state_write(&p, 0, 3, &v, 8) == HEDDLE_OK,
          "H5 state write in session");

    /* Consumer must REFUSE while the session is in flight. */
    uint64_t out = 0;
    CHECK(hplane_cell_read(&c, 0, 3, &out, 8, 4) == HEDDLE_E_SEQ_TORN,
          "H5 torn read refused while session open (E_SEQ_TORN)");

    CHECK(hplane_commit_end(&p) == HEDDLE_OK, "H5 commit ok");
    CHECK(hplane_epoch_get(&c) == 1, "H5 epoch 1 after one session");

    CHECK(hplane_cell_read(&c, 0, 3, &out, 8, 64) == HEDDLE_OK,
          "H5 cell read after commit");
    CHECK(out == v, "H5 cell round-trip value");

    /* Unwritten cell reads as zero (deterministic create). */
    out = 99;
    CHECK(hplane_cell_read(&c, 1, 9, &out, 8, 64) == HEDDLE_OK &&
          out == 0, "H5 unwritten cell reads zero");

    /* Bounds ladder. */
    CHECK(hplane_commit_begin(&p) == HEDDLE_OK, "H5 second session begin");
    CHECK(hplane_state_write(&p, 4, 0, &v, 8) == HEDDLE_E_LANE_OVERFLOW,
          "H5 lane 4 -> E_LANE_OVERFLOW");
    CHECK(hplane_state_write(&p, 0, 16, &v, 8) == HEDDLE_E_CAPACITY,
          "H5 cell 16 -> E_CAPACITY");
    CHECK(hplane_state_write(&p, 0, 0, &v, 9) == HEDDLE_E_PAYLOAD,
          "H5 len 9 > sample -> E_PAYLOAD");
    CHECK(hplane_state_write(&p, 0, 0, &v, 0) == HEDDLE_E_PAYLOAD,
          "H5 len 0 -> E_PAYLOAD");
    CHECK(hplane_state_write(&p, 0, 0, NULL, 8) == HEDDLE_E_ARG,
          "H5 null val -> E_ARG");
    CHECK(hplane_state_write(&p, 0, 1, &v, 8) == HEDDLE_OK, "H5 write cell 1");
    CHECK(hplane_commit_end(&p) == HEDDLE_OK, "H5 second commit");

    CHECK(hplane_cell_read(&c, 4, 0, &out, 8, 64) == HEDDLE_E_LANE_OVERFLOW,
          "H5 read lane 4 -> E_LANE_OVERFLOW");
    CHECK(hplane_cell_read(&c, 0, 16, &out, 8, 64) == HEDDLE_E_CAPACITY,
          "H5 read cell 16 -> E_CAPACITY");
    CHECK(hplane_cell_read(&c, 0, 0, &out, 9, 64) == HEDDLE_E_PAYLOAD,
          "H5 read len 9 -> E_PAYLOAD");
    CHECK(hplane_cell_read(&c, 0, 0, &out, 0, 64) == HEDDLE_E_PAYLOAD,
          "H5 read len 0 -> E_PAYLOAD");

    /* Consumer-role gate on mutation. */
    CHECK(hplane_state_write(&c, 0, 0, &v, 8) == HEDDLE_E_STATE,
          "H5 consumer ctx cannot mutate -> E_STATE");
    /* Producer-role gate on harvest. */
    uint64_t mask = 1;
    CHECK(hplane_dirty_harvest(&p, &mask) == HEDDLE_E_STATE,
          "H5 producer ctx cannot harvest -> E_STATE");

    /* Mode gate: cell ops refuse on ring planes (H11 covers ring). */
    free(mem);
}

// --- H8/H9 dirty + bbox ----------------------------------------------------

static void h8_dirty_bbox(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_STATE, 8, 8, 16, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = alloc64(sz);
    hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);
    hplane_ctx_t p, c;
    hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER);
    hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER);

    uint64_t v = 7;
    hplane_commit_begin(&p);
    hplane_state_write(&p, 0, 3, &v, 8);
    hplane_state_write(&p, 2, 7, &v, 8);
    hplane_state_write(&p, 5, 1, &v, 8);
    hplane_commit_end(&p);

    uint64_t mask = 0;
    CHECK(hplane_dirty_harvest(&c, &mask) == HEDDLE_OK &&
          mask == ((1u << 0) | (1u << 2) | (1u << 5)),
          "H8 harvest exact mask 0b1010001");

    mask = 0xDEAD;
    CHECK(hplane_dirty_harvest(&c, &mask) == HEDDLE_OK && mask == 0,
          "H8 second harvest empty");

    CHECK(hplane_dirty_remark(&c, (1u << 1)) == HEDDLE_OK, "H8 remark ok");
    CHECK(hplane_dirty_harvest(&c, &mask) == HEDDLE_OK && mask == (1u << 1),
          "H8 remark visible on next harvest");
    CHECK(hplane_dirty_remark(&c, (1u << 8)) == HEDDLE_E_LANE_OVERFLOW,
          "H8 remark bit beyond lane_count -> E_LANE_OVERFLOW");

    /* Bounding boxes. */
    uint64_t bbox = 0;
    CHECK(hplane_bbox_harvest(&c, 0, &bbox) == HEDDLE_OK &&
          bbox == (((uint64_t)3 << 32) | 3), "H8 lane0 bbox [3,3]");
    CHECK(hplane_bbox_harvest(&c, 2, &bbox) == HEDDLE_OK &&
          bbox == (((uint64_t)7 << 32) | 7), "H8 lane2 bbox [7,7]");
    CHECK(hplane_bbox_harvest(&c, 5, &bbox) == HEDDLE_OK &&
          bbox == (((uint64_t)1 << 32) | 1), "H8 lane5 bbox [1,1]");

    hplane_commit_begin(&p);
    hplane_state_write(&p, 1, 9, &v, 8);
    hplane_state_write(&p, 1, 2, &v, 8);
    hplane_state_write(&p, 1, 13, &v, 8);
    hplane_commit_end(&p);
    CHECK(hplane_bbox_harvest(&c, 1, &bbox) == HEDDLE_OK &&
          bbox == (((uint64_t)2 << 32) | 13), "H8 lane1 bbox [2,13]");
    /* V2 protocol: the register is producer-owned — a repeated consumer
     * harvest (no frame advance) still sees the same range. */
    CHECK(hplane_bbox_harvest(&c, 1, &bbox) == HEDDLE_OK &&
          bbox == (((uint64_t)2 << 32) | 13),
          "H8 bbox persists until the frame advances");
    /* Frame advance + producer's next session start => reset + re-seed. */
    uint64_t fid = 0;
    CHECK(hplane_frame_commit(&c, &fid) == HEDDLE_OK && fid == 1,
          "H8 frame 1 committed");
    hplane_commit_begin(&p);
    hplane_state_write(&p, 1, 5, &v, 8);
    hplane_commit_end(&p);
    CHECK(hplane_bbox_harvest(&c, 1, &bbox) == HEDDLE_OK &&
          bbox == (((uint64_t)5 << 32) | 5),
          "H8 bbox producer-reset on frame advance -> [5,5]");

    CHECK(hplane_bbox_harvest(&c, 8, &bbox) == HEDDLE_E_LANE_OVERFLOW,
          "H8 bbox lane 8 -> E_LANE_OVERFLOW");

    free(mem);
}

// --- H10 stats -------------------------------------------------------------

static void h10_stats(void)
{
    /* u64 kind */
    hplane_cfg_t cfg = { WHP2_MODE_STATE, 2, 8, 8, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = alloc64(sz);
    hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);
    hplane_ctx_t p, c;
    hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER);
    hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER);

    hplane_lane_stats_t st;
    CHECK(hplane_lane_stats_get(&c, 0, &st, 64) == HEDDLE_OK &&
          st.commit_count == 0, "H10 fresh lane commit_count 0");

    hplane_commit_begin(&p);
    uint64_t vals[3] = { 5, 100, 1 };
    for (int i = 0; i < 3; i++) {
        hplane_state_write(&p, 0, (uint32_t)i, &vals[i], 8);
    }
    hplane_commit_end(&p);
    CHECK(hplane_lane_stats_get(&c, 0, &st, 64) == HEDDLE_OK &&
          st.min_raw == 1 && st.max_raw == 100 && st.current_raw == 1 &&
          st.commit_count == 1, "H10 u64 min/max/current/commits");
    free(mem);

    /* i64 kind: negatives order below positives. */
    cfg = (hplane_cfg_t){ WHP2_MODE_STATE, 1, 8, 8, WHP2_STAT_I64, 0 };
    sz = (size_t)hplane_plane_size(&cfg);
    mem = alloc64(sz);
    hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);
    hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER);
    hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER);
    hplane_commit_begin(&p);
    int64_t ivals[4] = { -2, 7, -1000000, 0 };
    for (int i = 0; i < 4; i++) {
        hplane_state_write(&p, 0, (uint32_t)i, &ivals[i], 8);
    }
    hplane_commit_end(&p);
    CHECK(hplane_lane_stats_get(&c, 0, &st, 64) == HEDDLE_OK &&
          (int64_t)st.min_raw == -1000000 && (int64_t)st.max_raw == 7,
          "H10 i64 signed order");
    free(mem);

    /* f64 kind: IEEE-754 total order incl. negatives. */
    cfg = (hplane_cfg_t){ WHP2_MODE_STATE, 1, 8, 8, WHP2_STAT_F64, 0 };
    sz = (size_t)hplane_plane_size(&cfg);
    mem = alloc64(sz);
    hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);
    hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER);
    hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER);
    hplane_commit_begin(&p);
    double fvals[4] = { -2.5, 3.5, 1.0, -0.125 };
    for (int i = 0; i < 4; i++) {
        hplane_state_write(&p, 0, (uint32_t)i, &fvals[i], 8);
    }
    hplane_commit_end(&p);
    CHECK(hplane_lane_stats_get(&c, 0, &st, 64) == HEDDLE_OK &&
          memcmp(&st.min_raw, &(double){ -2.5 }, 8) == 0 &&
          memcmp(&st.max_raw, &(double){ 3.5 }, 8) == 0,
          "H10 f64 total order");
    free(mem);
}

// --- H11/H12 ring ----------------------------------------------------------

static void h11_ring(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_RING, 2, 8, 8, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = alloc64(sz);
    hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);
    hplane_ctx_t p, c;
    hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER);
    hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER);

    CHECK(hplane_ring_push(&p, 0, &(uint64_t){ 1 }, 8) == HEDDLE_E_STATE,
          "H11 push outside session -> E_STATE");

    hplane_commit_begin(&p);
    for (uint64_t n = 1; n <= 6; n++) {
        uint64_t s = n * 100;
        CHECK(hplane_ring_push(&p, 0, &s, 8) == HEDDLE_OK, "H11 push");
    }
    hplane_commit_end(&p);

    uint64_t head = 0;
    CHECK(hplane_ring_head(&c, 0, &head) == HEDDLE_OK && head == 6,
          "H11 head == pushes");
    uint64_t out = 0;
    for (uint64_t n = 1; n <= 6; n++) {
        CHECK(hplane_slot_read(&c, 0, n, &out, 8, 64) == HEDDLE_OK &&
              out == n * 100, "H11 slot ordinal round-trip");
    }
    CHECK(hplane_slot_read(&c, 0, 7, &out, 8, 64) == HEDDLE_E_STATE,
          "H11 ordinal 7 maps to an unwritten slot -> E_STATE");
    CHECK(hplane_slot_read(&c, 0, 0, &out, 8, 64) == HEDDLE_E_ARG,
          "H11 ordinal 0 -> E_ARG");
    CHECK(hplane_slot_read(&c, 1, 1, &out, 8, 64) == HEDDLE_E_STATE,
          "H11 untouched lane slot -> E_STATE");
    CHECK(hplane_cell_read(&c, 0, 0, &out, 8, 64) == HEDDLE_E_MODE,
          "H11 cell op on ring -> E_MODE");

    /* Lane stats on a ring plane: current == head. */
    hplane_lane_stats_t st;
    CHECK(hplane_lane_stats_get(&c, 0, &st, 64) == HEDDLE_OK &&
          st.current_raw == 6, "H11 ring lane current == head");
    free(mem);
}

static void h12_overrun(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_RING, 1, 8, 4, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = alloc64(sz);
    hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);
    hplane_ctx_t p, c;
    hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER);
    hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER);

    /* capacity 4; push 10 (2 full cycles + 2). */
    hplane_commit_begin(&p);
    for (uint64_t n = 1; n <= 10; n++) {
        uint64_t s = n;
        hplane_ring_push(&p, 0, &s, 8);
    }
    hplane_commit_end(&p);

    uint64_t out = 0;
    CHECK(hplane_slot_read(&c, 0, 1, &out, 8, 64) == HEDDLE_E_OVERRUN,
          "H12 ordinal 1 lost to wrap -> E_OVERRUN");
    CHECK(hplane_slot_read(&c, 0, 6, &out, 8, 64) == HEDDLE_E_OVERRUN,
          "H12 ordinal 6 lost to wrap -> E_OVERRUN");
    /* Ordinal 13 maps to slot 0, which holds ordinal 9: future ordinal
     * in a wrapped slot -> honest E_NOT_PUBLISHED. */
    CHECK(hplane_slot_read(&c, 0, 13, &out, 8, 64) == HEDDLE_E_NOT_PUBLISHED,
          "H12 future ordinal in wrapped slot -> E_NOT_PUBLISHED");
    for (uint64_t n = 7; n <= 10; n++) {
        CHECK(hplane_slot_read(&c, 0, n, &out, 8, 64) == HEDDLE_OK &&
              out == n, "H12 surviving window readable");
    }

    hplane_lane_desc_t *d =
        (hplane_lane_desc_t *)(void *)(mem + WHP2_HEADER_SIZE);
    uint32_t lflags = __atomic_load_n(&d->lane_flags, __ATOMIC_RELAXED);
    uint32_t misc = __atomic_load_n(&d->lane_misc, __ATOMIC_RELAXED);
    CHECK((lflags & WHP2_LANE_F_BP_MARK) != 0 &&
          (lflags & WHP2_LANE_F_OVERRUN) != 0,
          "H12 in-band backpressure marks set");
    CHECK((misc & 0xFFFFu) == 2, "H12 overrun cycle count == 2");

    uint64_t head = 0;
    hplane_ring_head(&c, 0, &head);
    CHECK(head == 10, "H12 head 10");
    free(mem);
}

// --- H13/H14 multi-producer ------------------------------------------------

static void h13_multi(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_STATE, 4, 8, 8, WHP2_STAT_U64,
                         WHP2_F_MULTI_PRODUCER };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = alloc64(sz);
    hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);
    hplane_ctx_t p1, p2, c;
    hplane_attach(mem, sz, &p1, WHP2_ROLE_PRODUCER);
    hplane_attach(mem, sz, &p2, WHP2_ROLE_PRODUCER);
    hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER);

    CHECK(hplane_commit_begin(&p1) == HEDDLE_E_MODE,
          "H13 global session on multi plane -> E_MODE");

    CHECK(hplane_lane_begin(&p1, 0) == HEDDLE_OK, "H13 lane0 begin");
    CHECK(hplane_lane_begin(&p1, 1) == HEDDLE_E_STATE,
          "H13 second concurrent lane session -> E_STATE");
    uint64_t v = 42;
    CHECK(hplane_state_write(&p1, 1, 0, &v, 8) == HEDDLE_E_STATE,
          "H13 write to non-open lane -> E_STATE");
    CHECK(hplane_state_write(&p1, 0, 2, &v, 8) == HEDDLE_OK, "H13 lane0 write");
    CHECK(hplane_lane_end(&p1, 1) == HEDDLE_E_STATE,
          "H13 end wrong lane -> E_STATE");
    CHECK(hplane_lane_end(&p1, 0) == HEDDLE_OK, "H13 lane0 end");

    /* Second producer takes another lane concurrently. */
    CHECK(hplane_lane_begin(&p2, 3) == HEDDLE_OK, "H13 lane3 begin (p2)");
    CHECK(hplane_state_write(&p2, 3, 5, &v, 8) == HEDDLE_OK, "H13 lane3 write");
    CHECK(hplane_lane_end(&p2, 3) == HEDDLE_OK, "H13 lane3 end");

    uint64_t out = 0;
    CHECK(hplane_cell_read(&c, 0, 2, &out, 8, 64) == HEDDLE_OK && out == 42,
          "H13 lane0 read after lane session");
    CHECK(hplane_cell_read(&c, 3, 5, &out, 8, 64) == HEDDLE_OK && out == 42,
          "H13 lane3 read after lane session");

    uint64_t mask = 0;
    hplane_dirty_harvest(&c, &mask);
    CHECK(mask == ((1u << 0) | (1u << 3)), "H13 multi mask exact");
    CHECK(hplane_epoch_get(&c) == 2, "H13 epoch counts lane sessions");
    free(mem);

    /* Single-producer plane refuses lane sessions. */
    hplane_cfg_t single = { WHP2_MODE_STATE, 2, 8, 8, WHP2_STAT_U64, 0 };
    sz = (size_t)hplane_plane_size(&single);
    mem = alloc64(sz);
    hplane_plane_create(mem, sz, &single, FIXED_STAMP);
    hplane_ctx_t ps;
    hplane_attach(mem, sz, &ps, WHP2_ROLE_PRODUCER);
    CHECK(hplane_lane_begin(&ps, 0) == HEDDLE_E_MODE,
          "H13 lane session on single plane -> E_MODE");
    free(mem);
}

// --- H15 frame protocol ----------------------------------------------------

static void h15_frames(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_STATE, 2, 8, 8, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = alloc64(sz);
    hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);
    hplane_ctx_t p, c;
    hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER);
    hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER);

    uint64_t fid = 0;
    CHECK(hplane_frame_commit(&c, &fid) == HEDDLE_OK && fid == 1, "H15 frame 1");
    CHECK(hplane_frame_commit(&c, &fid) == HEDDLE_OK && fid == 2, "H15 frame 2");
    CHECK(hplane_frame_commit(&p, &fid) == HEDDLE_E_STATE,
          "H15 producer cannot commit frames -> E_STATE");

    CHECK(hplane_heartbeat_touch(&p) == HEDDLE_OK, "H15 heartbeat touch");
    CHECK(hplane_heartbeat_get(&c) > 0, "H15 heartbeat visible");

    /* epoch idle-killer: no commits -> epoch unchanged. */
    uint64_t e1 = hplane_epoch_get(&c);
    hplane_frame_commit(&c, &fid);
    CHECK(hplane_epoch_get(&c) == e1, "H15 idle frame leaves epoch alone");
    free(mem);
}

// --- H16 zero-heap ---------------------------------------------------------

static void h16_zero_heap(void)
{
#if defined(HPLANE_HAVE_MALLINFO2) && !defined(__SANITIZE_ADDRESS__) && \
    !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_UNDEFINED__)
    hplane_cfg_t cfg = { WHP2_MODE_STATE, 4, 16, 64, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = alloc64(sz);
    hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);
    hplane_ctx_t p, c;
    hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER);
    hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER);

    struct mallinfo2 before = mallinfo2();
    void *brk_before = sbrk(0);

    uint8_t sample[16];
    memset(sample, 0x5A, sizeof(sample));
    uint8_t out[16];
    for (int i = 0; i < 100000; i++) {
        hplane_commit_begin(&p);
        hplane_state_write(&p, (uint32_t)(i & 3), (uint32_t)(i & 63), sample, 16);
        hplane_commit_end(&p);
        hplane_cell_read(&c, (uint32_t)(i & 3), (uint32_t)(i & 63), out, 16, 64);
    }

    struct mallinfo2 after = mallinfo2();
    void *brk_after = sbrk(0);
    CHECK(after.uordblks == before.uordblks && after.hblkhd == before.hblkhd,
          "H16 100k cycles: heap use delta 0 (mallinfo2)");
    CHECK(brk_after == brk_before, "H16 100k cycles: sbrk delta 0");
    free(mem);
#else
    printf("SKIP H16 zero-heap probe (sanitizer or non-glibc build)\n");
#endif
}

// --- H17 100k continuous ---------------------------------------------------

static void h17_continuous(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_STATE, 8, 24, 32, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = alloc64(sz);
    hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);
    hplane_ctx_t p, c;
    hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER);
    hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER);

    uint8_t sample[24];
    uint8_t out[24];
    int bad = 0;
    uint64_t harvested_bits = 0;
    for (uint32_t i = 0; i < 200000u; i++) {
        /* deterministic payload: session counter + index + canary */
        uint64_t canary = 0xC0FFEE00ull + i;
        memcpy(sample, &canary, 8);
        memcpy(sample + 8, &i, 4);
        sample[12] = 0xA5;
        sample[13] = (uint8_t)(i >> 8);

        uint32_t lane = i & 7u;
        uint32_t cell = (i * 7u) & 31u;
        hplane_commit_begin(&p);
        hplane_state_write(&p, lane, cell, sample, 24);
        hplane_commit_end(&p);

        if (hplane_cell_read(&c, lane, cell, out, 24, 64) != HEDDLE_OK ||
            memcmp(out, sample, 24) != 0) {
            bad++;
        }
        uint64_t mask = 0;
        hplane_dirty_harvest(&c, &mask);
        harvested_bits += (uint64_t)__builtin_popcountll(mask);
    }
    CHECK(bad == 0, "H17 200k continuous updates: zero mismatches");
    CHECK(harvested_bits == 200000,
          "H17 exact dirty-bit accounting (200000 bits in, 200000 out)");

    uint64_t epoch = hplane_epoch_get(&c);
    CHECK(epoch == 200000, "H17 epoch == sessions");
    free(mem);
}

// --- H18 fork cross-process ------------------------------------------------

static void h18_fork_ipc(void)
{
    hplane_cfg_t cfg = { WHP2_MODE_STATE, 2, 16, 64, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);

    uint8_t *mem = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(mem != MAP_FAILED, "H18 mmap shared ok");
    hplane_plane_create(mem, sz, &cfg, FIXED_STAMP);

    fflush(NULL);   /* stdio hygiene before fork (Law: no dup output) */

    pid_t pid = fork();
    if (pid == 0) {
        /* child: consumer, LOCK-STEP with the producer — waits for
         * epoch >= i+1, reads the exact cell, verifies the canary,
         * then publishes frame i+1 so the parent may proceed. */
        hplane_ctx_t c;
        if (hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER) != HEDDLE_OK) {
            _exit(10);
        }
        uint8_t out[16];
        int bad = 0;
        for (uint32_t i = 0; i < 100000u; i++) {
            uint64_t epoch = hplane_epoch_get(&c);
            while (epoch < (uint64_t)i + 1) {
                epoch = hplane_epoch_get(&c);
            }
            uint32_t lane = i & 1u;
            uint32_t cell = (i * 13u) & 63u;
            if (hplane_cell_read(&c, lane, cell, out, 16, 64) != HEDDLE_OK) {
                bad += 1000000;   /* torn reads at rest: never acceptable */
                continue;
            }
            uint64_t canary;
            memcpy(&canary, out, 8);
            if (canary != 0xABCD0000ull + (uint64_t)(i + 1)) {
                bad++;
            }
            uint64_t fid = 0;
            hplane_frame_commit(&c, &fid);
        }
        _exit(bad > 0 ? 11 : 0);
    }

    /* parent: producer, one session per sample, then waits for the
     * child's frame watermark — the plane's own two counters are the
     * ENTIRE cross-process protocol (no pipes, no sockets, no broker). */
    hplane_ctx_t p;
    CHECK(hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER) == HEDDLE_OK,
          "H18 producer attach");
    uint8_t sample[16];
    for (uint32_t i = 0; i < 100000u; i++) {
        uint64_t canary = 0xABCD0000ull + (uint64_t)(i + 1);
        memcpy(sample, &canary, 8);
        memset(sample + 8, 0x11, 8);
        uint32_t lane = i & 1u;
        uint32_t cell = (i * 13u) & 63u;
        hplane_commit_begin(&p);
        hplane_state_write(&p, lane, cell, sample, 16);
        hplane_commit_end(&p);
        while (hplane_frame_get(&p) < (uint64_t)i + 1) {
            /* spin for consumer frame watermark */
        }
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "H18 fork IPC lock-step: 100k msgs, zero tears, zero canary misses");
    if (!WIFEXITED(st)) {
        fprintf(stderr, "H18 child raw status: 0x%x (signal %d)\n", st,
                WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    }
    munmap(mem, sz);
}

// ---------------------------------------------------------------------------

int main(void)
{
    printf("=== heddle-2.0 hot-plane unit conformance (H-series) ===\n");
    h1_crc_and_le();
    h2_create_attach();
    h4_ladder();
    h5_state_plane();
    h8_dirty_bbox();
    h10_stats();
    h11_ring();
    h12_overrun();
    h13_multi();
    h15_frames();
    h16_zero_heap();
    h17_continuous();
    h18_fork_ipc();

    printf("\nH-SERIES: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
