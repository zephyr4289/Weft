/*
 * test_fuzz_torture.c - Weft Pillar 6 fuzz & torture suite.
 *
 * 10,000,000 malformed/truncated byte-injection cycles (default;
 * override with WEFT_FUZZ_CYCLES) across all six decode surfaces:
 *   0: ITCH 5.0 fixed framing
 *   1: ITCH 5.0 MoldUDP64 length-prefix framing
 *   2: OUCH 5.0 SoupBinTCP payload framing
 *   3: SBE canonical-Weft-MD stream
 *   4: WAF frames
 *   5: MoldUDP64 packets
 *
 * Invariants enforced EVERY cycle:
 *   F1  no crash (implicit; signal-free completion of the run)
 *   F2  status is a legal weft_adapter_status code
 *   F3  consumed <= len (no overrun of the input contract)
 *   F4  decoded count <= out capacity
 *   F5  (every 1024th cycle) determinism: re-decode, full 64B memcmp
 *
 * Continuously checked across the whole run:
 *   F6  zero heap growth (glibc mallinfo2 delta == 0, non-ASAN builds)
 *   F7  zero allocation events (malloc interposition, when built with
 *       -DWEFT_ENABLE_MALLOC_PROBE)
 *
 * The run prints a per-target status histogram (audit input for
 * D-61 section 7).
 */
#include "test_util.h"

#if defined(WEFT_ENABLE_MALLOC_PROBE)
#  include "test_alloc_intercept.h"
#endif

#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
#  if defined(__has_feature)
#    if __has_feature(address_sanitizer)
#      define WEFT_NO_MALLINFO 1
#    endif
#  endif
#  if !defined(WEFT_NO_MALLINFO)
#    include <malloc.h>
#    if defined(__GLIBC_PREREQ)
#      if __GLIBC_PREREQ(2, 33)
#        define WEFT_HAVE_MALLINFO2 1
#      endif
#    endif
#  endif
#endif

typedef struct {
    unsigned long long ok;
    unsigned long long err;
    unsigned long long hist[16];
} fuzz_stat_t;

static int status_slot(int32_t st)
{
    if (st == 0) { return 0; }
    if (st >= -8 && st <= -1) { return 1 - st; } /* -1..-8 -> 1..8 */
    return 15; /* illegal */
}

static const char *status_name(int32_t st)
{
    switch (st) {
    case WEFT_ADAPTER_OK:      return "OK";
    case WEFT_ADAPTER_EINVAL:  return "EINVAL";
    case WEFT_ADAPTER_EBADMSG: return "EBADMSG";
    case WEFT_ADAPTER_ETRUNC:  return "ETRUNC";
    case WEFT_ADAPTER_EALIGN:  return "EALIGN";
    case WEFT_ADAPTER_ENOSUP:  return "ENOSUP";
    case WEFT_ADAPTER_ECRC:    return "ECRC";
    case WEFT_ADAPTER_ESCHEMA: return "ESCHEMA";
    case WEFT_ADAPTER_EBOUNDS: return "EBOUNDS";
    default:                    return "ILLEGAL";
    }
}

