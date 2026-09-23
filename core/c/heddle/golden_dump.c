// golden_dump.c — WHP2 golden plane generator (Pillar 4).
//
// Emits a FULLY deterministic hot-plane image plus a JSON descriptor:
//   core/c/heddle/golden/golden.bin   — the raw plane bytes
//   core/c/heddle/golden/golden.json  — identity, geometry, expected
//                                       volatile registers, per-lane
//                                       expected stats and sample
//                                       locations/values
//
// The JS package (packages/heddle-hotplane) attaches to a copy of
// golden.bin inside a SharedArrayBuffer and must decode EVERY field
// and read EVERY sample byte-exactly — the C <-> SAB/Atomics interop
// contract, proven. CI regenerates and diffs (byte-identical across
// runs and machines: fixed create stamp, splitmix-derived samples,
// deterministic zeroing at create).
//
// Plane recipe (FIXED forever — changing it invalidates the golden):
//   mode=STATE, lanes=4, sample=16B, cells=32, stat=u64, single-producer
//   create_stamp_ns=1546300800000000000 (2019-01-01T00:00:00Z)
//   4 sessions: session s in {1..4} writes cells of all 4 lanes with
//   samples splitmix64(s*P1 + lane*P2 + cell*P3), leaving the mask,
//   stats, bboxes and slot ordinals in a rich, verifiable state.

#include "heddle_hotplane_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

#define GOLDEN_STAMP 1546300800000000000ull
#define GOLDEN_LANES 4
#define GOLDEN_CELLS 32
#define GOLDEN_SAMPLE 16

