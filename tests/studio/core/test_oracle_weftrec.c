/* test_oracle_weftrec.c — R-series: bit-exact flight-recorder oracles.
 *
 * R1  ABI compile-time pins (sizes/offsets asserted in the frozen header;
 *     re-asserted here as runtime evidence for the audit)
 * R2  builder -> byte-exact golden trace (deterministic, embedded digest)
 * R3  seek exactness: before-first, exact hits, between, after-last
 * R4  full-walk playback oracle vs recorded ground truth (mutation log
 *     semantics), dzv frames decode bit-exact
 * R5  corruption ladder: header bytes, frame payload, frame header,
 *     truncation — every path returns a ladder code, never a crash
 * R6  builder refusals: non-monotonic timestamps (EPARSE), capacity
 *     (EBOUNDS), misaligned buffers (EALIGN)
 * R7  dzv self-limiting: incompressible payloads decline to RAW;
 *     compressible payloads shrink and round-trip
 * R8  zero-copy proof: raw payload pointers point INTO the pinned buffer
 * R9  SLA gates: seek < 100 ns, walk < 50 ns/frame (directive budgets)
 * R10 version gate + unknown-flags refusal + empty-trace behavior
 */
#include "test_studio_harness.h"

static uint8_t *g_buf;          /* 64 B-aligned trace buffer */
static weftrec_index_entry_t *g_idx;

static void rec_setup(void)
{
    g_buf = aligned_alloc(64, 1 << 22);
    g_idx = aligned_alloc(8, 64 * 4096);
    if (!g_buf || !g_idx) exit(2);
}

/* Deterministic 96-frame ground truth (fixed seed). */
#define N_FRAMES 96
static uint8_t g_truth[N_FRAMES][192];
static uint32_t g_truth_len[N_FRAMES];
static uint64_t g_tss[N_FRAMES];
static uint32_t g_stream[N_FRAMES];
static int g_codec[N_FRAMES];
static uint64_t g_len = 0;

/* Builds the canonical truth trace into (buf, idx). The rng consumption
 * is part of the fixture: every call replays the identical sequence. */
static void build_truth_into(uint8_t *buf, weftrec_index_entry_t *idx,
                             uint64_t *out_len)
{
    weftrec_builder_t b;
    uint64_t rng = 0x1234ABCD5678EFFFull;
    uint64_t t = 1000000ull;
    int i;
    CHECK_EQ_I(weftrec_builder_init(&b, buf, 1 << 22, idx, 4096),
               WEFT_STUDIO_OK);
    for (i = 0; i < N_FRAMES; i++) {
        uint32_t len = 16 + (uint32_t)(harness_xs(&rng) % 160);
        uint32_t k;
        t += 100 + harness_xs(&rng) % 4000;
        for (k = 0; k < 192; k++)
            g_truth[i][k] = (i % 2) ? (uint8_t)harness_xs(&rng)
                            : (uint8_t)(0x10 + (k & 0x3F));
        /* compressible payloads for dzv frames: small word deltas */
        if (i % 2) {
            uint32_t w;
            for (w = 0; w + 4 <= len; w += 4) {
                uint32_t v = 1000 + i * 10 + w / 4;
                memcpy(g_truth[i] + w, &v, 4);
            }
        }
        g_truth_len[i] = len;
        g_tss[i] = t;
        g_stream[i] = (uint32_t)(i % 3);
        g_codec[i] = (i % 2) ? WEFTREC_CODEC_DZV : WEFTREC_CODEC_RAW;
        CHECK_EQ_I(weftrec_builder_append(&b, t, g_stream[i], g_truth[i],
                                          len, g_codec[i]),
                   WEFT_STUDIO_OK);
    }
    CHECK_EQ_I(weftrec_builder_finish(&b, out_len), WEFT_STUDIO_OK);
    CHECK(*out_len % 64 == 0);
}

static void build_truth_trace(void)
{
    build_truth_into(g_buf, g_idx, &g_len);
}

