/*
 * test_oracle_itch50.c - Pillar 6 golden oracle: ITCH 5.0 / OUCH 5.0 /
 *                        MoldUDP64 bit-exact verification + fixtures.
 *
 * O-series checks:
 *   O1  wire-size oracle (all 256 type codes)
 *   O2  in-memory golden decode, fixed framing (23 msgs, field-exact)
 *   O3  in-memory golden decode, MoldUDP64 length-prefix framing
 *   O4  decode determinism (two decodes, full 64B memcmp)
 *   O5  fixture round-trip: files byte-identical to in-memory builds
 *   O6  fixture integrity sidecar (crc32c + adler32 recomputed)
 *   O7  golden PCAP walk: Ethernet/IP/UDP + MoldUDP64 + 23 msgs
 *   O8  negative ladder (EINVAL/EALIGN/ETRUNC/EBADMSG + fail-closed
 *       untouched-output proof)
 *   O9  batch semantics (capacity stop, corrupt-stop, prefix trust)
 *   O10 OUCH 5.0 golden decode + negatives
 *   O11 MoldUDP64 walker negatives (truncation, end-of-session)
 */
#include "test_util.h"

static const char *fixtures_dir(void)
{
    const char *d = getenv("WEFT_FIXTURES_DIR");
    return (d && *d) ? d : "tests/adapters/core/fixtures";
}