int main(void)
{
    static uint8_t itch_fixed[4096];
    static uint8_t itch_len[4096];
    static uint8_t ouch_stream[4096];
    static uint8_t sbe_stream[512];
    static uint8_t mold_pkt[4096];
    static WEFT_ALIGNED64 uint8_t waf_frame[4096];
    static WEFT_ALIGNED64 uint8_t scratch[4096];
    WEFT_ALIGNED64 static weft_itch_msg_t ibatch[32];
    WEFT_ALIGNED64 static weft_itch_msg_t ibatch2[32];
    WEFT_ALIGNED64 static weft_ouch_msg_t obatch[32];

    static const sbe_golden_entry_t ent[] = {
        { 123450000ull, 0x0100000000000001ull, 300u, 'B', 'A' },
        { 99999999ull,  0x0100000000000002ull, 200u, 'S', 'A' }
    };

    size_t n_fixed, n_len, n_ouch, n_sbe, n_mold;
    int32_t waf_wr;
    long cycles = weft_env_cycles("WEFT_FUZZ_CYCLES", 10000000L);
    uint64_t rng = 0xDEADBEEFCAFEBABEull;
    unsigned long long c;
    fuzz_stat_t stat[6];
#if defined(WEFT_HAVE_MALLINFO2)
    long long heap_before = 0, heap_after = 0;
#endif

    memset(stat, 0, sizeof stat);

    /* build the golden base buffers (deterministic) */
    n_fixed = itch_golden_build_fixed(itch_fixed, sizeof itch_fixed);
    n_len = itch_golden_build_lenprefixed(itch_len, sizeof itch_len);
    n_ouch = ouch_golden_build(ouch_stream, sizeof ouch_stream);
    n_sbe = sbe_md_build(sbe_stream, sizeof sbe_stream, GOLDEN_TS_BASE,
                         0x00C0FFEEu, 1u, 0x5Au, ent, 2, "AAPL");
    n_mold = mold_build_packet(mold_pkt, sizeof mold_pkt, "WEFTGOLDEN", 1u,
                               g_golden_itch, 9);
    waf_wr = weft_adapter_waf_emit(waf_frame, sizeof waf_frame,
                                   itch_fixed, 256u);
    CHECK(waf_wr > 0);

#if defined(WEFT_HAVE_MALLINFO2)
    {
        struct mallinfo2 mi = mallinfo2();
        heap_before = (long long)mi.uordblks + (long long)mi.hblkhd;
    }
#endif
#if defined(WEFT_ENABLE_MALLOC_PROBE)
    probe_reset();
#endif

    for (c = 0; c < (unsigned long long)cycles; ++c) {
        unsigned target = (unsigned)(weft_prng(&rng) % 6);
        unsigned mode;
        int intact;
        const uint8_t *base;
        size_t base_len;
        size_t len;
        int32_t st = WEFT_ADAPTER_OK;
        size_t consumed = 0;

        switch (target) {
        case 0: base = itch_fixed; base_len = n_fixed; break;
        case 1: base = itch_len;   base_len = n_len;   break;
        case 2: base = ouch_stream; base_len = n_ouch; break;
        case 3: base = sbe_stream;  base_len = n_sbe;  break;
        case 4: base = waf_frame;   base_len = (size_t)waf_wr; break;
        default: base = mold_pkt;  base_len = n_mold;  break;
        }

        /* mutation: copy then mutate scratch */
        memcpy(scratch, base, base_len);
        len = base_len;
        mode = (unsigned)(weft_prng(&rng) % 6);
        intact = (mode == 5u);

        switch (mode) {
        case 0: /* truncate anywhere (incl. empty) */
            len = (size_t)(weft_prng(&rng) % (len + 1));
            break;
        case 1: { /* flip 1..4 bytes */
            unsigned k, flips = 1u + (unsigned)(weft_prng(&rng) % 4);
            for (k = 0; k < flips && len > 0; ++k) {
                size_t pos = (size_t)(weft_prng(&rng) % len);
                scratch[pos] ^= (uint8_t)(weft_prng(&rng) >> 40);
            }
            break;
        }
        case 2: { /* truncate + flip */
            unsigned k, flips;
            if (len > 0) {
                len = (size_t)(weft_prng(&rng) % len);
            }
            flips = 1u + (unsigned)(weft_prng(&rng) % 3);
            for (k = 0; k < flips && len > 0; ++k) {
                size_t pos = (size_t)(weft_prng(&rng) % len);
                scratch[pos] ^= (uint8_t)(weft_prng(&rng) >> 44);
            }
            break;
        }
        case 3: /* random type byte */
            if (len > 0) {
                scratch[0] = (uint8_t)(weft_prng(&rng) >> 24);
                if (target == 1 || target == 2) {
                    /* also randomize the length prefix */
                    scratch[1] = (uint8_t)(weft_prng(&rng) >> 32);
                    scratch[0] = (uint8_t)(weft_prng(&rng) >> 16);
                }
            }
            break;
        case 4: /* pure random garbage */
            len = (size_t)(weft_prng(&rng) % 161);
            {
                size_t j;
                for (j = 0; j < len; ++j) {
                    scratch[j] = (uint8_t)(weft_prng(&rng) >> 32);
                }
            }
            break;
        default: /* intact (baseline: must decode OK) */
            break;
        }

        /* decode through the matching surface; the intact-baseline
         * invariant (must decode OK end-to-end) only applies when the
         * input was NOT mutated. */
        switch (target) {
        case 0: {
            size_t cnt = weft_itch50_decode_batch(scratch, len, 0u,
                                                  ibatch, 32, &st,
                                                  &consumed);
            CHECK(cnt <= 32);                                   /* F4 */
            if (intact) { CHECK_I64(st, WEFT_ADAPTER_OK); }
            break;
        }
        case 1: {
            size_t cnt = weft_itch50_decode_batch(
                scratch, len, WEFT_ITCH_F_LENPREFIX, ibatch, 32,
                &st, &consumed);
            CHECK(cnt <= 32);
            if (intact) { CHECK_I64(st, WEFT_ADAPTER_OK); }
            break;
        }
        case 2: {
            size_t cnt = weft_ouch50_decode_batch(scratch, len, obatch, 32,
                                                  &st, &consumed);
            CHECK(cnt <= 32);
            if (intact) { CHECK_I64(st, WEFT_ADAPTER_OK); }
            break;
        }
        case 3: {
            weft_sbe_view_t v;
            st = weft_sbe_decode(scratch, len, weft_sbe_schema_md(), &v);
            if (intact && st == WEFT_ADAPTER_OK) {
                CHECK_U64X(v.total_size, (uint64_t)n_sbe);
            }
            consumed = (st == WEFT_ADAPTER_OK) ? v.total_size : 0;
            break;
        }
        case 4: {
            weft_waf_view_t wv;
            st = weft_adapter_waf_scan(scratch, len, &wv);
            if (intact && st != WEFT_ADAPTER_EALIGN) {
                /* intact 4-aligned input must verify */
                CHECK_I64(st, WEFT_ADAPTER_OK);
            }
            consumed = (st == WEFT_ADAPTER_OK)
                       ? 12u + wv.payload_len : 0;
            break;
        }
        default: {
            weft_mold_view_t mv;
            st = weft_mold64_open(scratch, len, &mv);
            if (st == WEFT_ADAPTER_OK) {
                const uint8_t *msg = NULL;
                uint16_t mlen = 0;
                consumed = 16u;
                for (;;) {
                    int32_t r = weft_mold64_next(&mv, &msg, &mlen);
                    if (r <= 0) {
                        if (r < 0) { st = r; }
                        break;
                    }
                    consumed += 2u + mlen;
                }
                if (intact) { CHECK_I64(st, WEFT_ADAPTER_OK); }
            }
            break;
        }
        }

        /* F2: legal status code */
        CHECK(st >= -8 && st <= 0);
        /* F3: consumed within the buffer */
        CHECK(consumed <= len);

        ++stat[target].hist[status_slot(st)];
        if (st == WEFT_ADAPTER_OK) { ++stat[target].ok; }
        else                       { ++stat[target].err; }

        /* F5: determinism spot checks - the decoder must be a pure
         * function of (input, framing, zeroed-output-state): decode
         * twice into independently zeroed batches and memcmp. */
        if ((c & 1023u) == 0u && target <= 2) {
            int32_t st2 = WEFT_ADAPTER_OK;
            int32_t st3 = WEFT_ADAPTER_OK;
            size_t consumed2 = 0;
            size_t consumed3 = 0;
            if (target == 2) {
                WEFT_ALIGNED64 static weft_ouch_msg_t obatch2[32];
                memset(obatch, 0, sizeof obatch);
                memset(obatch2, 0, sizeof obatch2);
                (void)weft_ouch50_decode_batch(scratch, len, obatch, 32,
                                               &st2, &consumed2);
                (void)weft_ouch50_decode_batch(scratch, len, obatch2, 32,
                                               &st3, &consumed3);
                CHECK_I64(st3, st2);
                CHECK_U64X(consumed3, consumed2);
                if (st2 == WEFT_ADAPTER_OK) {
                    CHECK(memcmp(obatch, obatch2, sizeof obatch) == 0);
                }
            } else {
                uint32_t fl = (target == 1) ? WEFT_ITCH_F_LENPREFIX : 0u;
                memset(ibatch, 0, sizeof ibatch);
                memset(ibatch2, 0, sizeof ibatch2);
                (void)weft_itch50_decode_batch(scratch, len, fl,
                                               ibatch, 32, &st2, &consumed2);
                (void)weft_itch50_decode_batch(scratch, len, fl,
                                               ibatch2, 32, &st3, &consumed3);
                CHECK_I64(st3, st2);
                CHECK_U64X(consumed3, consumed2);
                if (st2 == WEFT_ADAPTER_OK) {
                    CHECK(memcmp(ibatch, ibatch2, sizeof ibatch) == 0);
                }
            }
        }
    }

#if defined(WEFT_HAVE_MALLINFO2)
    {
        struct mallinfo2 mi = mallinfo2();
        heap_after = (long long)mi.uordblks + (long long)mi.hblkhd;
    }
    CHECK_I64(heap_after - heap_before, 0);   /* F6 */
#endif
#if defined(WEFT_ENABLE_MALLOC_PROBE)
    CHECK_I64(g_probe_alloc_events, 0);       /* F7 */
    CHECK_I64(g_probe_free_events, 0);
#endif

    /* report */
    {
        static const char *names[6] = {
            "itch-fixed", "itch-lenprefix", "ouch50",
            "sbe-md", "waf-frame", "mold64"
        };
        unsigned t;
        printf("FUZZ cycles=%lld targets=6\n", (long long)cycles);
        for (t = 0; t < 6; ++t) {
            printf("FUZZ %-14s ok=%-12llu err=%-12llu ", names[t],
                   stat[t].ok, stat[t].err);
            {
                int s;
                const char *sep = "hist[";
                for (s = 0; s < 16; ++s) {
                    if (stat[t].hist[s] == 0) { continue; }
                    printf("%s%s=%llu", sep,
                           s == 0 ? "OK" : status_name((int32_t)(s == 15 ? 99 : 1 - s)),
                           stat[t].hist[s]);
                    sep = " ";
                }
                printf("]\n");
            }
        }
#if defined(WEFT_HAVE_MALLINFO2)
        printf("FUZZ heap-delta-bytes=%lld\n", heap_after - heap_before);
#endif
#if defined(WEFT_ENABLE_MALLOC_PROBE)
        printf("FUZZ malloc-events=%ld free-events=%ld\n",
               g_probe_alloc_events, g_probe_free_events);
#endif
    }

    REPORT_AND_EXIT("fuzz_torture");
}