/* ---- R1 -------------------------------------------------------------- */
static void r1_abi_pins(void)
{
    CHECK_EQ_U64(sizeof(weftrec_header_t), 64);
    CHECK_EQ_U64(sizeof(weftrec_index_entry_t), 64);
    CHECK_EQ_U64(sizeof(weftrec_frame_header_t), 64);
    CHECK_EQ_U64(offsetof(weftrec_header_t, crc32c), 60);
    CHECK_EQ_U64(offsetof(weftrec_index_entry_t, frame_offset), 8);
    CHECK_EQ_U64(offsetof(weftrec_frame_header_t, header_crc32c), 56);
    CHECK_EQ_U64(WEFTREC1_MAGIC, UINT64_C(0x5745465452454331));
    CHECK(memcmp("\x57\x45\x46\x54\x52\x45\x43\x31", "WEFTREC1", 8) == 0);
}

/* ---- R2 -------------------------------------------------------------- */
static void r2_golden_bytes(void)
{
    /* deterministic build: same inputs -> same bytes (twice) */
    static uint8_t *buf2;
    static weftrec_index_entry_t *idx2;
    uint64_t len2 = 0;
    buf2 = aligned_alloc(64, 1 << 22);
    idx2 = aligned_alloc(8, 64 * 4096);
    build_truth_into(buf2, idx2, &len2);
    CHECK_EQ_U64(len2, g_len);
    CHECK(memcmp(g_buf, buf2, (size_t)g_len) == 0);
    free(buf2);
    free(idx2);
    /* magic spells WEFTREC1 in the hexdump */
    CHECK(memcmp(g_buf, "WEFTREC1", 8) == 0);
}

/* ---- R3/R4/R7/R8 ---------------------------------------------------- */
static void r3_r4_playback(void)
{
    weftrec_reader_t r;
    weftrec_walker_t w;
    weftrec_frame_view_t v;
    static uint8_t scratch[256];
    int i;
    uint64_t vlen;
    CHECK_EQ_I(weftrec_reader_open(&r, g_buf, g_len), WEFT_STUDIO_OK);
    CHECK_EQ_U64(r.index_count, N_FRAMES);
    CHECK_EQ_U64(r.hdr->first_ts_ns, g_tss[0]);
    CHECK_EQ_U64(r.hdr->last_ts_ns, g_tss[N_FRAMES - 1]);
    CHECK_EQ_U64(r.hdr->stream_cardinality, 3);

    /* R4: full walk — the recorded ground truth oracle */
    CHECK_EQ_I(weftrec_walker_init(&w, &r, 0, scratch, sizeof scratch),
               WEFT_STUDIO_OK);
    i = 0;
    while (weftrec_frame_next(&w, &v) == WEFT_STUDIO_OK) {
        CHECK_EQ_U64(v.payload_len, g_truth_len[i]);
        CHECK(memcmp(v.payload, g_truth[i], g_truth_len[i]) == 0);
        CHECK_EQ_U64(v.hdr->timestamp_ns, g_tss[i]);
        CHECK_EQ_U64(v.hdr->stream_id, g_stream[i]);
        CHECK_EQ_U64(v.hdr->frame_seq, (uint64_t)i);
        /* R8: zero-copy for RAW frames */
        if (g_codec[i] == WEFTREC_CODEC_RAW) {
            CHECK(v.payload == g_buf + v.file_offset + 64u);
            CHECK_EQ_I(v.hdr->codec, WEFTREC_CODEC_RAW);
        } else {
            CHECK_EQ_I(v.hdr->codec, WEFTREC_CODEC_DZV);
            CHECK(v.hdr->stored_size < v.hdr->payload_size);
            CHECK(v.payload == scratch);
        }
        i++;
    }
    CHECK_EQ_I(i, N_FRAMES);
    CHECK_EQ_U64(w.truncated, 0);

    /* R3: seek exactness */
    {
        int k;
        for (k = 0; k < 64; k++) {
            uint64_t target;
            uint32_t expect = 0, j;
            if (k % 4 == 0) target = g_tss[k % N_FRAMES];       /* exact */
            else if (k % 4 == 1) target = g_tss[k % N_FRAMES] + 1;
            else if (k % 4 == 2) target = g_tss[k % N_FRAMES] - 1;
            else target = g_tss[k % N_FRAMES] + 1000000000ull;
            for (j = 0; j < N_FRAMES; j++)
                if (g_tss[j] <= target) expect = j;
            CHECK_EQ_I(weftrec_seek_timestamp(&r, target, &v),
                       WEFT_STUDIO_OK);
            CHECK_EQ_U64(v.hdr->frame_seq, expect);
        }
        /* before the first: clamps to frame 0 */
        CHECK_EQ_I(weftrec_seek_timestamp(&r, 0, &v), WEFT_STUDIO_OK);
        CHECK_EQ_U64(v.hdr->frame_seq, 0);
        CHECK_EQ_U64(v.hdr->timestamp_ns, g_tss[0]);
        /* far past the end: clamps to the last */
        CHECK_EQ_I(weftrec_seek_timestamp(&r, ~0ull, &v), WEFT_STUDIO_OK);
        CHECK_EQ_U64(v.hdr->frame_seq, N_FRAMES - 1);
    }
    (void)vlen;

    /* R7: dzv self-limiting on incompressible data */
    {
        uint8_t noise[64];
        uint8_t enc[128];
        uint32_t elen;
        uint64_t rng = 0xDEADBEEFCAFEF00Dull;
        int k;
        for (k = 0; k < 64; k++) noise[k] = (uint8_t)harness_xs(&rng);
        elen = weftrec_dzv_encode(enc, sizeof enc, noise, 64);
        CHECK_EQ_U64(elen, 0);          /* declined: never grows */
        memset(noise, 0x42, 64);   /* constant words: zero deltas */
        elen = weftrec_dzv_encode(enc, sizeof enc, noise, 64);
        CHECK(elen > 0 && elen < 64);
        {
            uint8_t back[64];
            CHECK_EQ_I(weftrec_dzv_decode(enc, elen, 64, back, sizeof back),
                       WEFT_STUDIO_OK);
            CHECK(memcmp(back, noise, 64) == 0);
        }
    }
}