int main(void)
{
    static uint8_t wire_fixed[4096];
    static uint8_t wire_len[4096];
    static uint8_t wire_ouch[4096];
    static uint8_t filebuf[262144];
    WEFT_ALIGNED64 static weft_itch_msg_t batch_a[64];
    WEFT_ALIGNED64 static weft_itch_msg_t batch_b[64];
    WEFT_ALIGNED64 static weft_ouch_msg_t obatch[16];
    size_t n_fixed, n_len, n_ouch, i;
    int32_t st;
    size_t consumed = 0;

    /* ---------------- O1: wire-size oracle ---------------- */
    {
        static const struct { uint8_t t; size_t sz; } tbl[] = {
            { 'S', 12 }, { 'R', 39 }, { 'H', 25 }, { 'Y', 20 },
            { 'L', 26 }, { 'V', 35 }, { 'W', 12 }, { 'K', 28 },
            { 'J', 47 }, { 'A', 36 }, { 'F', 40 }, { 'E', 31 },
            { 'C', 36 }, { 'X', 23 }, { 'D', 19 }, { 'U', 35 },
            { 'P', 44 }, { 'Q', 40 }, { 'I', 50 }
        };
        size_t k, known = 0;
        for (k = 0; k < 256; ++k) {
            size_t got = weft_itch50_wire_size((uint8_t)k);
            size_t want = 0;
            size_t j;
            for (j = 0; j < sizeof(tbl) / sizeof(tbl[0]); ++j) {
                if (tbl[j].t == (uint8_t)k) { want = tbl[j].sz; ++known; }
            }
            CHECK_U64X(got, want);
        }
        CHECK_U64X(known, 19);
    }

    /* ---------------- O2: golden decode, fixed framing ---------------- */
    n_fixed = itch_golden_build_fixed(wire_fixed, sizeof wire_fixed);
    memset(batch_a, 0, sizeof batch_a);
    {
        size_t cnt = weft_itch50_decode_batch(wire_fixed, n_fixed, 0u,
                                              batch_a, 64, &st, &consumed);
        CHECK_U64X(cnt, GOLDEN_ITCH_COUNT);
        CHECK_I64(st, WEFT_ADAPTER_OK);
        CHECK_U64X(consumed, n_fixed);
        for (i = 0; i < GOLDEN_ITCH_COUNT; ++i) {
            CHECK(verify_itch(&g_golden_itch[i], &batch_a[i]) == 0);
        }
    }

    /* ---------------- O3: golden decode, LENPREFIX framing ---------------- */
    n_len = itch_golden_build_lenprefixed(wire_len, sizeof wire_len);
    memset(batch_b, 0, sizeof batch_b);
    {
        size_t cnt = weft_itch50_decode_batch(
            wire_len, n_len, WEFT_ITCH_F_LENPREFIX,
            batch_b, 64, &st, &consumed);
        CHECK_U64X(cnt, GOLDEN_ITCH_COUNT);
        CHECK_I64(st, WEFT_ADAPTER_OK);
        CHECK_U64X(consumed, n_len);
        for (i = 0; i < GOLDEN_ITCH_COUNT; ++i) {
            CHECK(verify_itch(&g_golden_itch[i], &batch_b[i]) == 0);
        }
    }

    /* ---------------- O4: determinism (full 64B memcmp) ---------------- */
    CHECK(memcmp(batch_a, batch_b, sizeof batch_a) == 0);

    /* decode_one sizes agree with the oracle */
    {
        size_t off = 0;
        for (i = 0; i < GOLDEN_ITCH_COUNT; ++i) {
            size_t sz = 0;
            memset(&batch_b[0], 0, sizeof batch_b[0]);
            st = weft_itch50_decode_one(wire_fixed + off, n_fixed - off,
                                        0u, &batch_b[0], &sz);
            CHECK_I64(st, WEFT_ADAPTER_OK);
            CHECK_U64X(sz, weft_itch50_wire_size(g_golden_itch[i].type));
            CHECK(verify_itch(&g_golden_itch[i], &batch_b[0]) == 0);
            off += sz;
        }
        CHECK_U64X(off, n_fixed);
    }

    /* ---------------- O5/O6: fixtures ---------------- */
    {
        char path[512];
        long n;

        /* regenerate if missing */
        snprintf(path, sizeof path, "%s/checksums.txt", fixtures_dir());
        if (read_file(path, filebuf, sizeof filebuf) < 0) {
            CHECK(golden_write_fixtures(fixtures_dir()) == 0);
        }

        /* fixed stream fixture is byte-identical */
        snprintf(path, sizeof path, "%s/itch50_golden_fixed.bin",
                 fixtures_dir());
        n = read_file(path, filebuf, sizeof filebuf);
        CHECK(n == (long)n_fixed);
        if (n == (long)n_fixed) {
            CHECK(memcmp(filebuf, wire_fixed, n_fixed) == 0);
        }

        /* lenprefixed stream fixture is byte-identical */
        snprintf(path, sizeof path, "%s/itch50_golden_len.bin",
                 fixtures_dir());
        n = read_file(path, filebuf, sizeof filebuf);
        CHECK(n == (long)n_len);
        if (n == (long)n_len) {
            CHECK(memcmp(filebuf, wire_len, n_len) == 0);
        }

        /* OUCH stream fixture is byte-identical */
        n_ouch = ouch_golden_build(wire_ouch, sizeof wire_ouch);
        snprintf(path, sizeof path, "%s/ouch50_golden.bin",
                 fixtures_dir());
        n = read_file(path, filebuf, sizeof filebuf);
        CHECK(n == (long)n_ouch);
        if (n == (long)n_ouch) {
            CHECK(memcmp(filebuf, wire_ouch, n_ouch) == 0);
        }

        /* PCAP fixture is byte-identical to a fresh in-memory build */
        {
            static uint8_t pcap_mem[262144];
            static uint8_t mold[65536];
            static const char sess[10] = "WEFTGOLDEN";
            size_t off = 0, m;
            put_le32(pcap_mem + 0, 0xA1B2C3D4u);
            put_le16(pcap_mem + 4, 2);
            put_le16(pcap_mem + 6, 4);
            put_le32(pcap_mem + 8, 0);
            put_le32(pcap_mem + 12, 0);
            put_le32(pcap_mem + 16, 65535u);
            put_le32(pcap_mem + 20, 1u);
            off = 24;
            m = mold_build_packet(mold, sizeof mold, sess, 1u,
                                  g_golden_itch, 9);
            off += pcap_wrap_packet(pcap_mem + off, sizeof pcap_mem - off,
                                    mold, m, 1u, 1760000000u, 0u);
            m = mold_build_packet(mold, sizeof mold, sess, 10u,
                                  g_golden_itch + 9, 8);
            off += pcap_wrap_packet(pcap_mem + off, sizeof pcap_mem - off,
                                    mold, m, 2u, 1760000000u, 1000u);
            m = mold_build_packet(mold, sizeof mold, sess, 18u,
                                  g_golden_itch + 17, 6);
            off += pcap_wrap_packet(pcap_mem + off, sizeof pcap_mem - off,
                                    mold, m, 3u, 1760000000u, 2000u);

            snprintf(path, sizeof path, "%s/itch50_golden.pcap",
                     fixtures_dir());
            n = read_file(path, filebuf, sizeof filebuf);
            CHECK(n == (long)off);
            if (n == (long)off) {
                CHECK(memcmp(filebuf, pcap_mem, off) == 0);
            }
        }

        /* checksum sidecar integrity */
        snprintf(path, sizeof path, "%s/checksums.txt", fixtures_dir());
        n = read_file(path, filebuf, sizeof filebuf);
        CHECK(n > 0);
        if (n > 0) {
            static char lines[4096];
            char *save = NULL;
            size_t nlines = 0;
            CHECK(n < (long)sizeof lines);
            memcpy(lines, filebuf, (size_t)n);
            lines[n < (long)sizeof lines ? n : (long)sizeof lines - 1] = '\0';
            /* parse: "name crc32c=%08x adler32=%08x size=%ld" */
            {
                char *ln = strtok_r(lines, "\n", &save);
                while (ln) {
                    char fname[64];
                    unsigned c = 0, a = 0;
                    long sz = 0, got;
                    static uint8_t fb[262144];
                    char fpath[512];
                    if (sscanf(ln, "%63s crc32c=%x adler32=%x size=%ld",
                               fname, &c, &a, &sz) == 4) {
                        snprintf(fpath, sizeof fpath, "%s/%s",
                                 fixtures_dir(), fname);
                        got = read_file(fpath, fb, sizeof fb);
                        CHECK(got == sz);
                        if (got == sz) {
                            CHECK_U64X(weft_adapter_crc32c(fb, (size_t)got), c);
                            CHECK_U64X(weft_adapter_adler32(fb, (size_t)got), a);
                        }
                        ++nlines;
                    }
                    ln = strtok_r(NULL, "\n", &save);
                }
            }
            CHECK_U64X(nlines, 5);
        }
    }

    /* ---------------- O7: golden PCAP walk ---------------- */
    {
        char path[512];
        long n;
        snprintf(path, sizeof path, "%s/itch50_golden.pcap",
                 fixtures_dir());
        n = read_file(path, filebuf, sizeof filebuf);
        CHECK(n > 24);
        if (n > 24) {
            const uint8_t *p = filebuf;
            const uint8_t *end = filebuf + n;
            size_t msg_idx = 0;
            uint32_t pkt_idx = 0;
            static const uint32_t want_seq[3] = { 1u, 10u, 18u };
            static const uint16_t want_count[3] = { 9u, 8u, 6u };

            CHECK(get_le16(p + 4) == 2 && get_le16(p + 6) == 4);
            {
                uint32_t lt = (uint32_t)p[20] | ((uint32_t)p[21] << 8) |
                              ((uint32_t)p[22] << 16) | ((uint32_t)p[23] << 24);
                CHECK_U64X(lt, 1u); /* Ethernet */
            }
            p += 24;

            while (p + 16 <= end && pkt_idx < 3) {
                uint32_t incl = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                                ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
                const uint8_t *pkt = p + 16;
                const uint8_t *udp;
                const uint8_t *moldpkt;
                size_t mold_len;
                weft_mold_view_t mv;

                CHECK(p + 16 + incl <= end);
                CHECK_U64X(pkt[12], 0x08u);
                CHECK_U64X(pkt[13], 0x00u);            /* IPv4         */
                CHECK_U64X(pkt[14] >> 4, 4u);          /* version      */
                CHECK_U64X(pkt[14] & 0xFu, 5u);        /* IHL          */
                CHECK_U64X(pkt[23], 17u);              /* UDP proto    */
                {
                    /* IP header checksum verification */
                    uint8_t hdr[20];
                    uint16_t stored, calc;
                    memcpy(hdr, pkt + 14, 20);
                    stored = get_be16(hdr + 10);
                    hdr[10] = 0;
                    hdr[11] = 0;
                    calc = ipv4_checksum(hdr, 20);
                    CHECK_U64X(calc, stored);
                }
                udp = pkt + 14 + 20;
                CHECK_U64X(get_be16(udp + 2), 40001u);  /* dest port */
                moldpkt = udp + 8;
                mold_len = incl - 14u - 20u - 8u;

                st = weft_mold64_open(moldpkt, mold_len, &mv);
                CHECK_I64(st, WEFT_ADAPTER_OK);
                if (st == WEFT_ADAPTER_OK) {
                    CHECK(memcmp(mv.session, "WEFTGOLDEN", 10) == 0);
                    CHECK_U64X(mv.sequence, want_seq[pkt_idx]);
                    CHECK_U64X(mv.count, want_count[pkt_idx]);
                    for (;;) {
                        const uint8_t *msg = NULL;
                        uint16_t mlen = 0;
                        int32_t r = weft_mold64_next(&mv, &msg, &mlen);
                        if (r == 0) { break; }
                        CHECK_I64(r, 1);
                        if (r == 1 && msg) {
                            size_t sz = 0;
                            /* msg points AFTER the 2-byte block length;
                             * hand the decoder the block start so the
                             * LENPREFIX framing validates the prefix. */
                            st = weft_itch50_decode_one(msg - 2,
                                                        (size_t)mlen + 2u,
                                                        WEFT_ITCH_F_LENPREFIX,
                                                        &batch_a[0], &sz);
                            CHECK_I64(st, WEFT_ADAPTER_OK);
                            CHECK_U64X(sz, (size_t)mlen + 2u);
                            if (st == WEFT_ADAPTER_OK &&
                                msg_idx < GOLDEN_ITCH_COUNT) {
                                CHECK(verify_itch(&g_golden_itch[msg_idx],
                                                  &batch_a[0]) == 0);
                            }
                            ++msg_idx;
                        }
                    }
                }
                ++pkt_idx;
                p += 16 + incl;
            }
            CHECK_U64X(pkt_idx, 3u);
            CHECK_U64X(msg_idx, GOLDEN_ITCH_COUNT);
        }
    }

    /* ---------------- O8: negative ladder ---------------- */
    {
        uint8_t tmp[64];
        size_t sz = 0;
        static uint8_t pat[64];
        WEFT_ALIGNED64 static weft_itch_msg_t m;
        size_t k;
        /* true stream offset of the first 'A' (add order) message */
        size_t a_off = 0;
        for (i = 0; i < GOLDEN_ITCH_COUNT && g_golden_itch[i].type != 'A'; ++i) {
            a_off += weft_itch50_wire_size(g_golden_itch[i].type);
        }
        CHECK(i < GOLDEN_ITCH_COUNT);

        /* arg validation */
        CHECK_I64(weft_itch50_decode_one(NULL, 16, 0u, &m, &sz),
                  WEFT_ADAPTER_EINVAL);
        CHECK_I64(weft_itch50_decode_one(wire_fixed, 16, 0u, NULL, &sz),
                  WEFT_ADAPTER_EINVAL);
        CHECK_I64(weft_itch50_decode_one(wire_fixed, 16, 0x8u, &m, &sz),
                  WEFT_ADAPTER_EINVAL);

        /* alignment: single needs 8B */
        {
            WEFT_ALIGNED64 static uint8_t raw[128];
            weft_itch_msg_t *bad = (weft_itch_msg_t *)(raw + 1);
            CHECK_I64(weft_itch50_decode_one(wire_fixed, 36, 0u, bad, &sz),
                      WEFT_ADAPTER_EALIGN);
        }
        /* alignment: batch needs 64B */
        {
            WEFT_ALIGNED64 static uint8_t raw[256];
            weft_itch_msg_t *bad = (weft_itch_msg_t *)(raw + 8);
            CHECK_I64(weft_itch50_decode_batch(wire_fixed, n_fixed, 0u,
                                               bad, 4, &st, &consumed),
                      0u);
            CHECK_I64(st, WEFT_ADAPTER_EALIGN);
        }

        /* truncation of every message at len-1 */
        {
            size_t off = 0;
            for (i = 0; i < GOLDEN_ITCH_COUNT; ++i) {
                size_t w = weft_itch50_wire_size(g_golden_itch[i].type);
                CHECK_I64(weft_itch50_decode_one(wire_fixed + off, w - 1u,
                                                 0u, &m, &sz),
                          WEFT_ADAPTER_ETRUNC);
                off += w;
            }
        }

        /* unknown type */
        memcpy(tmp, wire_fixed + a_off, 36); /* an 'A' message */
        tmp[0] = 0x00;
        CHECK_I64(weft_itch50_decode_one(tmp, 36, 0u, &m, &sz),
                  WEFT_ADAPTER_EBADMSG);
        tmp[0] = 'Z';
        CHECK_I64(weft_itch50_decode_one(tmp, 36, 0u, &m, &sz),
                  WEFT_ADAPTER_EBADMSG);

        /* bad side char */
        memcpy(tmp, wire_fixed + a_off, 36);
        tmp[19] = 'X';
        CHECK_I64(weft_itch50_decode_one(tmp, 36, 0u, &m, &sz),
                  WEFT_ADAPTER_EBADMSG);

        /* bad timestamp (>= 24h) */
        memcpy(tmp, wire_fixed + a_off, 36);
        memset(tmp + 5, 0xFF, 6);
        CHECK_I64(weft_itch50_decode_one(tmp, 36, 0u, &m, &sz),
                  WEFT_ADAPTER_EBADMSG);

        /* bad event code */
        memcpy(tmp, wire_fixed, 12);
        tmp[11] = 'Z';
        CHECK_I64(weft_itch50_decode_one(tmp, 12, 0u, &m, &sz),
                  WEFT_ADAPTER_EBADMSG);

        /* zero shares on 'A' */
        memcpy(tmp, wire_fixed + a_off, 36);
        memset(tmp + 20, 0, 4);
        CHECK_I64(weft_itch50_decode_one(tmp, 36, 0u, &m, &sz),
                  WEFT_ADAPTER_EBADMSG);

        /* non-printable symbol bytes */
        memcpy(tmp, wire_fixed + a_off, 36);
        tmp[24] = 0x01;
        CHECK_I64(weft_itch50_decode_one(tmp, 36, 0u, &m, &sz),
                  WEFT_ADAPTER_EBADMSG);

        /* fail-closed: output untouched on error */
        memcpy(tmp, wire_fixed + a_off, 36);
        tmp[19] = 'X';
        memset(pat, 0xA5, sizeof pat);
        memcpy(&m, pat, sizeof m);
        CHECK_I64(weft_itch50_decode_one(tmp, 36, 0u, &m, &sz),
                  WEFT_ADAPTER_EBADMSG);
        CHECK(memcmp(&m, pat, sizeof m) == 0);

        /* LENPREFIX framing policy */
        {
            static uint8_t lp[64];
            memcpy(lp + 2, wire_fixed + a_off, 36);
            put_be16(lp, 35u);  /* too short for 'A' */
            CHECK_I64(weft_itch50_decode_one(lp, 38, WEFT_ITCH_F_LENPREFIX,
                                             &m, &sz),
                      WEFT_ADAPTER_EBADMSG);
            put_be16(lp, 37u);  /* oversized, strict */
            CHECK_I64(weft_itch50_decode_one(lp, 39, WEFT_ITCH_F_LENPREFIX,
                                             &m, &sz),
                      WEFT_ADAPTER_EBADMSG);
            put_be16(lp, 37u);  /* oversized, lenient accepts */
            CHECK_I64(weft_itch50_decode_one(lp, 39,
                                             WEFT_ITCH_F_LENPREFIX |
                                             WEFT_ITCH_F_LENIENT,
                                             &m, &sz),
                      WEFT_ADAPTER_OK);
            CHECK_U64X(sz, 39u);
            put_be16(lp, 0u);
            CHECK_I64(weft_itch50_decode_one(lp, 2, WEFT_ITCH_F_LENPREFIX,
                                             &m, &sz),
                      WEFT_ADAPTER_EBADMSG);
            CHECK_I64(weft_itch50_decode_one(lp, 1, WEFT_ITCH_F_LENPREFIX,
                                             &m, &sz),
                      WEFT_ADAPTER_ETRUNC);
        }

        /* ---------------- O9: batch semantics ---------------- */
        {
            static uint8_t stream[512];
            size_t off = 0;
            /* 6 good 'A' messages then a corrupt one */
            for (k = 0; k < 7; ++k) {
                memcpy(stream + off, wire_fixed + a_off, 36);
                off += 36;
            }
            stream[6 * 36 + 19] = 'X'; /* corrupt msg 6 */

            memset(batch_a, 0, sizeof batch_a);
            {
                size_t cnt = weft_itch50_decode_batch(stream, off, 0u,
                                                      batch_a, 64,
                                                      &st, &consumed);
                CHECK_U64X(cnt, 6u);
                CHECK_I64(st, WEFT_ADAPTER_EBADMSG);
                CHECK_U64X(consumed, 6u * 36u);
                /* the first 6 remain trustworthy */
                for (k = 0; k < 6; ++k) {
                    CHECK_U64X(batch_a[k].u.add.order_ref,
                               0x0100000000000001ull);
                }
            }
            /* capacity stop: clean */
            {
                size_t cnt = weft_itch50_decode_batch(wire_fixed, n_fixed,
                                                      0u, batch_b, 5,
                                                      &st, &consumed);
                CHECK_U64X(cnt, 5u);
                CHECK_I64(st, WEFT_ADAPTER_OK);
                CHECK_U64X(consumed,
                           12u + 39u + 25u + 20u + 26u);
            }
        }
    }

    /* ---------------- O10: OUCH golden + negatives ---------------- */
    {
        size_t cnt;
        memset(obatch, 0, sizeof obatch);
        cnt = weft_ouch50_decode_batch(wire_ouch, n_ouch, obatch, 16,
                                       &st, &consumed);
        CHECK_U64X(cnt, GOLDEN_OUCH_COUNT);
        CHECK_I64(st, WEFT_ADAPTER_OK);
        CHECK_U64X(consumed, n_ouch);
        for (i = 0; i < GOLDEN_OUCH_COUNT; ++i) {
            CHECK(verify_ouch(&g_golden_ouch[i], &obatch[i]) == 0);
        }

        /* negatives */
        {
            uint8_t frame[64];
            size_t sz = 0;
            WEFT_ALIGNED64 static weft_ouch_msg_t om;

            CHECK_I64(weft_ouch50_decode_one(NULL, 16, &om, &sz),
                      WEFT_ADAPTER_EINVAL);
            CHECK_I64(weft_ouch50_decode_one(wire_ouch, 2, &om, &sz),
                      WEFT_ADAPTER_ETRUNC);

            /* take the golden 'A' (accepted) message and vary framing */
            {
                size_t off = 0, i2;
                for (i2 = 0; i2 < GOLDEN_OUCH_COUNT; ++i2) {
                    if (g_golden_ouch[i2].type == 'A') { break; }
                    off += 2u + ouch_encode_msg(frame, &g_golden_ouch[i2]);
                }
                memcpy(frame, wire_ouch + off, 2u + 46u);
            }

            put_be16(frame, 45u);  /* prefix < oracle size */
            CHECK_I64(weft_ouch50_decode_one(frame, 47, &om, &sz),
                      WEFT_ADAPTER_EBADMSG);
            put_be16(frame, 46u);  /* buffer too short */
            CHECK_I64(weft_ouch50_decode_one(frame, 47, &om, &sz),
                      WEFT_ADAPTER_ETRUNC);
            put_be16(frame, 46u);  /* exact fit */
            CHECK_I64(weft_ouch50_decode_one(frame, 48, &om, &sz),
                      WEFT_ADAPTER_OK);
            CHECK_U64X(sz, 48u);
            put_be16(frame, 47u);  /* prefix > oracle size */
            CHECK_I64(weft_ouch50_decode_one(frame, 49, &om, &sz),
                      WEFT_ADAPTER_EBADMSG);
            put_be16(frame, 0u);   /* zero prefix: avail<3 truncates first */
            CHECK_I64(weft_ouch50_decode_one(frame, 2, &om, &sz),
                      WEFT_ADAPTER_ETRUNC);
            CHECK_I64(weft_ouch50_decode_one(frame, 3, &om, &sz),
                      WEFT_ADAPTER_EBADMSG);
            frame[2] = 'Z';        /* unknown type */
            put_be16(frame, 46u);
            CHECK_I64(weft_ouch50_decode_one(frame, 48, &om, &sz),
                      WEFT_ADAPTER_EBADMSG);
        }
    }

    /* ---------------- O11: MoldUDP64 negatives ---------------- */
    {
        static uint8_t mold[65536];
        weft_mold_view_t mv;
        const uint8_t *msg = NULL;
        uint16_t mlen = 0;
        size_t n;

        n = mold_build_packet(mold, sizeof mold, "WEFTGOLDEN", 1u,
                              g_golden_itch, 2);
        CHECK_I64(weft_mold64_open(mold, 15, &mv), WEFT_ADAPTER_ETRUNC);
        CHECK_I64(weft_mold64_open(mold, n, &mv), WEFT_ADAPTER_OK);
        CHECK_I64(weft_mold64_next(&mv, &msg, &mlen), 1);
        CHECK_U64X(mlen, 12u);
        CHECK_I64(weft_mold64_next(&mv, &msg, &mlen), 1);
        CHECK_U64X(mlen, 39u);
        CHECK_I64(weft_mold64_next(&mv, &msg, &mlen), 0);

        /* truncated block stream: declared count 2, only 1 block fits */
        CHECK_I64(weft_mold64_open(mold, 16u + 2u + 12u, &mv),
                  WEFT_ADAPTER_OK);
        CHECK_I64(weft_mold64_next(&mv, &msg, &mlen), 1);
        CHECK_I64(weft_mold64_next(&mv, &msg, &mlen), WEFT_ADAPTER_ETRUNC);

        /* end-of-session zero-length block */
        put_be16(mold + 16, 0u);
        CHECK_I64(weft_mold64_open(mold, 18, &mv), WEFT_ADAPTER_OK);
        CHECK_I64(weft_mold64_next(&mv, &msg, &mlen), 0);
    }

    REPORT_AND_EXIT("oracle_itch50");
}
