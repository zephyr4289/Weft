/*
 * test_checksum.c - Pillar 6 oracle: CRC-32C, Adler-32, WAF frames.
 *
 * K-series checks:
 *   K1  CRC-32C normative vectors ("123456789" == 0xE3069283,
 *       32 zero bytes == 0x8A9136AA, empty == 0)
 *   K2  CRC-32C hardware-vs-reference cross-check (random slices)
 *   K3  CRC-32C incremental == one-shot
 *   K4  Adler-32 normative vectors ("Wikipedia" == 0x11E60398)
 *   K5  Adler-32 vectorized-vs-portable cross-check (random slices)
 *   K6  Adler-32 incremental == one-shot
 *   K7  WAF emit/scan round-trip (random payloads)
 *   K8  WAF negative ladder (ECRC/EBADMSG/ETRUNC/EALIGN)
 */
#include "test_util.h"

int main(void)
{
    static uint8_t scratch[1 << 20]; /* 1 MB deterministic random-ish */
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    size_t i;

    /* deterministic pseudo-random fill */
    for (i = 0; i < sizeof scratch; ++i) {
        scratch[i] = (uint8_t)(weft_prng(&rng) >> 32);
    }

    printf("impl: crc32c=%s adler32=%s\n",
           weft_adapter_crc32c_impl_name(),
           weft_adapter_adler32_impl_name());

    /* ---------------- K1: CRC-32C vectors ---------------- */
    CHECK_U64X(weft_adapter_crc32c("123456789", 9), 0xE3069283ull);
    {
        static const uint8_t zeros[32];
        CHECK_U64X(weft_adapter_crc32c(zeros, 32), 0x8A9136AAull);
        CHECK_U64X(weft_adapter_crc32c(zeros, 0), 0ull);
    }
    /* the software reference must agree on the same vectors */
    CHECK_U64X(weft_adapter_crc32c_sw("123456789", 9), 0xE3069283ull);

    /* ---------------- K2: hw vs sw cross-check ---------------- */
    {
        int ok = 1;
        for (i = 0; i < 512; ++i) {
            size_t off = (size_t)(weft_prng(&rng) %
                                  (sizeof scratch - 8192));
            size_t len = (size_t)(weft_prng(&rng) % 8192);
            uint32_t a = weft_adapter_crc32c(scratch + off, len);
            uint32_t b = weft_adapter_crc32c_sw(scratch + off, len);
            if (a != b) { ok = 0; break; }
            CHECK_U64X(a, b);
        }
        CHECK(ok);
        /* unaligned starts (exercise byte tails of both kernels) */
        for (i = 0; i < 128; ++i) {
            size_t off = (size_t)(weft_prng(&rng) %
                                  (sizeof scratch - 260));
            size_t len = (size_t)(weft_prng(&rng) % 256) + 1;
            CHECK_U64X(weft_adapter_crc32c(scratch + off, len),
                       weft_adapter_crc32c_sw(scratch + off, len));
        }
    }

    /* ---------------- K3: incremental == one-shot ---------------- */
    {
        for (i = 0; i < 64; ++i) {
            size_t off = (size_t)(weft_prng(&rng) %
                                  (sizeof scratch - 4096));
            size_t len = (size_t)(weft_prng(&rng) % 4096);
            size_t split = (size_t)(weft_prng(&rng) % (len + 1));
            uint32_t whole = weft_adapter_crc32c(scratch + off, len);
            uint32_t part = weft_adapter_crc32c(scratch + off, split);
            part = weft_adapter_crc32c_update(part, scratch + off + split,
                                              len - split);
            CHECK_U64X(part, whole);
        }
    }

    /* ---------------- K4: Adler-32 vectors ---------------- */
    {
        CHECK_U64X(weft_adapter_adler32("", 0), 1ull);
        CHECK_U64X(weft_adapter_adler32("Wikipedia", 9), 0x11E60398ull);
        CHECK_U64X(weft_adapter_adler32("a", 1), 0x00620062ull);
        CHECK_U64X(weft_adapter_adler32_portable(1u, "Wikipedia", 9),
                   0x11E60398ull);
    }

    /* ---------------- K5: vectorized vs portable ---------------- */
    {
        int ok = 1;
        for (i = 0; i < 512; ++i) {
            size_t off = (size_t)(weft_prng(&rng) %
                                  (sizeof scratch - 16384));
            size_t len = (size_t)(weft_prng(&rng) % 16384);
            uint32_t a = weft_adapter_adler32(scratch + off, len);
            uint32_t b = weft_adapter_adler32_portable(1u, scratch + off, len);
            if (a != b) { ok = 0; break; }
            CHECK_U64X(a, b);
        }
        CHECK(ok);
        /* sweep lengths 0..64 x unaligned offsets: tail + block edges */
        for (i = 0; i < 64; ++i) {
            size_t len = i;
            size_t off = (i * 7u) % 32u;
            CHECK_U64X(weft_adapter_adler32(scratch + off, len),
                       weft_adapter_adler32_portable(1u, scratch + off, len));
        }
        /* exactly at the 4096-byte reduction boundary and around it */
        for (i = 4090; i <= 4102; ++i) {
            CHECK_U64X(weft_adapter_adler32(scratch, i),
                       weft_adapter_adler32_portable(1u, scratch, i));
        }
    }

    /* ---------------- K6: adler incremental == one-shot ---------------- */
    {
        for (i = 0; i < 64; ++i) {
            size_t len = (size_t)(weft_prng(&rng) % 12288);
            size_t split = (size_t)(weft_prng(&rng) % (len + 1));
            uint32_t whole = weft_adapter_adler32(scratch, len);
            uint32_t part = weft_adapter_adler32(scratch, split);
            part = weft_adapter_adler32_update(part, scratch + split,
                                               len - split);
            CHECK_U64X(part, whole);
        }
    }

    /* ---------------- K7: WAF round-trip ---------------- */
    {
        static WEFT_ALIGNED64 uint8_t frame[8192];
        for (i = 0; i < 256; ++i) {
            size_t plen = (size_t)(weft_prng(&rng) % 512);
            int32_t wr = weft_adapter_waf_emit(frame, sizeof frame,
                                               scratch, plen);
            weft_waf_view_t wv;
            CHECK_I64(wr, (int32_t)(12u + plen));
            CHECK_I64(weft_adapter_waf_scan(frame, (size_t)wr, &wv),
                      WEFT_ADAPTER_OK);
            CHECK(wv.payload == frame + 8);
            CHECK_U64X(wv.payload_len, plen);
            CHECK_U64X(wv.crc_wire, wv.crc_calc);
        }
        /* emit negatives */
        CHECK_I64(weft_adapter_waf_emit(frame, 12u, scratch, 1u),
                  WEFT_ADAPTER_ETRUNC);
        CHECK_I64(weft_adapter_waf_emit(frame + 1, 64u, scratch, 1u),
                  WEFT_ADAPTER_EALIGN);
        CHECK_I64(weft_adapter_waf_emit(NULL, 64u, scratch, 1u),
                  WEFT_ADAPTER_EINVAL);
    }

    /* ---------------- K8: WAF negatives ---------------- */
    {
        static WEFT_ALIGNED64 uint8_t frame[256];
        weft_waf_view_t wv;
        int32_t wr = weft_adapter_waf_emit(frame, sizeof frame,
                                           scratch, 100u);
        CHECK(wr == 112);

        /* payload corruption -> ECRC */
        frame[8 + 50] ^= 0xFFu;
        CHECK_I64(weft_adapter_waf_scan(frame, (size_t)wr, &wv),
                  WEFT_ADAPTER_ECRC);
        frame[8 + 50] ^= 0xFFu;

        /* bad magic -> EBADMSG */
        frame[0] = 'X';
        CHECK_I64(weft_adapter_waf_scan(frame, (size_t)wr, &wv),
                  WEFT_ADAPTER_EBADMSG);
        frame[0] = 'W';

        /* truncation -> ETRUNC */
        CHECK_I64(weft_adapter_waf_scan(frame, 11u, &wv),
                  WEFT_ADAPTER_ETRUNC);
        CHECK_I64(weft_adapter_waf_scan(frame, 111u, &wv),
                  WEFT_ADAPTER_ETRUNC);

        /* declared payload longer than buffer -> ETRUNC */
        {
            static WEFT_ALIGNED64 uint8_t f2[64];
            uint32_t big = 1000u;
            put_be32(f2, WEFT_WAF_MAGIC);
            put_be32(f2 + 4, big);
            CHECK_I64(weft_adapter_waf_scan(f2, 64u, &wv),
                      WEFT_ADAPTER_ETRUNC);
        }

        /* unaligned buffer -> EALIGN */
        CHECK_I64(weft_adapter_waf_scan(frame + 1, (size_t)wr - 1u, &wv),
                  WEFT_ADAPTER_EALIGN);

        /* args */
        CHECK_I64(weft_adapter_waf_scan(NULL, 64u, &wv),
                  WEFT_ADAPTER_EINVAL);
        CHECK_I64(weft_adapter_waf_scan(frame, 64u, NULL),
                  WEFT_ADAPTER_EINVAL);
    }

    REPORT_AND_EXIT("checksum");
}