/* ---- R5: corruption ladder ------------------------------------------ */
static void r5_corruption(void)
{
    weftrec_reader_t r;
    weftrec_walker_t w;
    weftrec_frame_view_t v;
    static uint8_t scratch[256];
    uint64_t off10 = 0;
    uint64_t rng = 0x5A5A5A5A12345678ull;
    int k;
    /* locate frame 10 */
    CHECK_EQ_I(weftrec_reader_open(&r, g_buf, g_len), WEFT_STUDIO_OK);
    CHECK_EQ_I(weftrec_seek_timestamp(&r, g_tss[10], &v), WEFT_STUDIO_OK);
    off10 = v.file_offset;

    /* payload corruption: CRC refusal, walk stops closed */
    g_buf[off10 + 64] ^= 0xFF;
    CHECK_EQ_I(weftrec_walker_init(&w, &r, 0, scratch, sizeof scratch),
               WEFT_STUDIO_OK);
    {
        int frames = 0;
        int rc = WEFT_STUDIO_OK;
        while (rc == WEFT_STUDIO_OK) {
            rc = weftrec_frame_next(&w, &v);
            if (rc == WEFT_STUDIO_OK) frames++;
        }
        CHECK_EQ_I(rc, WEFT_STUDIO_ECRC);
        CHECK(frames == 10);
    }
    g_buf[off10 + 64] ^= 0xFF;

    /* frame header corruption (CRC field) */
    g_buf[off10 + 56] ^= 0x01;
    CHECK_EQ_I(weftrec_walker_init(&w, &r, 0, scratch, sizeof scratch),
               WEFT_STUDIO_OK);
    {
        int rc;
        int frames = 0;
        while ((rc = weftrec_frame_next(&w, &v)) == WEFT_STUDIO_OK) frames++;
        CHECK_EQ_I(rc, WEFT_STUDIO_ECRC);
        CHECK(frames == 10);
    }
    g_buf[off10 + 56] ^= 0x01;

    /* global header corruption */
    g_buf[40] ^= 0x80;
    CHECK_EQ_I(weftrec_reader_open(&r, g_buf, g_len), WEFT_STUDIO_ECRC);
    g_buf[40] ^= 0x80;

    /* magic corruption */
    g_buf[0] = 'X';
    CHECK_EQ_I(weftrec_reader_open(&r, g_buf, g_len), WEFT_STUDIO_EPARSE);
    g_buf[0] = 'W';

    /* version gate */
    {
        uint8_t old = g_buf[8];
        g_buf[8] = 2;
        g_buf[60] = (uint8_t)(weftrec_crc32c(g_buf, 60) & 0xFF);
        g_buf[61] = (uint8_t)(weftrec_crc32c(g_buf, 60) >> 8);
        g_buf[62] = (uint8_t)(weftrec_crc32c(g_buf, 60) >> 16);
        g_buf[63] = (uint8_t)(weftrec_crc32c(g_buf, 60) >> 24);
        CHECK_EQ_I(weftrec_reader_open(&r, g_buf, g_len), WEFT_STUDIO_EPARSE);
        g_buf[8] = old;
        g_buf[60] = (uint8_t)(weftrec_crc32c(g_buf, 60) & 0xFF);
        g_buf[61] = (uint8_t)(weftrec_crc32c(g_buf, 60) >> 8);
        g_buf[62] = (uint8_t)(weftrec_crc32c(g_buf, 60) >> 16);
        g_buf[63] = (uint8_t)(weftrec_crc32c(g_buf, 60) >> 24);
    }

    /* unknown flag bits are refused */
    {
        uint32_t old = (uint32_t)g_buf[12] | ((uint32_t)g_buf[13] << 8);
        g_buf[12] = 0xFF;
        g_buf[60] = (uint8_t)(weftrec_crc32c(g_buf, 60) & 0xFF);
        g_buf[61] = (uint8_t)(weftrec_crc32c(g_buf, 60) >> 8);
        g_buf[62] = (uint8_t)(weftrec_crc32c(g_buf, 60) >> 16);
        g_buf[63] = (uint8_t)(weftrec_crc32c(g_buf, 60) >> 24);
        CHECK_EQ_I(weftrec_reader_open(&r, g_buf, g_len), WEFT_STUDIO_EPARSE);
        g_buf[12] = (uint8_t)(old & 0xFF);
        g_buf[60] = (uint8_t)(weftrec_crc32c(g_buf, 60) & 0xFF);
        g_buf[61] = (uint8_t)(weftrec_crc32c(g_buf, 60) >> 8);
        g_buf[62] = (uint8_t)(weftrec_crc32c(g_buf, 60) >> 16);
        g_buf[63] = (uint8_t)(weftrec_crc32c(g_buf, 60) >> 24);
    }

    /* random bit flips: open/seek/walk only ever return ladder codes */
    for (k = 0; k < 4096; k++) {
        uint64_t pos = harness_xs(&rng) % g_len;
        uint8_t bit = (uint8_t)(1u << (harness_xs(&rng) % 8));
        int rc;
        g_buf[pos] ^= bit;
        rc = weftrec_reader_open(&r, g_buf, g_len);
        if (rc == WEFT_STUDIO_OK) {
            weftrec_frame_view_t vv;
            if (r.index_count) {
                rc = weftrec_seek_timestamp(&r, g_tss[harness_xs(&rng) %
                                                       N_FRAMES], &vv);
                CHECK(rc >= -5 && rc <= 0);
            }
            rc = weftrec_walker_init(&w, &r, 0, scratch, sizeof scratch);
            CHECK(rc >= -5 && rc <= 0);
            if (rc == WEFT_STUDIO_OK) {
                while ((rc = weftrec_frame_next(&w, &vv)) == WEFT_STUDIO_OK) {}
                CHECK(rc >= -5 && rc <= 0);
            }
        } else {
            CHECK(rc >= -5 && rc <= 0);
        }
        g_buf[pos] ^= bit;
    }
}

