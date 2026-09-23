// heddle_bridge_test.c — WHP2 bridge conformance (Pillar 4, W-series).
//
//   W1  every bridge offset equals the engine's own offsetof math
//   W2  describe() decodes a real plane (identity, geometry, ladder)
//   W3  describe() refuses tampered planes with the right codes
//   W4  ZERO-COPY ALIAS PROOF: sessions written ENTIRELY through the
//       bridge (raw atomics on offsets) are read consistently by the
//       engine's cell_read; dirty mask, transitions and lane stats all
//       agree — the bridge is the engine, seen from linear memory.
//   W5  bridge payload copies are byte-exact with the engine's

#include "heddle_hotplane_internal.h"
#include "heddle_bridge.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void)
{
    printf("=== heddle-2.0 WASM/FFI bridge conformance (W-series) ===\n");

    /* ---- W1: offset parity ------------------------------------------ */
    CHECK(hedbridge_off_epoch() == offsetof(hplane_header_t, epoch) &&
          hedbridge_off_begin_seq() == offsetof(hplane_header_t, begin_seq) &&
          hedbridge_off_commit_seq() == offsetof(hplane_header_t, commit_seq) &&
          hedbridge_off_dirty_mask() == offsetof(hplane_header_t, dirty_mask) &&
          hedbridge_off_render_frame() ==
              offsetof(hplane_header_t, render_frame_id) &&
          hedbridge_off_heartbeat() == offsetof(hplane_header_t, heartbeat_ns) &&
          hedbridge_off_dirty_transitions() ==
              offsetof(hplane_header_t, dirty_transitions),
          "W1 header field offsets == engine offsetof");

    hplane_cfg_t cfg = { WHP2_MODE_STATE, 4, 16, 32, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    CHECK(hedbridge_off_lane_desc(0) == WHP2_HEADER_SIZE &&
          hedbridge_off_lane_desc(3) ==
              WHP2_HEADER_SIZE + 3 * WHP2_LANE_DESC_SIZE,
          "W1 lane descriptor offsets");
    CHECK(hedbridge_off_lane_data(2, 4, 32 * 64) ==
              (uint64_t)WHP2_HEADER_SIZE + 4 * WHP2_LANE_DESC_SIZE +
              2 * 32 * 64,
          "W1 lane data offset arithmetic");
    CHECK(hedbridge_slot_stride(16) == hplane_slot_stride(16) &&
          hedbridge_cell_stride(16) == hplane_cell_stride(16) &&
          hedbridge_off_slot_payload() == WHP2_SLOT_HEADER_SIZE,
          "W1 stride + payload-offset parity");

    /* ---- W2/W4: real plane, bridge-written sessions, engine reads --- */
    uint8_t *mem = NULL;
    posix_memalign((void **)&mem, 64, sz);
    CHECK(hplane_plane_create(mem, sz, &cfg, 777777777ull) == HEDDLE_OK,
          "W2 plane created");

    heddle_bridge_desc_t d;
    CHECK(hedbridge_describe(mem, sz, &d) == HEDDLE_OK &&
          d.magic == WHP2_MAGIC && d.ver_major == 2 && d.ver_minor == 0 &&
          d.mode == WHP2_MODE_STATE && d.lane_count == 4 &&
          d.sample_size == 16 && d.slot_capacity == 32 &&
          d.plane_size == (uint64_t)sz && d.endian_ok && d.crc_ok &&
          d.create_stamp_ns == 777777777ull,
          "W2 describe decodes identity + geometry");

    hplane_ctx_t c;
    CHECK(hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER) == HEDDLE_OK,
          "W2 engine consumer attach");

    /* Bridge-driven producer sessions. */
    uint64_t session = 0;
    uint8_t sample[16];
    int bad = 0;
    for (uint32_t i = 0; i < 100000u; i++) {
        uint32_t lane = i & 3u;
        uint32_t cell = (i * 5u) & 31u;
        uint64_t canary = 0xB0D00000ull + i;
        memcpy(sample, &canary, 8);
        memcpy(sample + 8, &lane, 4);
        memcpy(sample + 12, &cell, 4);
        if (hedbridge_session_write(mem, sz, lane, cell, sample, 16,
                                    &session) != HEDDLE_OK) {
            bad += 1000;
            continue;
        }
        /* the ENGINE reads what the BRIDGE wrote */
        uint8_t out[16];
        if (hplane_cell_read(&c, lane, cell, out, 16, 64) != HEDDLE_OK) {
            bad++;
            continue;
        }
        if (memcmp(out, sample, 16) != 0) {
            bad++;
        }
    }
    CHECK(bad == 0, "W4 bridge-writes <-> engine-reads, 100k sessions");

    uint64_t mask = 0;
    CHECK(hplane_dirty_harvest(&c, &mask) == HEDDLE_OK && mask == 0xF,
          "W4 bridge sessions set all four lane bits");
    CHECK(hplane_dirty_transitions_get(&c) >= 4,
          "W4 transitions counted by the bridge agree with the engine");
    hplane_lane_stats_t st;
    CHECK(hplane_lane_stats_get(&c, 1, &st, 64) == HEDDLE_OK &&
          st.commit_count == 25000,
          "W4 lane stats visible across the alias");
    CHECK(hplane_epoch_get(&c) == session,
          "W4 engine epoch (derived from commit_seq) == bridge session");
    CHECK(hedbridge_a_load_rel64(mem, hedbridge_off_epoch()) == 0,
          "W4 single-plane epoch register untouched (RMW-free commits)");
    CHECK(hedbridge_a_load_acq64(mem, hedbridge_off_commit_seq()) ==
              session,
          "W4 bridge reads the engine's commit_seq directly");

    /* ---- W5: byte-exact payload copies ------------------------------ */
    {
        uint64_t cell_off = hedbridge_off_slot(0, 4, 32 * 64,
                                               WHP2_MODE_STATE, 16, 7);
        uint8_t via_bridge[16];
        uint8_t via_engine[16];
        /* state cells: payload at the cell base (no slot header) */
        hedbridge_copy_from(via_bridge, mem, cell_off, 16);
        hplane_cell_read(&c, 0, 7, via_engine, 16, 64);
        CHECK(memcmp(via_bridge, via_engine, 16) == 0,
              "W5 bridge copy == engine copy (byte-exact)");
    }

    /* ---- W3: describe refusal ladder -------------------------------- */
    {
        uint8_t *scratch = NULL;
        posix_memalign((void **)&scratch, 64, sz);
        memcpy(scratch, mem, sz);
        scratch[0] ^= 0xFF;
        CHECK(hedbridge_describe(scratch, sz, &d) == HEDDLE_E_MAGIC,
              "W3 bad magic -> E_MAGIC");
        memcpy(scratch, mem, sz);
        scratch[4] = 9;
        CHECK(hedbridge_describe(scratch, sz, &d) == HEDDLE_E_VERSION,
              "W3 bad version -> E_VERSION");
        memcpy(scratch, mem, sz);
        hplane_le32_put(scratch + 0x70, 0xDEAD0000u);
        CHECK(hedbridge_describe(scratch, sz, &d) == HEDDLE_E_ENDIAN,
              "W3 flipped canary -> E_ENDIAN");
        memcpy(scratch, mem, sz);
        scratch[0x20] ^= 1;
        CHECK(hedbridge_describe(scratch, sz, &d) == HEDDLE_E_CFG_CRC,
              "W3 corrupt config -> E_CFG_CRC");
        CHECK(hedbridge_describe(mem, sz - 1, &d) == HEDDLE_E_REGION_SIZE,
              "W3 short region -> E_REGION_SIZE");
        CHECK(hedbridge_describe(NULL, sz, &d) == HEDDLE_E_ARG,
              "W3 null base -> E_ARG");
        free(scratch);
    }

    printf("\nW-SERIES: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
