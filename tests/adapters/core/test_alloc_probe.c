/*
 * test_alloc_probe.c - Pillar 6 Law 1 proof: EXACTLY ZERO heap
 * allocation events during ingestion across every decode surface.
 *
 * Built with -DWEFT_ENABLE_MALLOC_PROBE and linked against the
 * interposer in test_alloc_intercept.h (plain build, no sanitizers).
 * Each window resets the event counters, hammers the surface, then
 * asserts zero events AND zero frees.
 */
#include "test_util.h"
#include "test_alloc_intercept.h"

int main(void)
{
    static uint8_t itch_fixed[4096];
    static uint8_t itch_len[4096];
    static uint8_t ouch_stream[4096];
    static uint8_t sbe_stream[512];
    static uint8_t mold_pkt[4096];
    static WEFT_ALIGNED64 uint8_t waf_frame[8192];
    static WEFT_ALIGNED64 uint8_t payload[512];
    WEFT_ALIGNED64 static weft_itch_msg_t ibatch[32];
    WEFT_ALIGNED64 static weft_ouch_msg_t obatch[32];
    WEFT_ALIGNED64 static weft_itch_msg_t proj[8];

    static const sbe_golden_entry_t ent[] = {
        { 123450000ull, 0x0100000000000001ull, 300u, 'B', 'A' },
        { 99999999ull,  0x0100000000000002ull, 200u, 'S', 'A' }
    };

    size_t n_fixed, n_len, n_ouch, n_sbe, n_mold;
    long cycles = weft_env_cycles("WEFT_PROBE_CYCLES", 200000L);
    long c;
    int32_t st;
    size_t consumed;

    n_fixed = itch_golden_build_fixed(itch_fixed, sizeof itch_fixed);
    n_len = itch_golden_build_lenprefixed(itch_len, sizeof itch_len);
    n_ouch = ouch_golden_build(ouch_stream, sizeof ouch_stream);
    n_sbe = sbe_md_build(sbe_stream, sizeof sbe_stream, GOLDEN_TS_BASE,
                         0x00C0FFEEu, 1u, 0x5Au, ent, 2, "AAPL");
    n_mold = mold_build_packet(mold_pkt, sizeof mold_pkt, "WEFTGOLDEN", 1u,
                               g_golden_itch, 9);
    memcpy(payload, itch_fixed, 256);
    st = weft_adapter_waf_emit(waf_frame, sizeof waf_frame, payload, 256u);
    CHECK(st == 268);

    /* -------- window 1: ITCH fixed framing -------- */
    probe_reset();
    for (c = 0; c < cycles; ++c) {
        size_t cnt = weft_itch50_decode_batch(itch_fixed, n_fixed, 0u,
                                              ibatch, 32, &st, &consumed);
        CHECK(cnt == GOLDEN_ITCH_COUNT);
        (void)cnt;
    }
    CHECK_I64(g_probe_alloc_events, 0);
    CHECK_I64(g_probe_free_events, 0);
    printf("PROBE itch-fixed       cycles=%ld alloc-events=0 free-events=0 PASS\n",
           cycles);

    /* -------- window 2: ITCH length-prefix framing -------- */
    probe_reset();
    for (c = 0; c < cycles; ++c) {
        (void)weft_itch50_decode_batch(itch_len, n_len,
                                       WEFT_ITCH_F_LENPREFIX,
                                       ibatch, 32, &st, &consumed);
    }
    CHECK_I64(g_probe_alloc_events, 0);
    CHECK_I64(g_probe_free_events, 0);
    printf("PROBE itch-lenprefix   cycles=%ld alloc-events=0 free-events=0 PASS\n",
           cycles);

    /* -------- window 3: OUCH -------- */
    probe_reset();
    for (c = 0; c < cycles; ++c) {
        (void)weft_ouch50_decode_batch(ouch_stream, n_ouch, obatch, 32,
                                       &st, &consumed);
    }
    CHECK_I64(g_probe_alloc_events, 0);
    CHECK_I64(g_probe_free_events, 0);
    printf("PROBE ouch50           cycles=%ld alloc-events=0 free-events=0 PASS\n",
           cycles);

    /* -------- window 4: SBE decode + projection -------- */
    probe_reset();
    for (c = 0; c < cycles; ++c) {
        weft_sbe_view_t v;
        int32_t s2 = weft_sbe_decode(sbe_stream, n_sbe,
                                     weft_sbe_schema_md(), &v);
        if (s2 == WEFT_ADAPTER_OK) {
            int32_t pst = 0;
            (void)weft_sbe_md_to_itch_add(&v, proj, 8, &pst);
        }
    }
    CHECK_I64(g_probe_alloc_events, 0);
    CHECK_I64(g_probe_free_events, 0);
    printf("PROBE sbe-md+proj      cycles=%ld alloc-events=0 free-events=0 PASS\n",
           cycles);

    /* -------- window 5: WAF scan + emit -------- */
    probe_reset();
    for (c = 0; c < cycles; ++c) {
        weft_waf_view_t wv;
        (void)weft_adapter_waf_scan(waf_frame, 268u, &wv);
        (void)weft_adapter_waf_emit(waf_frame, sizeof waf_frame,
                                    payload, 256u);
    }
    CHECK_I64(g_probe_alloc_events, 0);
    CHECK_I64(g_probe_free_events, 0);
    printf("PROBE waf-scan+emit    cycles=%ld alloc-events=0 free-events=0 PASS\n",
           cycles);

    /* -------- window 6: MoldUDP64 walker -------- */
    probe_reset();
    for (c = 0; c < cycles; ++c) {
        weft_mold_view_t mv;
        if (weft_mold64_open(mold_pkt, n_mold, &mv) == WEFT_ADAPTER_OK) {
            const uint8_t *msg = NULL;
            uint16_t mlen = 0;
            while (weft_mold64_next(&mv, &msg, &mlen) > 0) {
                size_t sz = 0;
                (void)weft_itch50_decode_one(msg, mlen,
                                             WEFT_ITCH_F_LENPREFIX,
                                             &ibatch[0], &sz);
            }
        }
    }
    CHECK_I64(g_probe_alloc_events, 0);
    CHECK_I64(g_probe_free_events, 0);
    printf("PROBE mold64-walker    cycles=%ld alloc-events=0 free-events=0 PASS\n",
           cycles);

    /* -------- window 7: checksum kernels -------- */
    probe_reset();
    for (c = 0; c < cycles; ++c) {
        (void)weft_adapter_crc32c(itch_fixed, n_fixed);
        (void)weft_adapter_crc32c_sw(itch_fixed, n_fixed);
        (void)weft_adapter_adler32(itch_fixed, n_fixed);
        (void)weft_adapter_adler32_portable(1u, itch_fixed, n_fixed);
    }
    CHECK_I64(g_probe_alloc_events, 0);
    CHECK_I64(g_probe_free_events, 0);
    printf("PROBE checksum-kernels cycles=%ld alloc-events=0 free-events=0 PASS\n",
           cycles);

    REPORT_AND_EXIT("alloc_probe");
}