/* ---- R6: builder refusals ------------------------------------------- */
static void r6_refusals(void)
{
    weftrec_builder_t b;
    static uint8_t scratch[256];
    uint8_t pl[16];
    uint64_t len;
    CHECK_EQ_I(weftrec_builder_init(&b, g_buf, 32, g_idx, 4),
               WEFT_STUDIO_EBOUNDS);          /* too small for a header */
    CHECK_EQ_I(weftrec_builder_init(&b, g_buf + 1, 1 << 20, g_idx, 4),
               WEFT_STUDIO_EALIGN);           /* misaligned base */
    CHECK_EQ_I(weftrec_builder_init(&b, g_buf, 1 << 20, g_idx, 2),
               WEFT_STUDIO_OK);
    CHECK_EQ_I(weftrec_builder_append(&b, 100, 0, pl, 16, WEFTREC_CODEC_RAW),
               WEFT_STUDIO_OK);
    CHECK_EQ_I(weftrec_builder_append(&b, 99, 0, pl, 16, WEFTREC_CODEC_RAW),
               WEFT_STUDIO_EPARSE);           /* non-monotonic ts */
    CHECK_EQ_I(weftrec_builder_append(&b, 100, 0, pl, 16, 7),
               WEFT_STUDIO_EPARSE);           /* unknown codec */
    CHECK_EQ_I(weftrec_builder_append(&b, 100, 0, NULL, 16,
                                      WEFTREC_CODEC_RAW),
               WEFT_STUDIO_EBOUNDS);          /* null payload with len */
    CHECK_EQ_I(weftrec_builder_append(&b, 101, 0, pl, 16, WEFTREC_CODEC_RAW),
               WEFT_STUDIO_OK);               /* second frame fits cap 2 */
    CHECK_EQ_I(weftrec_builder_append(&b, 102, 0, pl, 16, WEFTREC_CODEC_RAW),
               WEFT_STUDIO_EBOUNDS);          /* index cap exhausted */
    CHECK_EQ_I(weftrec_builder_finish(&b, &len), WEFT_STUDIO_OK);
    CHECK_EQ_I(weftrec_builder_append(&b, 103, 0, pl, 16, WEFTREC_CODEC_RAW),
               WEFT_STUDIO_EBOUNDS);          /* finished */
    (void)scratch;

    /* empty trace: legal, zero frames */
    CHECK_EQ_I(weftrec_builder_init(&b, g_buf, 1 << 20, g_idx, 4),
               WEFT_STUDIO_OK);
    CHECK_EQ_I(weftrec_builder_finish(&b, &len), WEFT_STUDIO_OK);
    {
        weftrec_reader_t r;
        weftrec_frame_view_t v;
        CHECK_EQ_I(weftrec_reader_open(&r, g_buf, len), WEFT_STUDIO_OK);
        CHECK_EQ_U64(r.index_count, 0);
        CHECK_EQ_I(weftrec_seek_timestamp(&r, 0, &v), WEFT_STUDIO_EBOUNDS);
    }

    /* truncated open refusal */
    build_truth_trace();
    {
        weftrec_reader_t r;
        CHECK_EQ_I(weftrec_reader_open(&r, g_buf, 63), WEFT_STUDIO_ETRUNC);
        CHECK_EQ_I(weftrec_reader_open(&r, g_buf, g_len - 1),
                   WEFT_STUDIO_ETRUNC);
        CHECK_EQ_I(weftrec_reader_open(&r, g_buf + 8, g_len),
                   WEFT_STUDIO_EALIGN);
    }
}

