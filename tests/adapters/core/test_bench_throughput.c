/*
 * test_bench_throughput.c - Pillar 6 throughput scorecard (D-61 §5).
 *
 * All measurements are SINGLE-CORE on cached buffers (buffers are
 * built once and hammered repeatedly; working set fits L2/L3).
 * Emits machine-parseable lines:  BENCH|key|value|unit
 *
 * Benchmarks:
 *   B1  ITCH 5.0 fixed-framing decode (realistic volume mix)
 *   B2  ITCH 5.0 length-prefix (MoldUDP64 block) decode
 *   B3  OUCH 5.0 decode
 *   B4  SBE canonical-MD decode (+ projection amortized separately)
 *   B5  SBE MD -> Weft projection (per projected message)
 *   B6  MoldUDP64 packet walk (open + all next + decode)
 *   B7  CRC-32C selected impl vs slicing-by-8 reference
 *   B8  Adler-32 vectorized vs portable
 *   B9  Endianness microbench: BE64 wire->host transform rate
 */
#include "test_util.h"

#define BENCH_STREAM (4u << 20)   /* 4 MB */
#define MIN_RUN_MS   250.0

typedef struct {
    size_t msgs;
    double ms;
} run_t;

static double best_of(double *runs, int n)
{
    double b = runs[0];
    int i;
    for (i = 1; i < n; ++i) {
        if (runs[i] < b) { b = runs[i]; }
    }
    return b;
}