int main(int argc, char **argv)
{
    const char *outdir = (argc > 1) ? argv[1] : "core/c/heddle/golden";

    hplane_cfg_t cfg = { WHP2_MODE_STATE, GOLDEN_LANES, GOLDEN_SAMPLE,
                         GOLDEN_CELLS, WHP2_STAT_U64, 0 };
    size_t sz = (size_t)hplane_plane_size(&cfg);
    uint8_t *mem = NULL;
    posix_memalign((void **)&mem, 64, sz);
    if (hplane_plane_create(mem, sz, &cfg, GOLDEN_STAMP) != HEDDLE_OK) {
        fprintf(stderr, "golden: create failed\n");
        return 1;
    }
    hplane_ctx_t p;
    if (hplane_attach(mem, sz, &p, WHP2_ROLE_PRODUCER) != HEDDLE_OK) {
        fprintf(stderr, "golden: attach failed\n");
        return 1;
    }

    /* 4 sessions; session s writes cells (s*7 + lane*3 + k) % 32,
     * k in 0..(s+2), of every lane — deterministic, varied fan-out. */
    uint8_t s[GOLDEN_SAMPLE];
    for (uint32_t sess = 1; sess <= 4; sess++) {
        hplane_commit_begin(&p);
        for (uint32_t lane = 0; lane < GOLDEN_LANES; lane++) {
            uint32_t n = sess + 2;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t cell = (sess * 7u + lane * 3u + k) % GOLDEN_CELLS;
                uint64_t canary =
                    mix64(((uint64_t)sess * 0x9E3779B97F4A7C15ull) ^
                          ((uint64_t)lane * 0xBF58476D1CE4E5B9ull) ^
                          ((uint64_t)cell * 0x94D049BB133111EBull) ^
                          0xC0FFEEull);
                memcpy(s, &canary, 8);
                memcpy(s + 8, &sess, 4);
                memcpy(s + 12, &cell, 4);
                if (hplane_state_write(&p, lane, cell, s, GOLDEN_SAMPLE) !=
                    HEDDLE_OK) {
                    fprintf(stderr, "golden: write failed\n");
                    return 1;
                }
            }
        }
        hplane_commit_end(&p);
    }

    /* ---- emit golden.bin -------------------------------------------- */
    char path[512];
    snprintf(path, sizeof(path), "%s/golden.bin", outdir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "golden: cannot open %s\n", path);
        return 1;
    }
    fwrite(mem, 1, sz, f);
    fclose(f);

    /* ---- emit golden.json ------------------------------------------- */
    hplane_ctx_t c;
    hplane_attach(mem, sz, &c, WHP2_ROLE_CONSUMER);
    snprintf(path, sizeof(path), "%s/golden.json", outdir);
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "golden: cannot open %s\n", path);
        return 1;
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"magic\": %u, \"ver_major\": %u, \"ver_minor\": %u,\n",
            WHP2_MAGIC, WHP2_LAYOUT_MAJOR, WHP2_LAYOUT_MINOR);
    fprintf(f, "  \"header_size\": %u, \"layout_rev\": %u,\n",
            WHP2_HEADER_SIZE, WHP2_LAYOUT_REV);
    fprintf(f, "  \"mode\": %u, \"lane_count\": %u, \"lane_stride\": %u,\n",
            cfg.mode, cfg.lane_count, 32 * 64);
    fprintf(f, "  \"sample_size\": %u, \"slot_capacity\": %u,\n",
            cfg.sample_size, cfg.slot_capacity);
    fprintf(f, "  \"stat_kind\": %u, \"flags\": %u,\n", cfg.stat_kind,
            cfg.flags);
    fprintf(f, "  \"plane_size\": \"%zu\", \"create_stamp_ns\": \"%llu\",\n", sz,
            (unsigned long long)GOLDEN_STAMP);
    fprintf(f, "  \"endian_canary\": %u,\n", WHP2_ENDIAN_CANARY);
    fprintf(f, "  \"epoch\": \"%llu\",\n",
            (unsigned long long)hplane_epoch_get(&c));
    fprintf(f, "  \"commit_seq\": \"%llu\", \"begin_seq\": \"%llu\",\n",
            (unsigned long long)hplane_ld_acq64(&hplane_hdr(&c)->commit_seq),
            (unsigned long long)hplane_ld_acq64(&hplane_hdr(&c)->begin_seq));
    fprintf(f, "  \"dirty_mask\": \"%llu\", "
                   "\"dirty_transitions\": \"%llu\",\n",
            (unsigned long long)hplane_ld_rel64(&hplane_hdr(&c)->dirty_mask),
            (unsigned long long)hplane_ld_rel64(
                &hplane_hdr(&c)->dirty_transitions));
    fprintf(f, "  \"render_frame_id\": \"%llu\",\n",
            (unsigned long long)hplane_ld_rel64(
                &hplane_hdr(&c)->render_frame_id));
    fprintf(f, "  \"static_crc32\": %u,\n",
            hplane_crc32(mem + WHP2_STATIC_CRC_COVER_OFF,
                         WHP2_STATIC_CRC_COVER_LEN));
    fprintf(f, "  \"lanes\": [\n");
    for (uint32_t lane = 0; lane < GOLDEN_LANES; lane++) {
        hplane_lane_stats_t st;
        hplane_lane_stats_get(&c, lane, &st, 64);
        uint64_t bbox = 0;
        hplane_bbox_harvest(&c, lane, &bbox);
        uint32_t mn = (uint32_t)(bbox >> 32), mx = (uint32_t)bbox;
        fprintf(f, "    {\"lane\": %u, \"commit_count\": \"%llu\", "
                   "\"min_raw\": \"%llu\", \"max_raw\": \"%llu\", "
                   "\"current_raw\": \"%llu\", \"bbox_min\": %u, "
                   "\"bbox_max\": %u, \"cells\": [",
                lane, (unsigned long long)st.commit_count,
                (unsigned long long)st.min_raw,
                (unsigned long long)st.max_raw,
                (unsigned long long)st.current_raw, mn, mx);
        for (uint32_t cell = 0; cell < GOLDEN_CELLS; cell++) {
            uint8_t out[GOLDEN_SAMPLE];
            if (hplane_cell_read(&c, lane, cell, out, GOLDEN_SAMPLE, 64) !=
                HEDDLE_OK) {
                fprintf(stderr, "golden: read failed\n");
                return 1;
            }
            uint64_t canary;
            memcpy(&canary, out, 8);
            /* cells written by session s=(sess): decode from payload */
            uint32_t sess;
            memcpy(&sess, out + 8, 4);
            uint32_t cell_tag;
            memcpy(&cell_tag, out + 12, 4);
            fprintf(f, "%s{\"cell\": %u, \"session\": %u, \"cell_tag\": %u, "
                       "\"canary\": \"%llu\", \"hex\": \"%02x%02x%02x%02x%02x%02x"
                       "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\"}",
                    cell ? ", " : "", cell, sess, cell_tag,
                    (unsigned long long)canary, out[0], out[1], out[2],
                    out[3], out[4], out[5], out[6], out[7], out[8], out[9],
                    out[10], out[11], out[12], out[13], out[14], out[15]);
        }
        fprintf(f, "]}%s\n", lane + 1 < GOLDEN_LANES ? "," : "");
    }
    fprintf(f, "  ],\n");
    fprintf(f, "  \"offsets\": {\n");
    fprintf(f, "    \"epoch\": %u, \"begin_seq\": %u, \"commit_seq\": %u,\n",
            0x40, 0x48, 0x50);
    fprintf(f, "    \"dirty_mask\": %u, \"render_frame_id\": %u,\n", 0x58,
            0x60);
    fprintf(f, "    \"heartbeat_ns\": %u, \"endian_canary\": %u,\n", 0x68,
            0x70);
    fprintf(f, "    \"dirty_transitions\": %u,\n", 0x78);
    fprintf(f, "    \"lane_desc_base\": %u, \"lane_desc_size\": %u,\n",
            WHP2_HEADER_SIZE, WHP2_LANE_DESC_SIZE);
    fprintf(f, "    \"lane_data_base\": %u, \"cell_stride\": %u,\n",
            WHP2_HEADER_SIZE + GOLDEN_LANES * WHP2_LANE_DESC_SIZE, 64);
    fprintf(f, "    \"cell_payload_off\": 0\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
    fclose(f);

    printf("golden: %zu bytes + descriptor written to %s\n", sz, outdir);
    return 0;
}