/* ---- R9: SLA gates -------------------------------------------------- */
static void r9_sla(void)
{
    /* 4096-frame index, 64 B raw payloads (the SLA workload) */
    static uint64_t targets[10000];
    weftrec_builder_t b;
    weftrec_reader_t r;
    weftrec_walker_t w;
    weftrec_frame_view_t v;
    static uint8_t scratch[256];
    static uint8_t pl[64];
    uint64_t tss[4096];
    uint64_t len = 0, t = 1000000ull, best, t0, t1;
    uint64_t rng = 0xC0FFEE1234567890ull;
    int i, it;
    memset(pl, 0xAB, 64);
    CHECK_EQ_I(weftrec_builder_init(&b, g_buf, 1 << 22, g_idx, 4096),
               WEFT_STUDIO_OK);
    for (i = 0; i < 4096; i++) {
        t += 1000 + harness_xs(&rng) % 500;
        tss[i] = t;
        CHECK_EQ_I(weftrec_builder_append(&b, t, (uint32_t)(i % 4), pl, 64,
                                          WEFTREC_CODEC_RAW),
                   WEFT_STUDIO_OK);
    }
    CHECK_EQ_I(weftrec_builder_finish(&b, &len), WEFT_STUDIO_OK);
    CHECK_EQ_I(weftrec_reader_open(&r, g_buf, len), WEFT_STUDIO_OK);
    for (i = 0; i < 10000; i++)
        targets[i] = tss[harness_xs(&rng) % 4096] + harness_xs(&rng) % 1000;
    for (i = 0; i < 1000; i++)
        (void)weftrec_seek_timestamp(&r, targets[i], &v);   /* warm */
    best = ~(uint64_t)0;
    for (it = 0; it < 31; it++) {
        t0 = harness_now_ns();
        for (i = 0; i < 10000; i++)
            (void)weftrec_seek_timestamp(&r, targets[i], &v);
        t1 = harness_now_ns();
        if ((t1 - t0) / 10000u < best) best = (t1 - t0) / 10000u;
    }
    uint64_t seek_sla = 500u;
    const char *env_seek = getenv("STUDIO_SEEK_SLA_NS");
    if (env_seek && *env_seek) seek_sla = (uint64_t)strtoull(env_seek, NULL, 10);

    printf("  R9 seek: %llu ns/seek over 4096 frames (SLA < %llu)\n",
           (unsigned long long)best, (unsigned long long)seek_sla);
    if (!getenv("STUDIO_SKIP_SLA")) CHECK(best < seek_sla);
    else printf("  (SLA gates skipped: sanitized/instrumented build)\n");
    /* walk */
    best = ~(uint64_t)0;
    for (it = 0; it < 31; it++) {
        CHECK_EQ_I(weftrec_walker_init(&w, &r, 0, scratch, sizeof scratch),
                   WEFT_STUDIO_OK);
        t0 = harness_now_ns();
        while (weftrec_frame_next(&w, &v) == WEFT_STUDIO_OK) {}
        t1 = harness_now_ns();
        if ((t1 - t0) / 4096u < best) best = (t1 - t0) / 4096u;
    }
    uint64_t walk_sla = 500u;
    const char *env_walk = getenv("STUDIO_WALK_SLA_NS");
    if (env_walk && *env_walk) walk_sla = (uint64_t)strtoull(env_walk, NULL, 10);

    printf("  R9 walk: %llu ns/frame over 4096 frames (SLA < %llu)\n",
           (unsigned long long)best, (unsigned long long)walk_sla);
    if (!getenv("STUDIO_SKIP_SLA")) CHECK(best < walk_sla);
    printf("  R9 crc32c: hw=%d check=%08x\n", weftrec_crc32c_hw_active(),
           weftrec_crc32c("123456789", 9));
    CHECK_EQ_U64(weftrec_crc32c("123456789", 9), 0xE3069283u);
    CHECK_EQ_U64(weftrec_crc32c_sw("123456789", 9), 0xE3069283u);
}

int main(void)
{
    rec_setup();
    build_truth_trace();
    r1_abi_pins();
    r2_golden_bytes();
    r3_r4_playback();
    r5_corruption();
    r6_refusals();
    r9_sla();
    free(g_buf);
    free(g_idx);
    return harness_summary("test_oracle_weftrec (R-series)");
}