int main(void)
{
    static WEFT_ALIGNED64 uint8_t stream[BENCH_STREAM];
    WEFT_ALIGNED64 static weft_itch_msg_t ibatch[8192];
    WEFT_ALIGNED64 static weft_ouch_msg_t obatch[8192];
    WEFT_ALIGNED64 static weft_itch_msg_t proj[8192];

    /* realistic ITCH volume mix (approximate production shares) */
    static const uint8_t mix[] = {
        'A','A','F','A','E','A','E','C','A','X','F','E','A','D','U','A',
        'E','A','P','X','A','F','E','A','D','A','E','Q','A','X','U','A',
        'A','E','F','A','X','A','E','D','A','P','A','E','F','A','X','I'
    };
    const size_t mix_n = sizeof(mix) / sizeof(mix[0]);

    uint64_t rng = 0x1234ABCD9876FEDCull;
    size_t off = 0, i;
    long total_msgs_mix = 0;

    /* ---------- build the 4 MB mixed stream (fixed framing) ---------- */
    while (off + 64u < BENCH_STREAM) {
        uint8_t t = mix[weft_prng(&rng) % mix_n];
        size_t n = itch_encode_msg(stream + off, &g_golden_itch[0]);
        /* patch type-specific message quickly: rebuild via golden of
         * the same type where available, else hand-roll generic */
        {
            size_t gi;
            int found = 0;
            for (gi = 0; gi < GOLDEN_ITCH_COUNT; ++gi) {
                if (g_golden_itch[gi].type == t) {
                    n = itch_encode_msg(stream + off, &g_golden_itch[gi]);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                /* every mix type exists in the golden table */
                n = 0;
            }
            if (n == 0) { continue; }
        }
        off += n;
        ++total_msgs_mix;
    }
    {
        size_t stream_len = off;
        double runs[3];
        int r;

        /* ---------------- B1: ITCH fixed framing ---------------- */
        for (r = 0; r < 3; ++r) {
            double t0, t1;
            unsigned long long total = 0;
            do {
                size_t pos = 0, cnt;
                int32_t st = 0;
                size_t consumed = 0;
                t0 = weft_now_ms();
                while (pos < stream_len) {
                    cnt = weft_itch50_decode_batch(stream + pos,
                                                   stream_len - pos, 0u,
                                                   ibatch, 8192, &st,
                                                   &consumed);
                    total += cnt;
                    pos += consumed;
                    if (st != WEFT_ADAPTER_OK) { break; }
                }
                t1 = weft_now_ms();
            } while (0);
            runs[r] = t1 - t0;
            printf("BENCH|itch_fixed_mmsgs|%.3f|Mmsg/s\n",
                   (double)total / (runs[r] * 1000.0));
            printf("BENCH|itch_fixed_ns|%.2f|ns/msg\n",
                   runs[r] * 1000000.0 / (double)total);
        }
        {
            double best = best_of(runs, 3);
            printf("BENCH|itch_fixed_mmsgs_best|%.3f|Mmsg/s\n",
                   (double)total_msgs_mix / (best * 1000.0));
            printf("BENCH|itch_fixed_ns_best|%.2f|ns/msg\n",
                   best * 1000000.0 / (double)total_msgs_mix);
        }

        /* ---------------- B2: length-prefix framing ---------------- */
        {
            /* build the prefixed variant of the same stream */
            static WEFT_ALIGNED64 uint8_t lstream[BENCH_STREAM];
            size_t pos = 0, lpos = 0;
            unsigned long long total = 0;
            int32_t st = 0;
            size_t consumed = 0, cnt;
            double t0, t1;

            while (pos < stream_len) {
                size_t w = weft_itch50_wire_size(stream[pos]);
                if (w == 0) { break; }
                put_be16(lstream + lpos, (uint16_t)w);
                memcpy(lstream + lpos + 2, stream + pos, w);
                lpos += 2 + w;
                pos += w;
            }
            for (r = 0; r < 3; ++r) {
                size_t lpos2 = 0;
                total = 0;
                t0 = weft_now_ms();
                while (lpos2 < lpos) {
                    cnt = weft_itch50_decode_batch(
                        lstream + lpos2, lpos - lpos2,
                        WEFT_ITCH_F_LENPREFIX, ibatch, 8192, &st,
                        &consumed);
                    total += cnt;
                    lpos2 += consumed;
                    if (st != WEFT_ADAPTER_OK) { break; }
                }
                t1 = weft_now_ms();
                runs[r] = t1 - t0;
                printf("BENCH|itch_len_mmsgs|%.3f|Mmsg/s\n",
                       (double)total / (runs[r] * 1000.0));
                printf("BENCH|itch_len_ns|%.2f|ns/msg\n",
                       runs[r] * 1000000.0 / (double)total);
            }
            {
                double best = best_of(runs, 3);
                printf("BENCH|itch_len_mmsgs_best|%.3f|Mmsg/s\n",
                       (double)total_msgs_mix / (best * 1000.0));
                printf("BENCH|itch_len_ns_best|%.2f|ns/msg\n",
                       best * 1000000.0 / (double)total_msgs_mix);
            }

            /* ---------------- B3: OUCH ---------------- */
            {
                static WEFT_ALIGNED64 uint8_t ostream[BENCH_STREAM];
                size_t opos = 0;
                unsigned long long ototal = 0;
                const uint8_t *cur;
                size_t cur_len;
                size_t step = 0;

                /* replicate the golden OUCH 'A'/'E' mix */
                while (opos + 64u < BENCH_STREAM) {
                    const golden_msg_t *g =
                        (step & 1u) ? &g_golden_ouch[1] : &g_golden_ouch[2];
                    size_t n = ouch_encode_msg(ostream + opos + 2, g);
                    put_be16(ostream + opos, (uint16_t)n);
                    opos += 2 + n;
                    ++step;
                }
                cur = ostream;
                cur_len = opos;
                {
                    double t0b, t1b;
                    t0b = weft_now_ms();
                    while (cur_len > 0) {
                        cnt = weft_ouch50_decode_batch(cur, cur_len, obatch,
                                                       8192, &st, &consumed);
                        ototal += cnt;
                        cur += consumed;
                        cur_len -= consumed;
                        if (st != WEFT_ADAPTER_OK || consumed == 0) { break; }
                    }
                    t1b = weft_now_ms();
                    printf("BENCH|ouch_mmsgs|%.3f|Mmsg/s\n",
                           (double)ototal / ((t1b - t0b) * 1000.0));
                    printf("BENCH|ouch_ns|%.2f|ns/msg\n",
                           (t1b - t0b) * 1000000.0 / (double)ototal);
                }
            }

            /* ---------------- B4/B5: SBE ---------------- */
            {
                static const sbe_golden_entry_t ent[] = {
                    { 123450000ull, 0x0100000000000001ull, 300u, 'B', 'A' },
                    { 99999999ull,  0x0100000000000002ull, 200u, 'S', 'A' }
                };
                static WEFT_ALIGNED64 uint8_t sstream[BENCH_STREAM];
                size_t spos = 0, smsg = 0;
                unsigned long long stotal = 0, ptotal = 0;
                double t0b, t1b;

                while (spos + 128u < BENCH_STREAM) {
                    spos += sbe_md_build(sstream + spos,
                                         BENCH_STREAM - spos,
                                         GOLDEN_TS_BASE, 0x00C0FFEEu,
                                         (uint32_t)smsg + 1u, 0x5Au,
                                         ent, 2, "AAPL");
                    ++smsg;
                }
                {
                    size_t pos2 = 0;
                    t0b = weft_now_ms();
                    while (pos2 < spos) {
                        weft_sbe_view_t v;
                        int32_t s2 = weft_sbe_decode(sstream + pos2,
                                                     spos - pos2,
                                                     weft_sbe_schema_md(),
                                                     &v);
                        if (s2 != WEFT_ADAPTER_OK) { break; }
                        stotal += 1;
                        pos2 += v.total_size;
                    }
                    t1b = weft_now_ms();
                    printf("BENCH|sbe_md_mmsgs|%.3f|Mmsg/s\n",
                           (double)stotal / ((t1b - t0b) * 1000.0));
                    printf("BENCH|sbe_md_ns|%.2f|ns/msg\n",
                           (t1b - t0b) * 1000000.0 / (double)stotal);
                }
                {
                    /* projection over the same decoded views */
                    weft_sbe_view_t v;
                    size_t pos2 = 0;
                    t0b = weft_now_ms();
                    while (pos2 < spos) {
                        int32_t s2 = weft_sbe_decode(sstream + pos2,
                                                     spos - pos2,
                                                     weft_sbe_schema_md(),
                                                     &v);
                        int32_t pst = 0;
                        if (s2 != WEFT_ADAPTER_OK) { break; }
                        ptotal += weft_sbe_md_to_itch_add(&v, proj, 8192,
                                                          &pst);
                        pos2 += v.total_size;
                    }
                    t1b = weft_now_ms();
                    printf("BENCH|sbe_proj_mmsgs|%.3f|Mmsg/s\n",
                           (double)ptotal / ((t1b - t0b) * 1000.0));
                    printf("BENCH|sbe_proj_ns|%.2f|ns/msg\n",
                           (t1b - t0b) * 1000000.0 / (double)ptotal);
                }
            }

            /* ---------------- B6: MoldUDP64 walk ---------------- */
            {
                /* one big packet with many blocks, repeated */
                static WEFT_ALIGNED64 uint8_t mstream[BENCH_STREAM];
                size_t mpos = 0;
                unsigned long long mtotal = 0;
                double t0b, t1b;

                while (mpos + 4096u < BENCH_STREAM) {
                    size_t n = mold_build_packet(
                        mstream + mpos, BENCH_STREAM - mpos,
                        "WEFTBENCH", 1u, g_golden_itch, 9);
                    mpos += n;
                }
                {
                    size_t pos2 = 0;
                    t0b = weft_now_ms();
                    while (pos2 + 16u <= mpos) {
                        weft_mold_view_t mv;
                        if (weft_mold64_open(mstream + pos2, mpos - pos2,
                                             &mv) == WEFT_ADAPTER_OK) {
                            const uint8_t *msg = NULL;
                            uint16_t mlen = 0;
                            while (weft_mold64_next(&mv, &msg, &mlen) > 0) {
                                size_t sz = 0;
                                if (weft_itch50_decode_one(
                                        msg - 2, (size_t)mlen + 2u,
                                        WEFT_ITCH_F_LENPREFIX,
                                        &ibatch[0], &sz) ==
                                    WEFT_ADAPTER_OK) {
                                    ++mtotal;
                                }
                            }
                            pos2 += (size_t)(mv.cursor - (mstream + pos2));
                        } else {
                            pos2 += 1;
                        }
                    }
                    t1b = weft_now_ms();
                    printf("BENCH|mold_walk_mmsgs|%.3f|Mmsg/s\n",
                           (double)mtotal / ((t1b - t0b) * 1000.0));
                    printf("BENCH|mold_walk_ns|%.2f|ns/msg\n",
                           (t1b - t0b) * 1000000.0 / (double)mtotal);
                }
            }
        }
    }

    /* ---------------- B7: CRC-32C ---------------- */
    {
        double t0, t1;
        unsigned long long acc = 0;
        int r;
        for (r = 0; r < 3; ++r) {
            t0 = weft_now_ms();
            acc += weft_adapter_crc32c(stream, BENCH_STREAM);
            t1 = weft_now_ms();
            printf("BENCH|crc32c_gbps|%.3f|GB/s\n",
                   (double)BENCH_STREAM / ((t1 - t0) * 1e6));
        }
        for (r = 0; r < 3; ++r) {
            t0 = weft_now_ms();
            acc += weft_adapter_crc32c_sw(stream, BENCH_STREAM);
            t1 = weft_now_ms();
            printf("BENCH|crc32c_sw_gbps|%.3f|GB/s\n",
                   (double)BENCH_STREAM / ((t1 - t0) * 1e6));
        }
        (void)acc;
    }

    /* ---------------- B8: Adler-32 ---------------- */
    {
        double t0, t1;
        unsigned long long acc = 0;
        int r;
        for (r = 0; r < 3; ++r) {
            t0 = weft_now_ms();
            acc += weft_adapter_adler32(stream, BENCH_STREAM);
            t1 = weft_now_ms();
            printf("BENCH|adler32_gbps|%.3f|GB/s\n",
                   (double)BENCH_STREAM / ((t1 - t0) * 1e6));
        }
        for (r = 0; r < 1; ++r) {
            t0 = weft_now_ms();
            acc += weft_adapter_adler32_portable(1u, stream, BENCH_STREAM);
            t1 = weft_now_ms();
            printf("BENCH|adler32_portable_gbps|%.3f|GB/s\n",
                   (double)BENCH_STREAM / ((t1 - t0) * 1e6));
        }
        (void)acc;
    }

    /* ---------------- B9: endianness transform rate ---------------- */
    {
        /* mirrors the parser's BE64 wire->host path: memcpy + bswap64 */
        double t0, t1;
        uint64_t acc = 0;
        size_t j;
        int r;
        for (r = 0; r < 3; ++r) {
            t0 = weft_now_ms();
            for (j = 0; j + 8u <= 1024u * 1024u; j += 8u) {
                uint64_t v;
                memcpy(&v, stream + j, 8);
                acc += __builtin_bswap64(v);
            }
            t1 = weft_now_ms();
            printf("BENCH|be64_swap_gbps|%.3f|GB/s\n",
                   (double)(1024u * 1024u) / ((t1 - t0) * 1e6));
        }
        /* liveness anchor: acc feeds a branch, so the transform
         * cannot be dead-code eliminated (vectorization is allowed) */
        if (acc == 0x1234567890ABCDEFull) { printf("unlikely\n"); }
    }

    printf("impl: crc32c=%s adler32=%s\n",
           weft_adapter_crc32c_impl_name(),
           weft_adapter_adler32_impl_name());
    printf("TEST bench_throughput: informational (no failure conditions)\n");
    (void)i;
    return 0;
}
