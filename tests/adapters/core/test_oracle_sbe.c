/*
 * test_oracle_sbe.c - Pillar 6 golden oracle: SBE schema-driven decoder,
 *                     canonical Weft-MD template, direct-to-Weft
 *                     projection transcoder, error ladder.
 *
 * S-series checks:
 *   S1  message header decode (block/template/schema/version)
 *   S2  full decode: fixed scalars, groups, var symbol (zero-copy)
 *   S3  group entry accessors (scalars + bounds)
 *   S4  direct-to-weft projection (add-order shaped 64B messages)
 *   S5  projection capacity stop + EALIGN
 *   S6  ESCHEMA ladder (template/schema/version/block length, bad
 *       schema descriptors, group field slot != 0)
 *   S7  ETRUNC ladder (byte-exact truncation sweep)
 *   S8  EBADMSG (group dimension block length mismatch)
 *   S9  EBOUNDS (accessor out-of-range)
 *   S10 signed-kind sign extension (I8/I16/I32/I64)
 *   S11 fixture round-trip (byte-identical + stream walk)
 */
#include "test_util.h"

int main(void)
{
    static const sbe_golden_entry_t e1[] = {
        { 123450000ull, 0x0100000000000001ull, 300u, 'B', 'A' },
        { 99999999ull,  0x0100000000000002ull, 200u, 'S', 'A' }
    };
    static const sbe_golden_entry_t e2[] = { { 0, 0, 0, 0, 0 } };
    static const sbe_golden_entry_t e3[] = {
        { 456700000ull, 0x0100000000000004ull, 700u, 'S', 'A' },
        { 456700100ull, 0x0100000000000005ull, 100u, 'B', 'A' },
        { 456700200ull, 0x0100000000000006ull, 900u, 'B', 'A' }
    };

    static uint8_t msg1[256], msg2[256], msg3[256];
    size_t n1, n2, n3;
    const weft_sbe_schema_t *sch = weft_sbe_schema_md();
    weft_sbe_view_t v;
    int32_t st;

    CHECK(sch != NULL);
    CHECK_U64X(sch->template_id, 1u);
    CHECK_U64X(sch->schema_id, 0x5746u);
    CHECK_U64X(sch->version, 0u);
    CHECK_U64X(sch->block_length, 24u);
    CHECK_U64X(sch->num_fixed, 4u);
    CHECK_U64X(sch->num_groups, 1u);
    CHECK_U64X(sch->num_var, 1u);

    n1 = sbe_md_build(msg1, sizeof msg1, GOLDEN_TS_BASE, 0x00C0FFEEu, 1u,
                      0x5Au, e1, 2, "AAPL");
    n2 = sbe_md_build(msg2, sizeof msg2, GOLDEN_TS_BASE + 1000,
                      0x00C0FFEFu, 2u, 0x11u, e2, 0, NULL);
    n3 = sbe_md_build(msg3, sizeof msg3, GOLDEN_TS_BASE + 2000,
                      0x00C0FFF0u, 3u, 0x22u, e3, 3, "MSFT");
    CHECK(n1 > 0 && n2 > 0 && n3 > 0);

    /* ---------------- S1: header decode ---------------- */
    {
        uint16_t bl = 0, tid = 0, sid = 0, ver = 0;
        st = weft_sbe_decode_header(msg1, n1, &bl, &tid, &sid, &ver);
        CHECK_I64(st, WEFT_ADAPTER_OK);
        CHECK_U64X(bl, 24u);
        CHECK_U64X(tid, 1u);
        CHECK_U64X(sid, 0x5746u);
        CHECK_U64X(ver, 0u);
        CHECK_I64(weft_sbe_decode_header(msg1, 7u, &bl, &tid, &sid, &ver),
                  WEFT_ADAPTER_ETRUNC);
        CHECK_I64(weft_sbe_decode_header(NULL, 8u, &bl, &tid, &sid, &ver),
                  WEFT_ADAPTER_EINVAL);
    }

    /* ---------------- S2: full decode msg1 ---------------- */
    memset(&v, 0, sizeof v);
    st = weft_sbe_decode(msg1, n1, sch, &v);
    CHECK_I64(st, WEFT_ADAPTER_OK);
    if (st == WEFT_ADAPTER_OK) {
        CHECK_U64X(v.num_fixed, 4u);
        CHECK_U64X(v.num_groups, 1u);
        CHECK_U64X(v.num_var, 1u);
        CHECK_U64X(v.scalar[0], GOLDEN_TS_BASE);
        CHECK_U64X(v.scalar[1], 0x00C0FFEEu);
        CHECK_U64X(v.scalar[2], 1u);
        CHECK_U64X(v.scalar[3], 0x5Au);
        CHECK_U64X(v.group[0].count, 2u);
        CHECK_U64X(v.group[0].block_length, 32u);
        CHECK_U64X(v.total_size, n1);

        /* zero-copy proof: var symbol points INTO the source buffer */
        CHECK(v.var[0].data == msg1 + 8u + 24u + 2u + 2u * 32u + 1u);
        CHECK_U64X(v.var[0].length, 4u);
        CHECK(memcmp(v.var[0].data, "AAPL", 4) == 0);

        /* group entries */
        {
            uint64_t price = 0, ref = 0, shares = 0, side = 0, action = 0;
            CHECK_I64(weft_sbe_group_entry(&v, 0u, 0u, 0u, &price),
                      WEFT_ADAPTER_OK);
            CHECK_U64X(price, 123450000ull);
            CHECK_I64(weft_sbe_group_entry(&v, 0u, 0u, 1u, &ref),
                      WEFT_ADAPTER_OK);
            CHECK_U64X(ref, 0x0100000000000001ull);
            CHECK_I64(weft_sbe_group_entry(&v, 0u, 1u, 2u, &shares),
                      WEFT_ADAPTER_OK);
            CHECK_U64X(shares, 200u);
            CHECK_I64(weft_sbe_group_entry(&v, 0u, 1u, 3u, &side),
                      WEFT_ADAPTER_OK);
            CHECK_U64X(side, (uint64_t)'S');
            CHECK_I64(weft_sbe_group_entry(&v, 0u, 0u, 4u, &action),
                      WEFT_ADAPTER_OK);
            CHECK_U64X(action, (uint64_t)'A');

            /* S9: accessor bounds */
            CHECK_I64(weft_sbe_group_entry(&v, 1u, 0u, 0u, &price),
                      WEFT_ADAPTER_EBOUNDS);
            CHECK_I64(weft_sbe_group_entry(&v, 0u, 2u, 0u, &price),
                      WEFT_ADAPTER_EBOUNDS);
            CHECK_I64(weft_sbe_group_entry(&v, 0u, 0u, 5u, &price),
                      WEFT_ADAPTER_EBOUNDS);
            CHECK_I64(weft_sbe_group_entry(NULL, 0u, 0u, 0u, &price),
                      WEFT_ADAPTER_EINVAL);
            {
                const uint8_t *d = NULL;
                uint16_t dl = 0;
                CHECK_I64(weft_sbe_group_bytes(&v, 0u, 0u, 0u, &d, &dl),
                          WEFT_ADAPTER_ESCHEMA); /* not a CHARS field */
                CHECK_I64(weft_sbe_group_bytes(&v, 0u, 9u, 0u, &d, &dl),
                          WEFT_ADAPTER_EBOUNDS);
            }
        }
    }

    /* ---------------- S2b: msg2 (0 entries, no symbol) ---------------- */
    memset(&v, 0, sizeof v);
    st = weft_sbe_decode(msg2, n2, sch, &v);
    CHECK_I64(st, WEFT_ADAPTER_OK);
    if (st == WEFT_ADAPTER_OK) {
        CHECK_U64X(v.group[0].count, 0u);
        CHECK_U64X(v.num_var, 1u);
        CHECK_U64X(v.var[0].length, 0u);
        CHECK_U64X(v.total_size, n2);
    }

    /* ---------------- S4: projection msg1 -> add orders ---------------- */
    {
        WEFT_ALIGNED64 static weft_itch_msg_t out[8];
        int32_t pst = 0;
        size_t cnt;

        memset(out, 0, sizeof out);
        cnt = weft_sbe_md_to_itch_add(&v, out, 0u, &pst);
        CHECK_U64X(cnt, 0u);
        CHECK_I64(pst, WEFT_ADAPTER_EINVAL);

        memset(&v, 0, sizeof v);
        CHECK_I64(weft_sbe_decode(msg1, n1, sch, &v), WEFT_ADAPTER_OK);
        cnt = weft_sbe_md_to_itch_add(&v, out, 8, &pst);
        CHECK_U64X(cnt, 2u);
        CHECK_I64(pst, WEFT_ADAPTER_OK);
        if (cnt == 2u) {
            CHECK_U64X(out[0].hdr.msg_type, (uint64_t)'A');
            CHECK_U64X(out[0].hdr.stock_locate, 0xFFEEu);
            CHECK_U64X(out[0].hdr.tracking_number, 0u);
            CHECK_U64X(out[0].hdr.timestamp_ns, GOLDEN_TS_BASE);
            CHECK_U64X(out[0].u.add.order_ref, 0x0100000000000001ull);
            CHECK_U64X(out[0].u.add.buy_sell, (uint64_t)'B');
            CHECK_U64X(out[0].u.add.shares, 300u);
            CHECK_U64X(out[0].u.add.price_raw, 123450000u);
            CHECK(memcmp(out[0].u.add.stock, "AAPL    ", 8) == 0);
            CHECK_U64X(out[1].u.add.order_ref, 0x0100000000000002ull);
            CHECK_U64X(out[1].u.add.shares, 200u);
            CHECK_U64X(out[1].u.add.price_raw, 99999999u);
            CHECK(memcmp(out[1].u.add.stock, "AAPL    ", 8) == 0);
        }

        /* S5: capacity stop */
        memset(&v, 0, sizeof v);
        CHECK_I64(weft_sbe_decode(msg3, n3, sch, &v), WEFT_ADAPTER_OK);
        memset(out, 0, sizeof out);
        cnt = weft_sbe_md_to_itch_add(&v, out, 2, &pst);
        CHECK_U64X(cnt, 2u);
        CHECK_I64(pst, WEFT_ADAPTER_OK);
        cnt = weft_sbe_md_to_itch_add(&v, out, 8, &pst);
        CHECK_U64X(cnt, 3u);
        CHECK_I64(pst, WEFT_ADAPTER_OK);
        if (cnt == 3u) {
            CHECK(memcmp(out[0].u.add.stock, "MSFT    ", 8) == 0);
            CHECK_U64X(out[0].hdr.stock_locate, 0xFFF0u);
            CHECK_U64X(out[2].u.add.shares, 900u);
        }

        /* S5b: EALIGN */
        {
            WEFT_ALIGNED64 static uint8_t raw[256];
            weft_itch_msg_t *bad = (weft_itch_msg_t *)(raw + 8);
            cnt = weft_sbe_md_to_itch_add(&v, bad, 4, &pst);
            CHECK_U64X(cnt, 0u);
            CHECK_I64(pst, WEFT_ADAPTER_EALIGN);
        }

        /* symbol longer than 8 -> EBOUNDS */
        {
            static uint8_t big[256];
            static const sbe_golden_entry_t ebig[] = {
                { 1000ull, 0x0100000000000009ull, 10u, 'B', 'A' }
            };
            size_t nb = sbe_md_build(big, sizeof big, GOLDEN_TS_BASE,
                                     7u, 9u, 0u, ebig, 1,
                                     "ABCDEFGHXYZ");  /* 11 chars */
            CHECK(nb > 0);
            memset(&v, 0, sizeof v);
            CHECK_I64(weft_sbe_decode(big, nb, sch, &v), WEFT_ADAPTER_OK);
            cnt = weft_sbe_md_to_itch_add(&v, out, 8, &pst);
            CHECK_U64X(cnt, 0u);
            CHECK_I64(pst, WEFT_ADAPTER_EBOUNDS);
        }
    }

    /* ---------------- S6: ESCHEMA ladder ---------------- */
    {
        uint16_t bl, tid, sid, ver;
        static uint8_t tmp[256];

        /* wrong template id */
        memcpy(tmp, msg1, n1);
        put_le16(tmp + 2, 2u);
        CHECK_I64(weft_sbe_decode(tmp, n1, sch, &v), WEFT_ADAPTER_ESCHEMA);
        /* wrong schema id */
        memcpy(tmp, msg1, n1);
        put_le16(tmp + 4, 0x5747u);
        CHECK_I64(weft_sbe_decode(tmp, n1, sch, &v), WEFT_ADAPTER_ESCHEMA);
        /* wrong version */
        memcpy(tmp, msg1, n1);
        put_le16(tmp + 6, 1u);
        CHECK_I64(weft_sbe_decode(tmp, n1, sch, &v), WEFT_ADAPTER_ESCHEMA);
        /* wrong block length */
        memcpy(tmp, msg1, n1);
        put_le16(tmp + 0, 25u);
        CHECK_I64(weft_sbe_decode(tmp, n1, sch, &v), WEFT_ADAPTER_ESCHEMA);

        (void)bl; (void)tid; (void)sid; (void)ver;

        /* bad schema descriptors */
        {
            weft_sbe_schema_t bad = *sch;
            bad.num_fixed = (uint16_t)(WEFT_SBE_MAX_FIXED + 1u);
            CHECK_I64(weft_sbe_decode(msg1, n1, &bad, &v),
                      WEFT_ADAPTER_ESCHEMA);
            bad = *sch;
            bad.num_groups = (uint16_t)(WEFT_SBE_MAX_GROUPS + 1u);
            CHECK_I64(weft_sbe_decode(msg1, n1, &bad, &v),
                      WEFT_ADAPTER_ESCHEMA);
            bad = *sch;
            bad.num_var = (uint16_t)(WEFT_SBE_MAX_VAR + 1u);
            CHECK_I64(weft_sbe_decode(msg1, n1, &bad, &v),
                      WEFT_ADAPTER_ESCHEMA);
            /* fixed field exceeding block */
            {
                static weft_sbe_field_desc_t f[4];
                memcpy(f, sch->fixed, sizeof f);
                f[0].offset = 22u;   /* 22 + 8 > 24 */
                bad = *sch;
                bad.fixed = f;
                CHECK_I64(weft_sbe_decode(msg1, n1, &bad, &v),
                          WEFT_ADAPTER_ESCHEMA);
            }
            /* group field slot != 0 (reserved) */
            {
                static weft_sbe_field_desc_t gf[5];
                memcpy(gf, sch->group_fields, sizeof gf);
                gf[0].slot = 1u;
                bad = *sch;
                bad.group_fields = gf;
                CHECK_I64(weft_sbe_decode(msg1, n1, &bad, &v),
                          WEFT_ADAPTER_ESCHEMA);
            }
            /* slot out of range on fixed */
            {
                static weft_sbe_field_desc_t f[4];
                memcpy(f, sch->fixed, sizeof f);
                f[0].slot = (uint16_t)WEFT_SBE_MAX_FIXED;
                bad = *sch;
                bad.fixed = f;
                CHECK_I64(weft_sbe_decode(msg1, n1, &bad, &v),
                          WEFT_ADAPTER_ESCHEMA);
            }
            /* view from a different schema */
            {
                weft_sbe_view_t v2;
                memset(&v2, 0, sizeof v2);
                CHECK_I64(weft_sbe_decode(msg1, n1, sch, &v2),
                          WEFT_ADAPTER_OK);
                {
                    WEFT_ALIGNED64 static weft_itch_msg_t out[8];
                    int32_t pst = 0;
                    weft_sbe_schema_t notmd = *sch;
                    notmd.template_id = 2u;
                    v2.schema = &notmd;
                    CHECK_U64X(weft_sbe_md_to_itch_add(&v2, out, 8, &pst), 0u);
                    CHECK_I64(pst, WEFT_ADAPTER_ESCHEMA);
                }
            }
        }

        CHECK_I64(weft_sbe_decode(NULL, n1, sch, &v), WEFT_ADAPTER_EINVAL);
        CHECK_I64(weft_sbe_decode(msg1, n1, NULL, &v), WEFT_ADAPTER_EINVAL);
        CHECK_I64(weft_sbe_decode(msg1, n1, sch, NULL), WEFT_ADAPTER_EINVAL);
    }

    /* ---------------- S7: truncation sweep ---------------- */
    {
        size_t L;
        for (L = 0; L < n1; ++L) {
            st = weft_sbe_decode(msg1, L, sch, &v);
            CHECK_I64(st, WEFT_ADAPTER_ETRUNC);
            CHECK_I64(v.status, WEFT_ADAPTER_ETRUNC);
        }
    }

    /* ---------------- S8: group dimension mismatch ---------------- */
    {
        static uint8_t tmp[256];
        memcpy(tmp, msg1, n1);
        tmp[32] = 31u;  /* group block length != 32 */
        CHECK_I64(weft_sbe_decode(tmp, n1, sch, &v), WEFT_ADAPTER_EBADMSG);
    }

    /* ---------------- S10: signed kinds sign extension ---------------- */
    {
        static weft_sbe_field_desc_t sf[4];
        static const weft_sbe_schema_t sschema = {
            7u, 0x5746u, 0u, 16u, 4u, 0u, 0u, 0u, sf, NULL, NULL
        };
        static uint8_t sb[32];
        uint64_t val = 0;

        sf[0].offset = 0;  sf[0].size = 1;  sf[0].kind = (uint16_t)WEFT_SBE_K_I8;  sf[0].slot = 0;
        sf[1].offset = 1;  sf[1].size = 2;  sf[1].kind = (uint16_t)WEFT_SBE_K_I16; sf[1].slot = 1;
        sf[2].offset = 4;  sf[2].size = 4;  sf[2].kind = (uint16_t)WEFT_SBE_K_I32; sf[2].slot = 2;
        sf[3].offset = 8;  sf[3].size = 8;  sf[3].kind = (uint16_t)WEFT_SBE_K_I64; sf[3].slot = 3;

        put_le16(sb + 0, 16u);
        put_le16(sb + 2, 7u);
        put_le16(sb + 4, 0x5746u);
        put_le16(sb + 6, 0u);
        sb[8] = 0xFFu;                 /* I8  -1     */
        put_le16(sb + 9, 0x8000u);     /* I16 -32768 */
        put_le32(sb + 12, 0x80000000u);/* I32 INT_MIN*/
        put_le64(sb + 16, 0x8000000000000000ull); /* I64 INT64_MIN */
        memset(sb + 24, 0, 8);

        memset(&v, 0, sizeof v);
        CHECK_I64(weft_sbe_decode(sb, 8u + 16u, &sschema, &v),
                  WEFT_ADAPTER_OK);
        CHECK_U64X(v.scalar[0], 0xFFFFFFFFFFFFFFFFull);
        CHECK_U64X(v.scalar[1], 0xFFFFFFFFFFFF8000ull);
        CHECK_U64X(v.scalar[2], 0xFFFFFFFF80000000ull);
        CHECK_U64X(v.scalar[3], 0x8000000000000000ull);
        CHECK_U64X(v.scalar_kind[0], (uint64_t)WEFT_SBE_K_I8);

        /* i32 positive */
        put_le32(sb + 12, 12345u);
        memset(&v, 0, sizeof v);
        CHECK_I64(weft_sbe_decode(sb, 24u, &sschema, &v), WEFT_ADAPTER_OK);
        CHECK_U64X(v.scalar[2], 12345u);
        val = 0; (void)val;
    }

    /* ---------------- S11: fixture round-trip ---------------- */
    {
        char path[512];
        static uint8_t expect[4096];
        static uint8_t filebuf[262144];
        long n;
        size_t off = 0;
        const char *dir = getenv("WEFT_FIXTURES_DIR");
        if (!dir || !*dir) { dir = "tests/adapters/core/fixtures"; }

        off += sbe_md_build(expect + off, sizeof expect - off, GOLDEN_TS_BASE,
                            0x00C0FFEEu, 1u, 0x5Au, e1, 2, "AAPL");
        off += sbe_md_build(expect + off, sizeof expect - off,
                            GOLDEN_TS_BASE + 1000, 0x00C0FFEFu, 2u, 0x11u,
                            e2, 0, NULL);
        off += sbe_md_build(expect + off, sizeof expect - off,
                            GOLDEN_TS_BASE + 2000, 0x00C0FFF0u, 3u, 0x22u,
                            e3, 3, "MSFT");

        snprintf(path, sizeof path, "%s/sbe_golden.bin", dir);
        n = read_file(path, filebuf, sizeof filebuf);
        if (n < 0) {
            CHECK(golden_write_fixtures(dir) == 0);
            n = read_file(path, filebuf, sizeof filebuf);
        }
        CHECK(n == (long)off);
        if (n == (long)off) {
            CHECK(memcmp(filebuf, expect, off) == 0);
        }

        /* stream walk via total_size (only when the fixture is sane) */
        if (n > 0) {
            size_t pos = 0, msgs = 0;
            static const uint64_t want_ts[3] = {
                GOLDEN_TS_BASE, GOLDEN_TS_BASE + 1000, GOLDEN_TS_BASE + 2000
            };
            static const uint64_t want_cnt[3] = { 2u, 0u, 3u };
            while (pos + 8u <= (size_t)n && msgs < 3u) {
                memset(&v, 0, sizeof v);
                st = weft_sbe_decode(filebuf + pos, (size_t)n - pos,
                                     sch, &v);
                CHECK_I64(st, WEFT_ADAPTER_OK);
                if (st != WEFT_ADAPTER_OK) { break; }
                CHECK_U64X(v.scalar[0], want_ts[msgs]);
                CHECK_U64X(v.group[0].count, want_cnt[msgs]);
                pos += v.total_size;
                ++msgs;
            }
            CHECK_U64X(msgs, 3u);
            CHECK_U64X(pos, (size_t)n);
        }
    }

    REPORT_AND_EXIT("oracle_sbe");
}
